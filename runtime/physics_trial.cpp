#include "physics_trial.h"
#include <cmath>
#include <limits>

namespace crml::physics {
namespace {
bool valid(float value) noexcept { return std::isfinite(value) && value>=0; }
}
TrialResult DampingTrial::clear(TrialResult result) noexcept {
    state_=State::idle; target_={}; original_=applied_=0; deadline_=last_time_=0;
    return result;
}
TrialResult DampingTrial::apply(DampingBackend& backend,const Target& target,float value,
                               uint64_t now,uint64_t duration) noexcept {
    if(pending()) return TrialResult::busy;
    if(!target.epoch || !target.entity || target.entity==UINT64_MAX || target.body==UINT64_MAX
       || target.local_index>=65536 || !valid(value) || value>100 || !duration || duration>10000
       || now>std::numeric_limits<uint64_t>::max()-duration) return TrialResult::invalid;
    const auto before=backend.read(target);
    if(before.status==ReadStatus::retired) return TrialResult::retired;
    if(before.status!=ReadStatus::ok || !valid(before.value)) return TrialResult::unavailable;
    if(before.value==value) return TrialResult::unchanged;
    target_=target; original_=before.value; applied_=value;
    deadline_=now+duration; last_time_=now;
    // Install restoration state BEFORE calling the adapter: a failed call can
    // have modified the property. Only a later observation can resolve that.
    state_=State::restoring;
    if(!backend.write(target_,original_,applied_)) return TrialResult::restore_pending;
    const auto after=backend.read(target_);
    if(after.status==ReadStatus::retired) return clear(TrialResult::retired);
    if(after.status!=ReadStatus::ok || !valid(after.value)) return TrialResult::restore_pending;
    if(after.value==original_) return clear(TrialResult::refused);
    if(after.value!=applied_) return clear(TrialResult::conflict);
    state_=State::active;
    return TrialResult::applied;
}
TrialResult DampingTrial::poll(DampingBackend& backend,uint64_t now,bool keep) noexcept {
    if(!pending()) return TrialResult::idle;
    // Expiration and clock discontinuity request restoration even if reads are
    // temporarily unavailable. A later successful callback must not renew it.
    if(!keep || now>=deadline_ || now<last_time_) state_=State::restoring;
    last_time_=now;
    const auto current=backend.read(target_);
    if(current.status==ReadStatus::retired) return clear(TrialResult::retired);
    if(current.status!=ReadStatus::ok || !valid(current.value)) {
        state_=State::restoring;
        return TrialResult::restore_pending;
    }
    if(current.value==original_) return clear(TrialResult::restored);
    if(current.value!=applied_) return clear(TrialResult::conflict);
    if(state_==State::active) return TrialResult::active;
    if(!backend.write(target_,applied_,original_)) return TrialResult::restore_pending;
    const auto after=backend.read(target_);
    if(after.status==ReadStatus::retired) return clear(TrialResult::retired);
    if(after.status!=ReadStatus::ok || !valid(after.value)) return TrialResult::restore_pending;
    if(after.value==original_) return clear(TrialResult::restored);
    if(after.value!=applied_) return clear(TrialResult::conflict);
    return TrialResult::restore_pending;
}
}
