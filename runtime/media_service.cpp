#include "media_service.h"
#include <cstring>

namespace crml::media {
Service& process_service() {static auto* value=new Service;return *value;}
namespace {
struct Lock {
    SRWLOCK* value;
    explicit Lock(SRWLOCK& lock):value(TryAcquireSRWLockExclusive(&lock)?&lock:nullptr){}
    ~Lock(){if(value) ReleaseSRWLockExclusive(value);}
    explicit operator bool() const {return value!=nullptr;}
};
}
void Service::enable(bool value) noexcept {
    AcquireSRWLockExclusive(&lock_);
    if(!value) {state_={};owner_=0;identity_=retired_=0;}
    enabled_.store(value,std::memory_order_release);
    ReleaseSRWLockExclusive(&lock_);
}
uint32_t Service::capabilities() const noexcept {
    return enabled_.load(std::memory_order_acquire)?CRML_CAP_MEDIA_READ|CRML_CAP_MEDIA_SKIP:0;
}
bool Service::observe_at(uintptr_t identity,bool active,bool ready,uint32_t elapsed,
                        std::string_view name,uint32_t source,uint64_t now,bool consume) noexcept {
    Lock lock(lock_);
    if(!lock || !enabled_.load()) return false;
    ++observations_;
    if(!identity || !active) {state_={};identity_=retired_=0;owner_=0;return false;}
    if(retired_==identity) return false;
    if(identity!=identity_ || !state_.generation || now<sampled_at_ || now-sampled_at_>250 || elapsed<state_.elapsed_ms) {
        state_={};owner_=0;retired_=0;
        if(generation_==UINT64_MAX) return false;
        state_.generation=++generation_;
        identity_=identity;
    }
    // A resource identity changing in place must invalidate requests too.
    const bool valid_name=!name.empty() && name.size()<sizeof(state_.name) && name.find('\0')==name.npos &&
        (source==CRML_MEDIA_ENGINE_NAME || source==CRML_MEDIA_MAPPED_NAME);
    const auto length=valid_name?name.size():0;
    if(state_.version && (state_.name_length!=length || (length && std::memcmp(state_.name,name.data(),length)!=0) ||
                         (state_.flags&(CRML_MEDIA_ENGINE_NAME|CRML_MEDIA_MAPPED_NAME))!=(valid_name?source:0))) {
        owner_=0;
        if(generation_==UINT64_MAX) {state_={};return false;}
        state_.generation=++generation_;
    }
    state_.size=sizeof(state_);state_.version=CRML_MEDIA_STATE_VERSION;
    state_.elapsed_ms=elapsed;state_.flags=CRML_MEDIA_ACTIVE;
    if(ready && elapsed>2000) state_.flags|=CRML_MEDIA_SKIPPABLE;
    state_.name_length=static_cast<uint32_t>(length);std::memset(state_.name,0,sizeof(state_.name));
    if(valid_name) {std::memcpy(state_.name,name.data(),length);state_.flags|=source;}
    sampled_at_=now;
    if(owner_ && (now<requested_at_ || now-requested_at_>1000)) owner_=0;
    if(consume && owner_ && (state_.flags&CRML_MEDIA_SKIPPABLE)) {
        owner_=0;retired_=identity;state_={};++skips_;return true;
    }
    return false;
}
int Service::read_at(crml_media_state& out,uint64_t now) noexcept {
    out={};Lock lock(lock_);
    if(!lock || !enabled_.load() || !state_.generation || now<sampled_at_ || now-sampled_at_>250) return -1;
    out=state_;return 1;
}
int Service::request_at(uint64_t owner,uint64_t generation,uint64_t now) noexcept {
    Lock lock(lock_);
    if(!lock || !enabled_.load() || !state_.generation || now<sampled_at_ || now-sampled_at_>250) return -1;
    if(owner_ && (now<requested_at_ || now-requested_at_>1000)) owner_=0;
    if(!owner || !generation || generation!=state_.generation || !(state_.flags&CRML_MEDIA_SKIPPABLE) || owner_) return -1;
    owner_=owner;requested_at_=now;++submissions_;return 0;
}
int Service::media_read(crml_media_state& out) noexcept {return read_at(out,GetTickCount64());}
int Service::media_skip(uint64_t owner,uint64_t generation) noexcept {return request_at(owner,generation,GetTickCount64());}
void Service::release(uint64_t owner) noexcept {
    AcquireSRWLockExclusive(&lock_);
    if(owner_==owner) owner_=0;
    ReleaseSRWLockExclusive(&lock_);
}
}
