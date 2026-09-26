#pragma once
#include "movement_view.h"
#include <cstdint>
#include <atomic>
namespace crml::probe::visibility {
// Requests are written by the host worker; the render phase reads only deadline.
struct Lease {
    uint64_t owner{};
    std::atomic<uint64_t> deadline{};
    int renew(uint64_t who,bool held,uint64_t now) noexcept {
        if(!who) return -1;
        if(owner && owner!=who && deadline.load()>now) return -2;
        owner=who; deadline.store(held?now+500:0,std::memory_order_release);
        return held?1:0;
    }
    bool active(uint64_t now) const noexcept { const auto d=deadline.load(std::memory_order_acquire); return d && now<d; }
    void release(uint64_t who) noexcept { if(owner==who) { deadline.store(0); owner=0; } }
};
using Player = Sample(*)() noexcept;
struct Target { uint8_t* hidden{}; uint32_t* flags{}; uint32_t handle{}; };
// Resolver runs only inside applyHide, which owns MeshHidden and MeshFlags writes.
bool resolve(const void* query, const Sample& player, Target& result) noexcept;
bool start(uintptr_t base, Player player) noexcept;
void stop() noexcept;
int poll(uint64_t owner, bool held, uint64_t now) noexcept;
void release(uint64_t owner) noexcept;
uint64_t submissions() noexcept;
// Exact one-handle renderer command, excluding the allocation's publication header.
struct Command { uint32_t header, handle; };
constexpr Command hide_command(uint32_t handle) { return {0x269,handle}; }
}
