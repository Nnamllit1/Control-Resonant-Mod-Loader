#include "lua_session.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <ostream>

namespace crml::probe::lua::session {
namespace {
SRWLOCK gate=SRWLOCK_INIT;
std::atomic<bool> armed{},requested_stop{},pending{};
std::atomic<uint64_t> generation{};
Execute execute{};
struct State {
    uintptr_t global{},world{};
    uint64_t owner{},next_tick{},calls{},sessions{},initialized{},invocations{},released{},vm_reclaimed{};
    uint64_t owner_retirements{},world_retirements{},explicit_unloads{},expected_errors{},failures{},rejected{};
    uint32_t teardown_depth{};
    DWORD callback_thread{},cleanup_thread{};
    int reference{},last_status{};
    int failure_status{};
    unsigned failure_line{};
    double failure_value{};
    bool retired{},halted{},engine_retired{},events{};
} state;
void retire() noexcept {if(state.reference) state.retired=true;}
void failed(const Result& result) noexcept {
    if(!state.failures) {state.failure_status=result.status;state.failure_value=result.value;state.failure_line=result.error_line;}
    ++state.failures;
}
void finish_reference(Context context) {
    // Only this component owns this ref. Never retry an ambiguous unref:
    // reference numbers recycle and a second release could free another value.
    const int ref=state.reference;
    const bool shutdown=state.events && !state.engine_retired;
    const auto result=execute(context,shutdown?Action::unload:Action::release,ref);
    if(!result.attempted) {++state.rejected;return;}
    state.reference=0;pending=false;state.retired=false;
    state.last_status=result.status;
    if(result.released && result.restored && !result.status && (!shutdown || (result.shutdown && result.value==double(state.calls)))) ++state.released;
    else {failed(result);state.halted=true;}
}
}
void start(Execute operation,bool events) noexcept {
    AcquireSRWLockExclusive(&gate);
    if(!armed.load() && operation) {execute=operation;state.events=events;armed=true;}
    ReleaseSRWLockExclusive(&gate);
}
bool needs_calls() noexcept {return armed.load() && (!requested_stop.load() || pending.load());}
bool configured() noexcept {return armed.load();}
uint64_t revision() noexcept {return generation.load(std::memory_order_acquire);}
void tick(Context context,uint64_t now) {
    if(!needs_calls() || !context.vm || !context.global || !context.world || !context.owner || !TryAcquireSRWLockExclusive(&gate)) return;
    // Hold this short gate through the bounded protected operation. Cleanup
    // waits for it before retiring ownership, then releases the gate before
    // entering engine code. Updates never wait for engine cleanup to finish.
    if(context.revision!=generation.load() || state.teardown_depth || state.halted || (state.global && state.global!=context.global)) {
        ReleaseSRWLockExclusive(&gate);return;
    }
    if(requested_stop.load()) retire();
    if(state.reference && state.world!=context.world) {++state.world_retirements;state.engine_retired=true;retire();}
    if(state.retired) {
        // An explicit Lua shutdown needs the still-live owner. After engine
        // teardown, only release our root; never remove an already-dead handle.
        if(state.events && !state.engine_retired && context.owner!=state.owner) {ReleaseSRWLockExclusive(&gate);return;}
        finish_reference(context);
        ReleaseSRWLockExclusive(&gate);return;
    }
    if(requested_stop.load() || now<state.next_tick) {ReleaseSRWLockExclusive(&gate);return;}
    if(!state.reference) {
        if(state.sessions>=8) {ReleaseSRWLockExclusive(&gate);return;}
        const auto action=state.sessions==1?Action::initialize_error:Action::initialize;
        const auto result=execute(context,action,0);
        if(!result.attempted) {++state.rejected;ReleaseSRWLockExclusive(&gate);return;}
        ++state.sessions;state.global=context.global;state.world=context.world;state.owner=context.owner;
        state.calls=0;state.engine_retired=false;state.reference=result.reference;pending=state.reference>0;state.next_tick=now+250;
        state.last_status=result.status;state.callback_thread=GetCurrentThreadId();
        if(result.reference>0 && !result.status && result.restored) ++state.initialized;
        else {failed(result);requested_stop=true;retire();if(!result.restored || !result.reference) state.halted=true;}
    } else if(state.owner==context.owner) {
        const auto result=execute(context,Action::invoke,state.reference);
        if(!result.attempted) {++state.rejected;ReleaseSRWLockExclusive(&gate);return;}
        ++state.calls;++state.invocations;state.next_tick=now+250;state.last_status=result.status;
        state.callback_thread=GetCurrentThreadId();
        if(state.sessions==2 && state.calls==3 && result.status==2 && result.restored && (!state.events || result.deliberate_error)) {
            ++state.expected_errors;retire();
        } else if(result.status || !result.restored || !std::isfinite(result.value) || result.value!=double(state.calls)) {
            failed(result);requested_stop=true;retire();if(!result.restored) state.halted=true;
        } else if(state.sessions==1 && state.calls==3) {++state.explicit_unloads;retire();}
    }
    ReleaseSRWLockExclusive(&gate);
}
void cleanup_begin(uintptr_t global,uint64_t owner) noexcept {
    AcquireSRWLockExclusive(&gate);
    ++state.teardown_depth;
    ++generation;
    if(!global && armed.load()) {requested_stop=true;state.halted=true;++state.failures;}
    if(armed.load() && state.reference && state.global==global && state.owner==owner && !state.engine_retired) {
        ++state.owner_retirements;state.engine_retired=true;state.cleanup_thread=GetCurrentThreadId();retire();
    }
    ReleaseSRWLockExclusive(&gate);
}
void cleanup_end() noexcept {
    AcquireSRWLockExclusive(&gate);
    if(state.teardown_depth) --state.teardown_depth;
    ReleaseSRWLockExclusive(&gate);
}
void close_begin(uintptr_t global) noexcept {
    AcquireSRWLockExclusive(&gate);
    ++state.teardown_depth;
    ++generation;
    if(!global && armed.load()) {requested_stop=true;state.halted=true;++state.failures;}
    if(armed.load() && state.global==global) {
        if(state.reference) ++state.vm_reclaimed;
        state.reference=0;pending=false;state.global=0;state.world=0;state.owner=0;state.retired=false;
    }
    ReleaseSRWLockExclusive(&gate);
}
void close_end() noexcept {cleanup_end();}
void stop() noexcept {requested_stop=true;}
void write(std::ostream& out) {
    State copy;
    AcquireSRWLockShared(&gate);copy=state;ReleaseSRWLockShared(&gate);
    out<<"{\"type\":\"lua_session\",\"schema\":2,\"armed\":"<<(armed.load()?"true":"false")
       <<",\"event_mode\":"<<(copy.events?"true":"false")
       <<",\"stop_requested\":"<<(requested_stop.load()?"true":"false")
       <<",\"active_reference\":"<<(copy.reference>0?"true":"false")<<",\"retired\":"<<(copy.retired?"true":"false")
       <<",\"halted\":"<<(copy.halted?"true":"false")<<",\"sessions\":"<<copy.sessions<<",\"initialized\":"<<copy.initialized
       <<",\"invocations\":"<<copy.invocations<<",\"current_calls\":"<<copy.calls<<",\"released\":"<<copy.released
       <<",\"vm_reclaimed\":"<<copy.vm_reclaimed<<",\"owner_retirements\":"<<copy.owner_retirements
       <<",\"world_retirements\":"<<copy.world_retirements<<",\"explicit_unloads\":"<<copy.explicit_unloads
       <<",\"expected_errors\":"<<copy.expected_errors<<",\"failures\":"<<copy.failures<<",\"rejected\":"<<copy.rejected
       <<",\"last_status\":"<<copy.last_status<<",\"teardown_depth\":"<<copy.teardown_depth
       <<",\"failure_status\":"<<copy.failure_status<<",\"failure_line\":"<<copy.failure_line<<",\"failure_value\":";
    if(std::isfinite(copy.failure_value)) out<<copy.failure_value;else out<<"null";
    out
       <<",\"callback_thread\":"<<copy.callback_thread<<",\"cleanup_thread\":"<<copy.cleanup_thread<<"}\n";
}
#ifdef CRML_LUA_SESSION_TESTING
void reset_for_test(Execute operation,bool events) {
    AcquireSRWLockExclusive(&gate);
    state={};state.events=events;execute=operation;armed=true;requested_stop=false;pending=false;generation=0;
    ReleaseSRWLockExclusive(&gate);
}
#endif
}
