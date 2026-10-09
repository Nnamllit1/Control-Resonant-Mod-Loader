#include "mod_feedback.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <mutex>

namespace crml {
namespace {
bool active(int status) {return status==CRML_FEEDBACK_QUEUED || status==CRML_FEEDBACK_PRESENTED;}
bool valid_text(std::string_view value) {
    if(value.empty() || value.size()>CRML_FEEDBACK_TEXT_MAX)return false;
    for(auto c:value)if(static_cast<unsigned char>(c)<32 || c==127)return false;
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0)>0;
}
bool number(std::string_view text,uint64_t& out) {
    const auto result=std::from_chars(text.data(),text.data()+text.size(),out);
    return !text.empty() && out && result.ec==std::errc{} && result.ptr==text.data()+text.size();
}
void quote(std::string& output,std::string_view text) {
    output+='"';for(const auto c:text) {if(c=='"' || c=='\\')output+='\\';output+=c;}output+='"';
}
}
struct ModFeedback::Impl {
    struct Entry {
        std::string id,name,text;
        uint64_t ticket{},deadline{},last_show{};
        uint32_t severity{};
        int status{};
    };
    std::mutex mutex;
    Clock clock;
    std::map<uint64_t,Entry> owners;
    std::array<uint64_t,4> offered{};
    uint64_t page{},sequence{},next_ticket{1};
    bool enabled{};
    explicit Impl(Clock source):clock(source?std::move(source):Clock([]{return GetTickCount64();})){}
    void expire(uint64_t now) {
        for(auto& [owner,item]:owners)if(active(item.status) && now>=item.deadline) {
            item.status=item.status==CRML_FEEDBACK_PRESENTED?CRML_FEEDBACK_EXPIRED_PRESENTED:CRML_FEEDBACK_EXPIRED_UNPRESENTED;
            item.text.clear();
        }
    }
};
ModFeedback::ModFeedback(Clock clock):impl_(std::make_unique<Impl>(std::move(clock))){}
ModFeedback::~ModFeedback()=default;
bool ModFeedback::attach(uint64_t owner,std::string_view id,const ModMetadata& metadata) {
    if(!owner || id.empty() || id.size()>64 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=id.npos || !metadata.valid())return false;
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(s.owners.size()>=32 || s.owners.contains(owner))return false;
    for(const auto& [key,entry]:s.owners)if(entry.id==id)return false;
    Impl::Entry entry;entry.id=id;entry.name=metadata.name.empty()?std::string(id):metadata.name;
    s.owners.emplace(owner,std::move(entry));return true;
}
void ModFeedback::detach(uint64_t owner) {std::lock_guard lock(impl_->mutex);impl_->owners.erase(owner);}
void ModFeedback::cancel(uint64_t owner) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);s.expire(s.clock());
    const auto found=s.owners.find(owner);
    if(found!=s.owners.end() && active(found->second.status)) {
        found->second.status=CRML_FEEDBACK_CANCELLED;found->second.text.clear();
    }
}
bool ModFeedback::available() {std::lock_guard lock(impl_->mutex);return impl_->enabled;}
void ModFeedback::enable_renderer(bool enabled) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(!enabled) {
        if(s.page!=UINT64_MAX)++s.page;
        s.offered={};s.sequence=0;
        for(auto& [owner,item]:s.owners)if(active(item.status)) {item.status=CRML_FEEDBACK_CANCELLED;item.text.clear();}
    }
    s.enabled=enabled;
}
uint64_t ModFeedback::open_page() {
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(!s.enabled || s.page==UINT64_MAX)return 0;
    s.offered={};s.sequence=0;return ++s.page;
}
int64_t ModFeedback::show(uint64_t owner,std::string_view text,uint32_t severity,uint32_t duration) {
    if(!valid_text(text) || severity>CRML_FEEDBACK_ERROR || duration<1000 || duration>10000)return -3;
    auto& s=*impl_;std::lock_guard lock(s.mutex);const auto now=s.clock();s.expire(now);
    const auto found=s.owners.find(owner);if(!s.enabled || found==s.owners.end())return -1;
    auto& item=found->second;
    if(active(item.status) || std::count_if(s.owners.begin(),s.owners.end(),[](const auto& pair){return active(pair.second.status);})>=4)return -2;
    if(item.ticket && (now<item.last_show || now-item.last_show<1000))return -4;
    if(s.next_ticket>INT64_MAX || now>UINT64_MAX-duration)return -5;
    std::string copied(text);item.text=std::move(copied);item.ticket=s.next_ticket++;
    item.last_show=now;item.deadline=now+duration;item.severity=severity;item.status=CRML_FEEDBACK_QUEUED;
    return static_cast<int64_t>(item.ticket);
}
int ModFeedback::status(uint64_t owner,uint64_t ticket) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);s.expire(s.clock());
    const auto found=s.owners.find(owner);if(found==s.owners.end())return -1;
    return ticket && found->second.ticket==ticket?found->second.status:-2;
}
int ModFeedback::dismiss(uint64_t owner,uint64_t ticket) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);s.expire(s.clock());
    const auto found=s.owners.find(owner);if(found==s.owners.end())return -1;
    auto& item=found->second;if(!ticket || item.ticket!=ticket)return -2;
    if(!active(item.status))return 0;
    item.status=CRML_FEEDBACK_DISMISSED;item.text.clear();return 1;
}
int ModFeedback::poll(uint64_t page,uint64_t sequence,std::span<const uint64_t> acknowledgements,std::vector<Message>& output) {
    output.clear();if(!page || !sequence || acknowledgements.size()>4)return 400;
    for(size_t i=0;i<acknowledgements.size();++i)if(!acknowledgements[i] ||
        std::find(acknowledgements.begin(),acknowledgements.begin()+i,acknowledgements[i])!=acknowledgements.begin()+i)return 400;
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(!s.enabled)return 503;
    if(page!=s.page)return 403;
    if(sequence<=s.sequence)return 409;
    const auto now=s.clock();s.expire(now);
    std::array<uint64_t,4> offered{};size_t count{};
    for(auto& [owner,item]:s.owners) {
        const bool acknowledged=std::find(acknowledgements.begin(),acknowledgements.end(),item.ticket)!=acknowledgements.end() &&
            std::find(s.offered.begin(),s.offered.end(),item.ticket)!=s.offered.end();
        if(acknowledged) {
            if(active(item.status))item.status=CRML_FEEDBACK_PRESENTED;
            else if(item.status==CRML_FEEDBACK_EXPIRED_UNPRESENTED)item.status=CRML_FEEDBACK_EXPIRED_PRESENTED;
        }
        if(!active(item.status))continue;
        output.push_back({item.ticket,item.id,item.name,item.text,item.severity,static_cast<uint32_t>(item.deadline-now)});
        offered[count++]=item.ticket;
    }
    s.sequence=sequence;s.offered=offered;return 200;
}
int ModFeedback::exchange(std::string_view url,std::string& output) noexcept {
    output.clear();if(!url.starts_with(feedback_prefix))return 0;
    try {
        if(url.size()>256)return 400;
        url.remove_prefix(feedback_prefix.size());
        const auto slash=url.find('/');if(slash==url.npos)return 400;
        uint64_t page{},sequence{};if(!number(url.substr(0,slash),page))return 400;
        url.remove_prefix(slash+1);const auto ack_start=url.find('/');
        if(!number(url.substr(0,ack_start),sequence))return 400;
        std::array<uint64_t,4> acks{};size_t count{};
        if(ack_start!=url.npos) {
            url.remove_prefix(ack_start+1);if(url.empty())return 400;
            while(!url.empty()) {
                const auto comma=url.find(',');
                if(count==acks.size() || !number(url.substr(0,comma),acks[count++]))return 400;
                if(comma==url.npos)break;
                url.remove_prefix(comma+1);if(url.empty())return 400;
            }
        }
        std::vector<Message> messages;const auto code=poll(page,sequence,std::span(acks).first(count),messages);
        if(code!=200)return code;
        output="{\"messages\":[";bool first=true;
        for(const auto& item:messages) {
            if(!first)output+=',';first=false;
            output+="{\"ticket\":\""+std::to_string(item.ticket)+"\",\"id\":";quote(output,item.id);
            output+=",\"name\":";quote(output,item.name);output+=",\"text\":";quote(output,item.text);
            output+=",\"severity\":"+std::to_string(item.severity)+",\"remaining_ms\":"+std::to_string(item.remaining_ms)+"}";
        }
        output+="]}";return 200;
    } catch(...) {output.clear();return 500;}
}
ModFeedback& process_feedback() {static auto* service=new ModFeedback;return *service;}
}
