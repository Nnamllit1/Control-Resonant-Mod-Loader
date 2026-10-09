#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <iosfwd>
#include "tutorial_context.h"

namespace crml::tutorial_observer {
enum class Phase : uint32_t { initialize, hint_sync, panel_sync, panel_events, requests, count };
enum class Query : uint32_t { ok, caller, memory, range };
inline constexpr size_t phase_count=static_cast<size_t>(Phase::count);
struct Event {
    uint64_t sequence{},begin{},end{},object{},entity{},world{};
    uint32_t thread{},overlap{},caller_rva{UINT32_MAX};
    Phase phase{};
    Query query{};
    tutorial::IdentityStatus identity{tutorial::IdentityStatus::unavailable};
    bool returned{};
};
// A sampled storage location, NOT an entity handle or lifetime generation.
Query inspect(Phase,uintptr_t caller,uintptr_t expected,const void* query,uint64_t salt,uint64_t& object) noexcept;
struct Counts {uint64_t calls{},overlaps{},unwinds{},rejected{},dropped{};uint32_t in_flight{};};
// Tracks admitted observation spans, including time before Recorder::enter().
// Closing this gate does not stop engine calls or establish engine quiescence.
class Admissions {
public:
    // Startup only: the caller must ensure no previous admissions remain.
    void open() noexcept {state_.store(0,std::memory_order_release);}
    bool enter() noexcept {
        auto state=state_.load(std::memory_order_acquire);
        while(!(state&closed) && state!=closed-1) {
            if(state_.compare_exchange_weak(state,state+1,std::memory_order_acquire)) return true;
        }
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
    Event enter(Phase,uint32_t thread,uint64_t tick) noexcept;
    void leave(Event) noexcept;
    size_t drain(Event* out,size_t capacity) noexcept;
    Counts counts(Phase) const noexcept;
private:
    struct Counter {
        std::atomic<uint64_t> calls{},overlaps{},unwinds{},rejected{},dropped{};
        std::atomic<uint32_t> active{};
    };
    std::array<Counter,phase_count> counts_{};
    std::atomic<uint64_t> sequence_{};
    SRWLOCK lock_=SRWLOCK_INIT;
    std::array<Event,256> events_{};
    size_t head_{},size_{};
};
// One capture per process. A later start does not rearm a stopped capture.
bool start() noexcept;
bool active() noexcept;
void stop() noexcept;
void poll(std::ostream&);
}
