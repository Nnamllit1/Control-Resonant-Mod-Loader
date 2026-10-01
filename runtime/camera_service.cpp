#include "camera_service.h"
#include "camera_update_hook.h"
#include <cmath>
#include <cstring>

namespace crml::camera {
namespace {
// Process-lifetime storage: pinned callbacks never refer to a worker's Service.
SnapshotCache cache;
void sample(void* context,bool before) noexcept {
    if(before) cache.begin();
    else cache.finish(context,GetTickCount64());
}
}
void SnapshotCache::begin() noexcept {
    in_flight_.fetch_add(1,std::memory_order_acq_rel);
    epoch_.fetch_add(1,std::memory_order_acq_rel);
}
void SnapshotCache::finish(void* context,uint64_t now) noexcept {
    const auto epoch=epoch_.load(std::memory_order_acquire);
    Selected copied{};
    const bool readable=in_flight_.load(std::memory_order_acquire)==1 && selected(context,copied)==Read::ok;
    if(TryAcquireSRWLockExclusive(&lock_)) {
        if(readable && epoch_.load(std::memory_order_acquire)==epoch && in_flight_.load(std::memory_order_acquire)==1) {
            const bool changed=!state_.version || world_!=copied.world || global_!=copied.global || entity_!=copied.entity || state_.mode!=copied.selector || now<sampled_at_ || now-sampled_at_>500;
            // A generation never wraps or recycles zero, even after an
            // invalidation. Exhaustion keeps this service unavailable.
            if(changed && next_generation_==UINT64_MAX) {
                state_={};world_=global_=0;entity_=0;
                ReleaseSRWLockExclusive(&lock_);
                in_flight_.fetch_sub(1,std::memory_order_release);
                return;
            }
            const auto generation=changed?++next_generation_:state_.generation;
            state_={};state_.version=1;state_.generation=generation;state_.mode=copied.selector;
            state_.flags=CRML_CAMERA_STATE_POSE;
            std::memcpy(state_.position,copied.position,sizeof(state_.position));
            std::memcpy(state_.basis,copied.basis,sizeof(state_.basis));
            // The projection path uses xx=1/tan(fov/2), yy=aspect/tan(fov/2):
            // CameraView +0x30 is horizontal radians and +0x34 is aspect.
            // This is the selected component, not a renderer override.
            if(std::isfinite(copied.lens[0]) && copied.lens[0]>0 && copied.lens[0]<3.14159265358979323846f &&
               std::isfinite(copied.lens[1]) && copied.lens[1]>0 && copied.lens[1]<=100) {
                state_.flags|=CRML_CAMERA_STATE_LENS;
                state_.horizontal_fov_radians=copied.lens[0];state_.aspect_ratio=copied.lens[1];
            }
            world_=copied.world;global_=copied.global;entity_=copied.entity;
            sampled_at_=now;published_epoch_=epoch;
        } else {state_={};world_=global_=0;entity_=0;}
        ReleaseSRWLockExclusive(&lock_);
    }
    in_flight_.fetch_sub(1,std::memory_order_release);
}
int SnapshotCache::read(crml_camera_state& out,uint64_t now) noexcept {
    out={};
    if(!TryAcquireSRWLockShared(&lock_)) return 0;
    bool ready=state_.version && !in_flight_.load(std::memory_order_acquire) &&
        published_epoch_==epoch_.load(std::memory_order_acquire) && now>=sampled_at_ && now-sampled_at_<=500;
    if(ready) {
        out=state_;out.age_ms=static_cast<uint32_t>(now-sampled_at_);
        // begin() invalidates without waiting for this reader's lock. Reject
        // an update that began while the historical value was being copied.
        ready=!in_flight_.load(std::memory_order_acquire) && published_epoch_==epoch_.load(std::memory_order_acquire);
        if(!ready) out={};
    }
    ReleaseSRWLockShared(&lock_);
    return ready?1:0;
}
std::string Service::start() {
    if(active_) return "Read-only selected camera service ready";
    const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!camera_update::start(image) || !camera_update::observe(&sample)) return "Camera service unavailable: update hook signature or ownership conflict";
    active_=true;
    return "Read-only selected camera service ready";
}
int Service::camera_read(crml_camera_state& out) noexcept {
    out={};return active_?cache.read(out,GetTickCount64()):-1;
}
}
