#include "lua_packages.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <set>

namespace crml::engine::lua {
namespace {
namespace fs=std::filesystem;
bool reparse(const fs::path& path) {
    const auto attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0;
}
struct Read {std::string source;const char* error{};bool missing{};};
Read read_source(const fs::path& path) {
    std::error_code ec;
    const auto status=fs::symlink_status(path,ec);
    if(status.type()==fs::file_type::not_found) return {{},nullptr,true};
    if(ec) return {{},"read_error"};
    if(!fs::is_regular_file(status) || reparse(path)) return {{},"unsupported_source_file"};
    const auto size=fs::file_size(path,ec);
    if(ec) return {{},"read_error"};
    if(size>max_source_bytes) return {{},"source_too_large"};
    std::ifstream file(path,std::ios::binary);
    if(!file) return {{},"read_error"};
    std::string source(static_cast<size_t>(size),'\0');
    file.read(source.data(),static_cast<std::streamsize>(source.size()));
    if(file.gcount()!=static_cast<std::streamsize>(source.size()) || file.bad()) return {{},"source_changed"};
    char extra;
    if(file.get(extra)) return {{},"source_changed"};
    return {std::move(source)};
}
}
SourcePackages::SourcePackages(std::filesystem::path root,ControllerHost& host,Compiler compiler)
    :root_(std::move(root)),host_(host),compile_(compiler) {}
std::vector<PackageEvent> SourcePackages::poll(bool enabled) {
    std::vector<PackageEvent> events;
    const auto scan_error=[&](const char* error) {
        if(scan_error_!=error) events.push_back({{},error});
        scan_error_=error;
        return events;
    };
    std::vector<std::pair<std::string,fs::path>> folders;
    if(enabled) {
        std::error_code ec;
        const auto status=fs::symlink_status(root_,ec);
        if(status.type()!=fs::file_type::not_found) {
            if(ec || !fs::is_directory(status) || reparse(root_)) return scan_error("package_root_unavailable");
            fs::directory_iterator at(root_,ec),end;
            if(ec) return scan_error("package_scan_failed");
            size_t count=0;
            for(;at!=end;at.increment(ec)) {
                if(ec) return scan_error("package_scan_failed");
                if(++count>64) return scan_error("package_scan_limit");
                const auto name=at->path().filename().native();
                if(name.size()>64 || std::any_of(name.begin(),name.end(),[](wchar_t c){return c>127;})) continue;
                std::string id;id.reserve(name.size());
                for(const auto c:name) id.push_back(static_cast<char>(c));
                if(!valid_controller_id(id)) continue;
                const auto entry_status=at->symlink_status(ec);
                if(ec) return scan_error("package_scan_failed");
                if(fs::is_directory(entry_status) || reparse(at->path())) folders.emplace_back(id,at->path());
                if(folders.size()>ControllerHost::capacity) return scan_error("package_count_limit");
            }
            if(ec) return scan_error("package_scan_failed");
        }
    }
    scan_error_=nullptr;
    std::sort(folders.begin(),folders.end());
    std::set<std::string> present;
    for(const auto& [id,path]:folders) {
        present.insert(id);
        if(reparse(path)) {events.push_back({id,"unsupported_package_path"});continue;}
        std::error_code ec;
        const bool disabled=fs::exists(path/"disabled",ec);
        if(ec) {events.push_back({id,"read_error"});continue;}
        auto read=disabled?Read{{},nullptr,true}:read_source(path/"main.luau");
        if(read.missing) {present.erase(id);continue;}
        if(read.error) {events.push_back({id,read.error});continue;}
        auto& record=records_[id];
        if(record.seen && !record.retry && record.observed==read.source) continue;
        record.observed=read.source;record.seen=true;record.retry=false;
        auto compiled=compile_(read.source);
        if(!compiled) {events.push_back({id,compiled.error?compiled.error:"compiler_failure",compiled.line});continue;}
        // Compile a copied snapshot, then reject it if an editor replaced the
        // source during compilation. This is not an adversarial filesystem sandbox.
        const auto after=read_source(path/"main.luau");
        if(after.error || after.missing || after.source!=read.source) {
            record.retry=true;events.push_back({id,"source_changed"});continue;
        }
        if(!host_.submit(id,std::move(compiled.bytes))) {record.retry=true;events.push_back({id,"submission_refused"});continue;}
        record.queued=true;events.push_back({id,"revision_queued"});
    }
    for(auto at=records_.begin();at!=records_.end();) {
        if(!present.contains(at->first)) {
            if(at->second.queued) {host_.unload(at->first);events.push_back({at->first,"unload_queued"});}
            at=records_.erase(at);
        } else ++at;
    }
    return events;
}
}
