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
    void read(const std::array<uint16_t,CRML_ACTION_COUNT>& keys,
              uint32_t& flags,uint32_t& held) noexcept override {
        flags=CRML_INPUT_CONTEXT_KNOWN;held=0;
        if(!available()) return;
        flags|=CRML_INPUT_AVAILABLE;
        const auto window=GetForegroundWindow();
        DWORD process{};GetWindowThreadProcessId(window,&process);
        if(!window || process!=GetCurrentProcessId()) return;
        BYTE state[256]{};
        const bool copied=probe::input::snapshot(state,GetTickCount64());
        // A foreground change during the read makes the whole sample unusable.
        if(GetForegroundWindow()!=window) return;
        flags|=CRML_INPUT_FOCUSED;
        if(!copied) return;
        flags|=CRML_INPUT_FRESH;
        if(state[VK_ESCAPE]&0x80) {flags|=CRML_INPUT_EMERGENCY;return;}
        for(size_t i=0;i<keys.size();++i)
            if(keys[i] && keys[i]<256 && (state[keys[i]]&0x80)) held|=1u<<i;
    }
    uint32_t state_flags() noexcept override {
        uint32_t flags{},held{};read({},flags,held);return flags;
    }
    uint32_t sample(const std::array<uint16_t,CRML_ACTION_COUNT>& keys) noexcept override {
        uint32_t flags{},held{};read(keys,flags,held);return held;
    }
private:
    bool attempted_{}; // Runtime worker only.
    bool requested_{};
};
}
