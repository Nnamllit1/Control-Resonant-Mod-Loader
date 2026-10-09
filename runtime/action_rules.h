#pragma once
#include "runtime.h"
#include "diagnostics/action_restriction_observer.h"
#include <Windows.h>
#include <array>
#include <atomic>

namespace crml::action_rules {
using Identity=action_restriction_observer::Identity;
using Resolve=bool(*)(Identity&) noexcept;
using Clock=uint64_t(*)() noexcept;
// Host writes are serialized separately from engine callbacks. Engine reads
// never wait for the host, allocate memory, or retain component addresses.
class Service final : public Gameplay {
public:
    Service(Resolve resolve,Clock clock) noexcept:resolve_(resolve),clock_(clock){}
    void available(bool value) noexcept;
    uint32_t capabilities() const noexcept override;
    int noclip_poll(uint64_t,float) noexcept override {return -1;}
    int action_rule_set(uint64_t,uint32_t,uint32_t) noexcept override;
    int action_rule_read(uint64_t,crml_action_rule_state&) noexcept override;
    void release(uint64_t) noexcept override;
    uint64_t allowed(const Identity&,uint64_t now) noexcept;
    bool pending(uint64_t now) const noexcept {return available_.load() && now<latest_deadline_.load();}
private:
    struct Lease {uint64_t owner{},deadline{};Identity identity{};uint32_t actions{},restrictions{};};
    static bool same(const Identity&,const Identity&) noexcept;
    static uint64_t native_actions(uint32_t) noexcept;
    SRWLOCK lock_=SRWLOCK_INIT;
    std::array<Lease,32> leases_{};
    std::atomic<bool> available_{};
    std::atomic<uint64_t> latest_deadline_{};
    Resolve resolve_{};
    Clock clock_{};
};
Service& process_service() noexcept;
bool start() noexcept;
void stop() noexcept;
}
