#include "action_rules.h"
#include <limits>

namespace crml::action_rules {
bool Service::same(const Identity& a,const Identity& b) noexcept {
    return a.world && a.entity && a.world==b.world && a.entity==b.entity && a.lifetime==b.lifetime;
}
uint64_t Service::native_actions(uint32_t mask) noexcept {
    constexpr uint64_t ids[]{1,2,0x10,0x20,0x10000};
    uint64_t value{};
    for(unsigned i=0;i<5;++i) if(mask&(1u<<i)) value|=ids[i];
    return value;
}
void Service::available(bool value) noexcept {
    // Closing the gate precedes clearing requests; reopening starts empty.
    available_.store(false);
    AcquireSRWLockExclusive(&lock_);
    leases_={};
    latest_deadline_.store(0);
    available_.store(value);
    ReleaseSRWLockExclusive(&lock_);
}
uint32_t Service::capabilities() const noexcept {return available_.load()?CRML_CAP_ACTION_RULES:0u;}
int Service::action_rule_set(uint64_t owner,uint32_t actions,uint32_t restrictions) noexcept {
    if(!owner || (actions&~CRML_RULE_ACTION_ALL) ||
       (actions?restrictions!=CRML_RULE_RESTRICTION_AREA:restrictions!=0)) return -3;
    if(!actions) {release(owner);return 0;}
    if(!available_.load()) return -1;
    Identity current{};
    if(!resolve_ || !clock_ || !resolve_(current) || !current.world || !current.entity) return -5;
    const auto now=clock_();
    if(now>std::numeric_limits<uint64_t>::max()-500) return -3;
    AcquireSRWLockExclusive(&lock_);
    if(!available_.load()) {ReleaseSRWLockExclusive(&lock_);return -1;}
    Lease* selected=nullptr;
    for(auto& lease:leases_) if(lease.owner==owner) {selected=&lease;break;}
    if(!selected) for(auto& lease:leases_) if(!lease.owner || now>=lease.deadline) {selected=&lease;break;}
    if(selected) {
        *selected={owner,now+500,current,actions,restrictions};
        uint64_t latest{};
        for(const auto& lease:leases_) if(lease.deadline>latest) latest=lease.deadline;
        latest_deadline_.store(latest);
    }
    ReleaseSRWLockExclusive(&lock_);
    return selected?1:-2;
}
int Service::action_rule_read(uint64_t owner,crml_action_rule_state& out) noexcept {
    out={};
    if(!owner) return -3;
    if(!available_.load()) return -1;
    const auto now=clock_();
    Identity current{};const bool have=resolve_ && resolve_(current);
    out.version=1;out.supported_actions=CRML_RULE_ACTION_ALL;
    out.supported_restrictions=CRML_RULE_RESTRICTION_AREA;
    AcquireSRWLockShared(&lock_);
    for(const auto& lease:leases_) if(lease.owner==owner) {
        out.requested_actions=lease.actions;out.restrictions=lease.restrictions;
        out.state=now>=lease.deadline?CRML_RULE_EXPIRED:
            !have || !same(lease.identity,current)?CRML_RULE_CONTEXT_CHANGED:CRML_RULE_LEASED;
        if(out.state==CRML_RULE_LEASED) out.remaining_ms=static_cast<uint32_t>(lease.deadline-now);
        break;
    }
    ReleaseSRWLockShared(&lock_);
    return 1;
}
void Service::release(uint64_t owner) noexcept {
    AcquireSRWLockExclusive(&lock_);
    for(auto& lease:leases_) if(lease.owner==owner) lease={};
    uint64_t latest{};
    for(const auto& lease:leases_) if(lease.deadline>latest) latest=lease.deadline;
    latest_deadline_.store(latest);
    ReleaseSRWLockExclusive(&lock_);
}
uint64_t Service::allowed(const Identity& current,uint64_t now) noexcept {
    if(!available_.load() || !TryAcquireSRWLockShared(&lock_)) return 0;
    uint32_t mask{};
    if(available_.load()) for(const auto& lease:leases_)
        if(lease.owner && now<lease.deadline && same(lease.identity,current)) mask|=lease.actions;
    ReleaseSRWLockShared(&lock_);
    return native_actions(mask);
}
}
