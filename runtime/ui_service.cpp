#include "ui_service.h"
#include <array>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <limits>

namespace crml::ui {
Service& process_service() {static auto* service=new Service;return *service;}
namespace {
struct Lock {
    SRWLOCK* value;
    explicit Lock(SRWLOCK& lock):value(TryAcquireSRWLockExclusive(&lock)?&lock:nullptr){}
    ~Lock(){if(value) ReleaseSRWLockExclusive(value);}
    explicit operator bool() const {return value!=nullptr;}
};
template<size_t N> bool parse(std::string_view text,std::array<uint64_t,N>& fields) noexcept {
    if(text.empty() || text.size()>128) return false;
    for(size_t i=0;i<fields.size();++i) {
        const auto end=text.find('/');
        auto part=text.substr(0,end);
        if(part.empty() || (part.size()>1 && part.front()=='0')) return false;
        const auto result=std::from_chars(part.data(),part.data()+part.size(),fields[i]);
        if(result.ec!=std::errc{} || result.ptr!=part.data()+part.size()) return false;
        if(i+1==fields.size()) return end==text.npos;
        if(end==text.npos) return false;
        text.remove_prefix(end+1);
    }
    return false;
}
bool can_continue(uint32_t screen) noexcept {
    return screen==CRML_UI_SCREEN_PHOTOSENSITIVITY || screen==CRML_UI_SCREEN_SAVE_WARNING || screen==CRML_UI_SCREEN_USER_INTERACTION;
}
}
void Service::enable(bool value) noexcept {
    AcquireSRWLockExclusive(&lock_);
    if(!value) {sampled_=false;finish_command(CRML_UI_ACTION_CANCELLED);leases_={};}
    enabled_.store(value,std::memory_order_release);
    ReleaseSRWLockExclusive(&lock_);
}
uint64_t Service::open_page() noexcept {
    Lock lock(lock_);
    if(!lock || !enabled_.load() || page_==UINT64_MAX) return 0;
    finish_command(CRML_UI_ACTION_CANCELLED);
    ++page_;sequence_=0;sampled_=false;screen_=0;actions_=0;leases_={};last_ack_=0;last_delivery_=0;
    return page_;
}
uint32_t Service::capabilities() const noexcept {
    return enabled_.load(std::memory_order_acquire)?CRML_CAP_UI_READ|CRML_CAP_UI_ACTIVATE|CRML_CAP_UI_PRESENTATION:0;
}
void Service::expire(uint64_t now) noexcept {
    if(command_.id && (now<command_.queued_at || now-command_.queued_at>2000)) finish_command(CRML_UI_ACTION_EXPIRED);
    for(auto& lease:leases_) if(lease.owner && (now<lease.issued || now>=lease.deadline)) lease={};
}
Service::Receipt* Service::receipt(uint32_t id) noexcept {
    if(id) for(auto& item:receipts_) if(item.id==id) return &item;
    return nullptr;
}
void Service::finish_command(int undelivered) noexcept {
    if(auto* item=receipt(command_.id))
        item->status=command_.delivered?CRML_UI_ACTION_OUTCOME_UNKNOWN:undelivered;
    command_={};
}
int Service::read_at(crml_ui_state& out,uint64_t now) noexcept {
    out={};Lock lock(lock_);
    if(!lock || !enabled_.load() || !sampled_ || now<sampled_at_ || now-sampled_at_>1000) return -1;
    out.size=sizeof(out);out.version=CRML_UI_STATE_VERSION;out.screen=screen_;out.actions=actions_;
    out.generation=generation_;out.age_ms=static_cast<uint32_t>(now-sampled_at_);
    return 1;
}
int Service::ui_read(crml_ui_state& out) noexcept {return read_at(out,GetTickCount64());}
int64_t Service::queue_locked(uint64_t owner,uint64_t generation,uint32_t action,uint64_t now,bool tracked) noexcept {
    if(!enabled_.load() || !sampled_ || now<sampled_at_ || now-sampled_at_>1000) return -1;
    expire(now);
    if(!owner || !generation || generation!=generation_ || action!=CRML_UI_ACTION_CONTINUE ||
       !(actions_&CRML_UI_ACTION_MASK_CONTINUE) || !can_continue(screen_)) return -3;
    if(command_.id) return -2;
    if(next_command_==UINT32_MAX) return -5;
    command_={owner,generation,now,++next_command_,screen_,action,false};
    if(tracked) {
        receipts_[next_receipt_]={owner,page_,command_.id,CRML_UI_ACTION_QUEUED};
        next_receipt_=(next_receipt_+1)%receipts_.size();
    }
    ++submissions_;
    return command_.id;
}
int Service::activate_at(uint64_t owner,uint64_t generation,uint32_t action,uint64_t now) noexcept {
    Lock lock(lock_);
    return lock && queue_locked(owner,generation,action,now,false)>0?0:-1;
}
int64_t Service::submit_at(uint64_t owner,uint64_t generation,uint32_t action,uint64_t now) noexcept {
    Lock lock(lock_);return lock?queue_locked(owner,generation,action,now,true):-1;
}
int Service::status_at(uint64_t owner,uint64_t ticket,uint64_t now) noexcept {
    Lock lock(lock_);if(!lock) return -1;
    expire(now);
    auto* item=ticket<=UINT32_MAX?receipt(static_cast<uint32_t>(ticket)):nullptr;
    return item && owner && item->owner==owner?item->status:-2;
}
int64_t Service::ui_action_submit(uint64_t owner,uint64_t generation,uint32_t action) noexcept {
    return submit_at(owner,generation,action,GetTickCount64());
}
int Service::ui_action_status(uint64_t owner,uint64_t ticket) noexcept {
    return status_at(owner,ticket,GetTickCount64());
}
int Service::ui_activate(uint64_t owner,uint64_t generation,uint32_t action) noexcept {
    return activate_at(owner,generation,action,GetTickCount64());
}
int Service::present_at(uint64_t owner,uint64_t generation,uint32_t kind,std::string_view name,bool hidden,uint32_t duration,uint64_t now) noexcept {
    if(!owner || !generation || (kind!=1 && kind!=2) || name.empty() || name.size()>64 ||
       (hidden?(duration<1 || duration>1000):duration!=0) || now>UINT64_MAX-duration) return -1;
    for(unsigned char c:name) if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-')) return -1;
    Lock lock(lock_);
    if(!lock || !enabled_.load() || !sampled_ || generation!=generation_ || now<sampled_at_ || now-sampled_at_>1000) return -1;
    expire(now);
    Lease* free=nullptr;
    for(auto& lease:leases_) {
        if(!lease.owner) {if(!free) free=&lease;continue;}
        if(lease.kind!=kind || name!=lease.name) continue;
        if(lease.owner!=owner) return -2;
        if(!hidden) {lease={};return 0;}
        lease.issued=now;lease.deadline=now+duration;return 0;
    }
    if(!hidden) return 0;
    if(!free) return -2;
    free->owner=owner;free->issued=now;free->deadline=now+duration;free->kind=kind;free->screen=screen_;
    std::copy(name.begin(),name.end(),free->name);free->name[name.size()]=0;
    return 0;
}
int Service::ui_present(uint64_t owner,uint64_t generation,uint32_t kind,std::string_view name,bool hidden,uint32_t duration) noexcept {
    return present_at(owner,generation,kind,name,hidden,duration,GetTickCount64());
}
void Service::release(uint64_t owner) noexcept {
    // Release runs on the mod host thread. A short exclusive section guarantees
    // that an undelivered command cannot survive mod teardown due to contention.
    AcquireSRWLockExclusive(&lock_);
    if(command_.owner==owner) finish_command(CRML_UI_ACTION_CANCELLED);
    for(auto& lease:leases_) if(lease.owner==owner) lease={};
    ReleaseSRWLockExclusive(&lock_);
}
int Service::exchange(std::string_view url,uint64_t now,std::string& response) noexcept {
    response.clear();
    const bool reports=url.starts_with(result_poll_prefix);
    if(!reports && !url.starts_with(poll_prefix)) return 0;
    std::array<uint64_t,6> fields{};
    bool valid{};
    if(reports) valid=parse(url.substr(result_poll_prefix.size()),fields);
    else {
        std::array<uint64_t,5> legacy{};
        valid=parse(url.substr(poll_prefix.size()),legacy);
        std::copy(legacy.begin(),legacy.end(),fields.begin());
    }
    if(!valid || !fields[0] || !fields[1] || fields[1]>UINT32_MAX ||
       fields[5]>3 || (reports && ((fields[4]==0)!=(fields[5]==0))) ||
       fields[2]>CRML_UI_SCREEN_MAIN_MENU || fields[3]>CRML_UI_ACTION_MASK_CONTINUE || fields[4]>UINT32_MAX) return 400;
    Lock lock(lock_);
    if(!lock || !enabled_.load()) return 503;
    if(fields[0]!=page_ || fields[1]<=sequence_ || fields[4]>last_delivery_) return 409;
    expire(now);
    const auto screen=static_cast<uint32_t>(fields[2]);
    const auto actions=can_continue(screen)?static_cast<uint32_t>(fields[3]):0u;
    // A v1 acknowledgement proves receipt only. A late v2 report may resolve
    // uncertainty even after another command advances the delivery watermark.
    // Definitive outcomes never change on repeated or contradictory reports.
    if(auto* item=receipt(static_cast<uint32_t>(fields[4]));item && item->page==page_ &&
       (item->status==CRML_UI_ACTION_DELIVERED || item->status==CRML_UI_ACTION_OUTCOME_UNKNOWN))
        item->status=!reports?CRML_UI_ACTION_OUTCOME_UNKNOWN:
            fields[5]==1?CRML_UI_ACTION_DISPATCHED:fields[5]==2?CRML_UI_ACTION_SKIPPED:CRML_UI_ACTION_DISPATCH_FAILED;
    if(fields[4]>last_ack_) {
        if(command_.id==fields[4] && command_.delivered) {command_={};++acknowledgements_;}
        last_ack_=static_cast<uint32_t>(fields[4]);
    }
    if(!sampled_ || screen_!=screen) leases_={};
    if(!sampled_ || screen_!=screen || actions_!=actions) {
        if(generation_==UINT64_MAX) return 503;
        ++generation_;finish_command(CRML_UI_ACTION_CANCELLED);
    }
    sampled_=true;sampled_at_=now;screen_=screen;actions_=actions;sequence_=static_cast<uint32_t>(fields[1]);
    ++polls_;
    const bool deliver=command_.id && !command_.delivered && command_.generation==generation_;
    std::array<char,180> json{};
    const auto length=std::snprintf(json.data(),json.size(),"{\"id\":%u,\"generation\":\"%llu\",\"action\":%u,\"screen\":%u,\"leases\":[",
        deliver?command_.id:0u,static_cast<unsigned long long>(generation_),deliver?command_.action:0u,screen_);
    if(length<0 || static_cast<size_t>(length)>=json.size()) return 503;
    try {
        response.assign(json.data(),static_cast<size_t>(length));bool first=true;
        for(const auto& lease:leases_) if(lease.owner && lease.screen==screen_) {
            if(!first) response+=',';
            first=false;
            response+="{\"kind\":"+std::to_string(lease.kind)+",\"name\":\""+lease.name+"\",\"ttl_ms\":"+std::to_string(lease.deadline-now)+"}";
        }
        response+="]}";
    } catch(...) {response.clear();return 503;}
    if(deliver) {
        command_.delivered=true;last_delivery_=command_.id;
        if(auto* item=receipt(command_.id)) item->status=CRML_UI_ACTION_DELIVERED;
    }
    return 200;
}
}
