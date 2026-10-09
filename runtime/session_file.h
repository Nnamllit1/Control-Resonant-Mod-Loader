#pragma once
#include <Windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <ostream>
#include <streambuf>
#include <string_view>

namespace crml {
// Single-writer session history. Readers can inspect the current log while the
// runtime runs. No guest chooses these paths. A blocked rotation preserves the
// current file and appends within the same size bound instead of truncating it.
class SessionFile {
    struct Handle {
        HANDLE value{INVALID_HANDLE_VALUE};
        ~Handle() { if(value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    } directory_, lock_, file_;
    class Buffer : public std::streambuf {
    public:
        HANDLE file{INVALID_HANDLE_VALUE};
        uint64_t bytes{};
        static constexpr uint64_t limit=4*1024*1024;
    private:
        bool capped_{};
        bool write(const char* data,size_t count) {
            DWORD written{};
            if(!WriteFile(file,data,static_cast<DWORD>(count),&written,nullptr) || written!=count)return false;
            bytes+=written;return true;
        }
        std::streamsize xsputn(const char* data,std::streamsize count) override {
            if(count<=0)return 0;
            if(file==INVALID_HANDLE_VALUE)return 0;
            if(capped_)return count;
            constexpr std::string_view notice="\n[host] Session file size limit reached; later output is suppressed.\n";
            const auto room=bytes<limit-notice.size()?limit-notice.size()-bytes:0;
            const auto accepted=static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(count),room));
            if(accepted && !write(data,accepted))return 0;
            if(accepted<static_cast<uint64_t>(count)) {
                capped_=true;
                if(bytes<=limit-notice.size() && !write(notice.data(),notice.size()))return 0;
            }
            return count;
        }
        int_type overflow(int_type c) override {
            if(traits_type::eq_int_type(c,traits_type::eof()))return traits_type::not_eof(c);
            const auto byte=traits_type::to_char_type(c);
            return xsputn(&byte,1)==1?c:traits_type::eof();
        }
    } buffer_;
    std::ostream stream_{&buffer_};
    bool rotated_{};
    static bool regular(HANDLE handle) {
        BY_HANDLE_FILE_INFORMATION info{};
        return handle!=INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle,&info) &&
            !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) && info.nNumberOfLinks==1;
    }
    static bool rotate(const std::filesystem::path& root) {
        const std::array paths{root/L"crml.log",root/L"crml.1.log",root/L"crml.2.log",root/L"crml.3.log"};
        std::array<bool,4> present{};
        std::array<Handle,4> pinned;
        // Inspect every slot before changing any of them. Never follow links or
        // recursively remove a directory when a history slot is obstructed.
        for(size_t i=0;i<paths.size();++i) {
            auto& item=pinned[i];
            // Acquire rename access on ALL existing files first, and keep it
            // while rotating. A reader denying deletion must not cause older
            // sessions to be discarded before current-log renaming fails.
            item.value=CreateFileW(paths[i].c_str(),FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
            if(item.value==INVALID_HANDLE_VALUE) {
                if(GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
            } else {
                FILE_ATTRIBUTE_TAG_INFO attributes{};
                if(!regular(item.value) || !GetFileInformationByHandleEx(item.value,FileAttributeTagInfo,&attributes,sizeof(attributes)) ||
                    (attributes.FileAttributes&FILE_ATTRIBUTE_READONLY))return false;
                present[i]=true;
            }
        }
        if(!present[0])return true;
        // Classic Win32 replacement cannot replace an open destination even
        // when it shares deletion. Only the oldest slot is overwritten; the
        // other handles remain pinned as their files move into vacated slots.
        if(pinned.back().value!=INVALID_HANDLE_VALUE) {
            CloseHandle(pinned.back().value);pinned.back().value=INVALID_HANDLE_VALUE;
        }
        for(size_t i=paths.size()-1;i>0;--i) {
            if(present[i-1] && !MoveFileExW(paths[i-1].c_str(),paths[i].c_str(),MOVEFILE_REPLACE_EXISTING))return false;
        }
        return true;
    }
public:
    explicit SessionFile(const std::filesystem::path& root) {
        directory_.value=CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if(directory_.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(directory_.value,FileAttributeTagInfo,&attributes,sizeof(attributes)) ||
            !(attributes.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (attributes.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return;
        lock_.value=CreateFileW((root/L"crml-log.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(!regular(lock_.value))return;
        rotated_=rotate(root);
        file_.value=CreateFileW((root/L"crml.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,
            rotated_?CREATE_NEW:OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(!regular(file_.value))return;
        LARGE_INTEGER end{};
        if(!SetFilePointerEx(file_.value,{},&end,FILE_END) || end.QuadPart<0)return;
        buffer_.file=file_.value;buffer_.bytes=static_cast<uint64_t>(end.QuadPart);
        if(!rotated_)stream_<<"\n[host] Log history could not rotate; continuing this file without truncation.\n";
    }
    SessionFile(const SessionFile&)=delete;
    SessionFile& operator=(const SessionFile&)=delete;
    bool available() const {return buffer_.file!=INVALID_HANDLE_VALUE;}
    bool rotated() const {return rotated_;}
    std::ostream& stream() {return stream_;}
    static constexpr uint64_t limit=Buffer::limit;
};
}
