#pragma once
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <cstdint>
#include <utility>

namespace crml {
// One host thread owns the sink. Guest quotas are per private mod identity, not
// inferred from message prefixes, so guest output cannot spend host capacity.
class SessionLog {
public:
    using Clock = std::function<uint64_t()>;
    explicit SessionLog(std::ostream& stream, Clock clock = [] {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }) : stream_(stream), clock_(std::move(clock)), started_(clock_()) {}
    void host(std::string_view text) {
        auto line=stamp()+"[host] "+single_line(text,16384)+'\n';
        if(host_bytes_+line.size()>host_limit) {
            if(!host_full_) {host_full_=true;stream_<<"[host] Host diagnostic limit reached.\n";stream_.flush();}
            return;
        }
        host_bytes_+=line.size();stream_<<line;stream_.flush();
    }
    void guest(std::string_view id,int level,std::string_view text) {
        // Runtime validates package IDs and level. Retain bounds for other
        // native consumers; these inputs never choose a file or logger class.
        if(id.empty() || id.size()>64 || level<0 || level>3) return;
        auto found=guests_.find(std::string(id));
        if(found==guests_.end()) {
            if(guests_.size()>=32) return;
            found=guests_.emplace(std::string(id),Guest{}).first;
        }
        auto& quota=found->second;
        if(quota.full) return;
        constexpr const char* levels[]{"debug","info","warning","error"};
        auto line=stamp()+"["+single_line(id,64)+"] ["+levels[level]+"] "+single_line(text,4096)+'\n';
        if(quota.bytes+line.size()>guest_limit) {
            if(!quota.full) {quota.full=true;host("Guest log limit reached for "+std::string(id)+"; later messages from this mod are suppressed.");}
            return;
        }
        quota.bytes+=line.size();stream_<<line;stream_.flush();
    }
    static constexpr size_t guest_limit=64*1024, host_limit=1024*1024;
private:
    struct Guest {size_t bytes{};bool full{};};
    std::ostream& stream_;
    Clock clock_;
    uint64_t started_;
    size_t host_bytes_{};
    bool host_full_{};
    std::map<std::string,Guest> guests_;
    static std::string single_line(std::string_view text,size_t limit) {
        std::string result(text.substr(0,limit));
        for(auto& c:result) if(static_cast<unsigned char>(c)<32 || c==127) c=' ';
        return result;
    }
    std::string stamp() const {
        const auto now=clock_();return "[+"+std::to_string(now>=started_?now-started_:0)+"ms] ";
    }
};
}
