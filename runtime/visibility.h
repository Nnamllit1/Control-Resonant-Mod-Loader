#pragma once
#include "movement_view.h"
#include "../sdk/include/crml_state.h"
#include <cstdint>
#include <atomic>
namespace crml::probe::visibility {
static_assert(std::atomic<uint64_t>::is_always_lock_free);
// Owner/deadline requests and snapshots belong to the host worker. The render
// phase reads atomic lease tokens and publishes atomic, lease-tagged evidence.
struct Lease {
    uint64_t owner{};
    std::atomic<uint64_t> deadline{};
    std::atomic<uint64_t> epoch{},hidden_epoch{},submitted_epoch{};
    int renew(uint64_t who,bool held,uint64_t now) noexcept {
        if(!who) return -1;
        if(owner && owner!=who && deadline.load()>now) return -2;
        const bool new_lease=held && (owner!=who || !active(now));
        const auto serial=epoch.load(std::memory_order_relaxed);
        if(held && (now>UINT64_MAX-500 || (new_lease && serial==UINT64_MAX))) return -1;
        owner=who; deadline.store(held?now+500:0,std::memory_order_release);
        // Publish the new token after its deadline. Readers reject an epoch
        // transition; late reports cannot be attributed to a replacement lease.
        if(new_lease) epoch.store(serial+1,std::memory_order_release);
        return held?1:0;
    }
    bool active(uint64_t now) const noexcept { const auto d=deadline.load(std::memory_order_acquire); return d && now<d; }
    void release(uint64_t who) noexcept { if(owner==who) { deadline.store(0); owner=0; } }
    uint64_t observation_token(uint64_t now) const noexcept {
        const auto token=epoch.load(std::memory_order_acquire);
        return token && active(now) && token==epoch.load(std::memory_order_acquire)?token:0;
    }
    void observe(uint64_t token,bool submitted) noexcept {
        if(!token) return;
        // At most one CAS per counter: telemetry never waits on a render thread.
        // A raced publication can be omitted, but cannot overwrite newer evidence.
        const auto publish=[token](std::atomic<uint64_t>& counter) {
            auto old=counter.load(std::memory_order_relaxed);
            if(token>old) counter.compare_exchange_strong(old,token,std::memory_order_release,std::memory_order_relaxed);
        };
        publish(hidden_epoch);
        if(submitted) publish(submitted_epoch);
    }
    int read_state(uint64_t who,uint64_t now,crml_visibility_state& out) const noexcept {
        out={};if(!who) return -1;
        out.version=1;
        const auto d=deadline.load(std::memory_order_acquire);
        if(owner!=who || !d) return 1;
        out.state=now<d?CRML_VISIBILITY_ACTIVE:CRML_VISIBILITY_EXPIRED;
        out.remaining_ms=now<d?static_cast<uint32_t>((d-now)>500?500:d-now):0;
        const auto token=epoch.load(std::memory_order_acquire);
        if(token && hidden_epoch.load(std::memory_order_acquire)==token) out.flags|=CRML_VISIBILITY_OBSERVED_HIDDEN;
        if(token && submitted_epoch.load(std::memory_order_acquire)==token)
            out.flags|=CRML_VISIBILITY_SUBMITTED|CRML_VISIBILITY_OBSERVED_HIDDEN;
        return 1;
    }
};
using Player = Sample(*)() noexcept;
struct Target { uint8_t* hidden{}; uint32_t* flags{}; uint32_t handle{}; };
// Resolver runs only inside applyHide, which owns MeshHidden and MeshFlags writes.
bool resolve(const void* query, const Sample& player, Target& result) noexcept;
bool start(uintptr_t base, Player player) noexcept;
void stop() noexcept;
int poll(uint64_t owner, bool held, uint64_t now) noexcept;
int read_state(uint64_t owner,uint64_t now,crml_visibility_state& out) noexcept;
void release(uint64_t owner) noexcept;
uint64_t submissions() noexcept;
// Exact one-handle renderer command, excluding the allocation's publication header.
struct Command { uint32_t header, handle; };
constexpr Command hide_command(uint32_t handle) { return {0x269,handle}; }
}
