#include "mod_storage.h"
#include <Windows.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace crml {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value(other.value){other.value=INVALID_HANDLE_VALUE;}
    explicit operator bool() const {return value!=INVALID_HANDLE_VALUE;}
};
bool regular(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(file,&info) &&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) && info.nNumberOfLinks==1;
}
uint32_t checksum(std::span<const unsigned char> bytes) {
    uint32_t value=2166136261u;
    for(auto b:bytes) value=(value^b)*16777619u;
    return value;
}
bool valid_id(std::string_view id) {
    return !id.empty() && id.size()<=64 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")==id.npos;
}
}
struct ModStorage::Impl {
    struct Record {
        std::vector<unsigned char> committed, pending;
        int read_result{-2}, state{};
        std::chrono::steady_clock::time_point next_write{};
    };
    std::filesystem::path root;
    std::vector<Handle> pins;
    std::unique_ptr<Handle> exclusive;
    std::map<std::string,Record,std::less<>> records;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool ready{}, stopping{};
    explicit Impl(const std::filesystem::path& requested) {
        if(requested.empty()) return;
        root=std::filesystem::absolute(requested).lexically_normal();
        // Pin every directory without following junctions/symlinks, and deny
        // rename/deletion while files are accessed. Only create the final leaf.
        auto part=root.root_path();
        auto pin=[&](const std::filesystem::path& path) {
            Handle h(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
                nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if(!h || !GetFileInformationByHandle(h.value,&info) ||
               !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
            pins.push_back(std::move(h));return true;
        };
        if(!pin(part)) return;
        for(const auto& component:root.relative_path()) {
            part/=component;
            if(part==root && !CreateDirectoryW(part.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) return;
            if(!pin(part)) return;
        }
        exclusive=std::make_unique<Handle>(CreateFileW((root/L"storage.lock").c_str(),GENERIC_READ,0,nullptr,
            OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!*exclusive || !regular(exclusive->value)) return;
        ready=true;
        worker=std::thread([this]{run();});
    }
    ~Impl() {
        {std::lock_guard lock(mutex);stopping=true;}
        wake.notify_one();
        if(worker.joinable()) worker.join();
    }
    std::filesystem::path filename(std::string_view id) const {return root/("mod-"+std::string(id)+".bin");}
    void load(std::string_view id,Record& record) {
        Handle file(CreateFileW(filename(id).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!file) {record.read_result=GetLastError()==ERROR_FILE_NOT_FOUND?-2:-5;return;}
        record.read_result=-5;
        LARGE_INTEGER size{};
        if(!regular(file.value) || !GetFileSizeEx(file.value,&size) || size.QuadPart<16 || size.QuadPart>16+limit) return;
        std::vector<unsigned char> data(static_cast<size_t>(size.QuadPart));
        DWORD count{};
        if(!ReadFile(file.value,data.data(),static_cast<DWORD>(data.size()),&count,nullptr) || count!=data.size()) return;
        uint32_t length{},hash{};
        std::memcpy(&length,data.data()+8,4);std::memcpy(&hash,data.data()+12,4);
        if(std::memcmp(data.data(),"CRMLDB01",8) || length!=data.size()-16 || checksum(std::span(data).subspan(16))!=hash) return;
        record.committed.assign(data.begin()+16,data.end());
        record.read_result=static_cast<int>(length);
    }
    bool save(std::string_view id,std::span<const unsigned char> bytes) {
        const auto target=filename(id);
        const auto temporary=root/("mod-"+std::string(id)+".pending");
        // One staging file per ID, including after interruption. The exclusive
        // root lock guarantees it cannot belong to another active store.
        {
            Handle stale(CreateFileW(temporary.c_str(),DELETE|FILE_READ_ATTRIBUTES,0,nullptr,OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(stale) {
                FILE_DISPOSITION_INFO remove{TRUE};
                if(!regular(stale.value) || !SetFileInformationByHandle(stale.value,FileDispositionInfo,&remove,sizeof(remove))) return false;
            } else if(GetLastError()!=ERROR_FILE_NOT_FOUND) return false;
        }
        // CREATE_NEW never truncates an existing file, hardlink or reparse point.
        bool written=false;
        {
            Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
            if(!file) return false;
            std::array<unsigned char,16> header{};
            std::memcpy(header.data(),"CRMLDB01",8);
            const uint32_t length=static_cast<uint32_t>(bytes.size()),hash=checksum(bytes);
            std::memcpy(header.data()+8,&length,4);std::memcpy(header.data()+12,&hash,4);
            DWORD count{};
            written=WriteFile(file.value,header.data(),16,&count,nullptr) && count==16;
            if(written && !bytes.empty()) written=WriteFile(file.value,bytes.data(),length,&count,nullptr) && count==length;
            if(written) written=FlushFileBuffers(file.value)!=0;
        }
        // Replacing the directory entry never writes through a target hardlink.
        if(written) written=MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
        if(!written) DeleteFileW(temporary.c_str());
        return written;
    }
    void run() noexcept {
        std::unique_lock lock(mutex);
        for(;;) {
            wake.wait(lock,[&]{if(stopping)return true;for(const auto& [id,r]:records)if(r.state==1)return true;return false;});
            auto found=records.begin();
            for(;found!=records.end() && found->second.state!=1;++found){}
            if(found==records.end()) {if(stopping)return;continue;}
            auto& r=found->second;
            // Map nodes and pending bytes remain stable while state is pending.
            lock.unlock();
            bool saved=false;
            try {saved=save(found->first,r.pending);} catch(...) {}
            lock.lock();
            if(saved) {r.committed=std::move(r.pending);r.read_result=static_cast<int>(r.committed.size());r.state=2;}
            else {r.pending.clear();r.state=-5;}
        }
    }
};
ModStorage::ModStorage(const std::filesystem::path& root):impl_(std::make_unique<Impl>(root)){}
ModStorage::~ModStorage()=default;
bool ModStorage::available() const noexcept{return impl_->ready;}
void ModStorage::attach(std::string_view id) {
    if(!available() || !valid_id(id)) return;
    std::lock_guard lock(impl_->mutex);
    if(impl_->records.contains(id) || impl_->records.size()>=32) return;
    auto& record=impl_->records[std::string(id)];impl_->load(id,record);
}
int ModStorage::read(std::string_view id,std::span<unsigned char> output) {
    std::lock_guard lock(impl_->mutex);
    const auto it=impl_->records.find(id);if(it==impl_->records.end())return -1;
    const auto& r=it->second;if(r.read_result<0)return r.read_result;
    if(output.size()<r.committed.size())return -3;
    std::copy(r.committed.begin(),r.committed.end(),output.begin());return r.read_result;
}
int ModStorage::write(std::string_view id,std::span<const unsigned char> bytes) {
    std::lock_guard lock(impl_->mutex);
    const auto it=impl_->records.find(id);if(it==impl_->records.end() || impl_->stopping)return -1;
    auto& r=it->second;
    if(bytes.size()>limit)return -3;
    const auto now=std::chrono::steady_clock::now();
    if(r.state==1 || now<r.next_write)return -4;
    r.pending.assign(bytes.begin(),bytes.end());r.state=1;r.next_write=now+std::chrono::seconds(1);
    impl_->wake.notify_one();return 0;
}
int ModStorage::status(std::string_view id) {
    std::lock_guard lock(impl_->mutex);
    const auto it=impl_->records.find(id);return it==impl_->records.end()?-1:it->second.state;
}
}
