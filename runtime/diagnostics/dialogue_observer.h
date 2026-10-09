#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include "dialogue_observation.h"

namespace crml::dialogue_observer {
// Diagnostic observations of the current selected subtitle segment only.
struct Event {
    uint64_t sequence{},time_ms{},text_hash{};
    uint32_t allocation{},playback_key{},segment{UINT32_MAX},text_length{};
    dialogue::Read read{dialogue::Read::empty};
};
struct Counts {
    uint64_t calls{},changes{},samples{},rejected{},dropped{},unwinds{};
    uint32_t in_flight{};
};
class Admissions {
public:
    void open() noexcept {state_.store(0,std::memory_order_release);}
    bool enter() noexcept {
        auto state=state_.load(std::memory_order_acquire);
        while(!(state&closed) && state!=closed-1)
            if(state_.compare_exchange_weak(state,state+1,std::memory_order_acquire)) return true;
        return false;
    }
    void leave() noexcept {state_.fetch_sub(1,std::memory_order_release);}
    void close() noexcept {state_.fetch_or(closed,std::memory_order_acq_rel);}
    uint64_t pending() const noexcept {return state_.load(std::memory_order_acquire)&~closed;}
private:
    static constexpr uint64_t closed=uint64_t{1}<<63;
    std::atomic<uint64_t> state_{closed};
};
class Recorder {
public:
    void begin() noexcept;
    void end(bool returned,bool changed,const Event* sample) noexcept;
    size_t drain(Event* out,size_t capacity) noexcept;
    Counts counts() const noexcept;
private:
    std::atomic<uint64_t> calls_{},changes_{},samples_{},rejected_{},dropped_{},unwinds_{};
    std::atomic<uint32_t> in_flight_{};
    SRWLOCK lock_=SRWLOCK_INIT;
    std::array<Event,256> events_{};
    size_t head_{},size_{};
};
bool start() noexcept;
bool active() noexcept;
void stop() noexcept;
void poll(std::ostream&);
#ifdef CRML_DIALOGUE_OBSERVER_TESTING
bool test_callthrough();
bool test_native_unwind();
bool test_callers() noexcept;
bool test_reporting();
#endif
}
