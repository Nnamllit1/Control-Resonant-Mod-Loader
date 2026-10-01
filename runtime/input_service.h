#pragma once
#include "runtime.h"
#include "input_filter.h"
#include <MinHook.h>

namespace crml {
// Used only in the game runtime. The standalone host has no OS input provider.
class KeyboardInput final : public Input {
public:
    bool available() noexcept override {
        requested_=true;
        if(probe::input::observing()) return true;
        if(attempted_) return false;
        attempted_=true;
        const auto init=MH_Initialize();
        return (init==MH_OK || init==MH_ERROR_ALREADY_INITIALIZED) && probe::input::start();
    }
    bool requested() const noexcept { return requested_; }
    uint32_t sample(const std::array<uint16_t,CRML_ACTION_COUNT>& keys) noexcept override {
        DWORD process{};
        GetWindowThreadProcessId(GetForegroundWindow(),&process);
        if(process!=GetCurrentProcessId() || !available() || probe::input::down(VK_ESCAPE)) return 0;
        uint32_t result{};
        for(size_t i=0;i<keys.size();++i) if(keys[i] && probe::input::down(keys[i])) result|=1u<<i;
        return result;
    }
private:
    bool attempted_{}; // Runtime worker only.
    bool requested_{};
};
}
