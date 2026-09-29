#include "lua_session.h"
#include "lua_dispatch.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <ostream>

namespace crml::probe::lua::session {
namespace {
namespace dispatch=engine::lua::dispatch;
using dispatch::Gate;
using dispatch::Interruption;
std::atomic<bool> armed{},requested_stop{},pending{};
Execute execute{};
struct State {
    uintptr_t global{},world{};
    uint64_t owner{},next_tick{},calls{},sessions{},initialized{},invocations{},released{},vm_reclaimed{};
    uint64_t owner_retirements{},world_retirements{},explicit_unloads{},expected_errors{},failures{},rejected{};
    uint64_t listener_error_checks{},initialization_rollbacks{},initialization_recoveries{};
    uint64_t eligible_updates{},owner_waits{},vm_waits{},revision_waits{},teardown_waits{};
    uint64_t interrupted_calls{},reentrant_cleanups{},reentrant_closes{};
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
struct Outcome {Result result;Interruption interruption;};
Outcome perform(Context context,Action action,int reference) {
    dispatch::Operation operation(context,&state);
    try {
        const auto result=execute(context,action,reference);
        if(result.attempted && !result.restored && operation.interruption!=Interruption::closed) dispatch::halt();
        if(operation.interruption!=Interruption::none) ++state.interrupted_calls;
        return {result,operation.interruption};
    } catch(...) {
        // An unexpected native exception must not leave the coordinator locked
        // or permit another operation against ambiguous reference ownership.
        Result failure{};failure.status=-308;failed(failure);
        requested_stop=true;state.halted=true;dispatch::halt();retire();
        throw;
    }
}
void finish_reference(Context context) {
    // Only this component owns this ref. Never retry an ambiguous unref:
    // reference numbers recycle and a second release could free another value.
    const int ref=state.reference;
    const bool shutdown=state.events && !state.engine_retired;
    const auto [result,interruption]=perform(context,shutdown?Action::unload:Action::release,ref);
    if(interruption==Interruption::closed || interruption==Interruption::unknown) return;
    if(!result.attempted) {++state.rejected;return;}
    if(interruption==Interruption::owner && !result.released && !result.release_attempted) {
        // Shutdown stopped when its owner retired. Keep our separate root for a
        // later raw release, without retrying the Lua shutdown or listener handle.
        retire();
        if(!result.restored) {failed(result);state.halted=true;}
        return;
    }
    state.reference=0;pending=false;state.retired=false;
    state.last_status=result.status;
    if(result.released && result.restored && !result.status && (interruption==Interruption::owner || !shutdown || (result.shutdown && result.value==double(state.calls)))) ++state.released;
    else {failed(result);state.halted=true;}
}
void notify(void*,dispatch::Event event) noexcept {
    const auto global=event.global,owner=event.owner;
    const bool ours=event.operation && event.operation->host==&state;
    if(ours) {
        if(event.close) ++state.reentrant_closes;else ++state.reentrant_cleanups;
    }
    if(!global && armed.load()) {requested_stop=true;state.halted=true;++state.failures;}
    if(event.close) {
        if(armed.load() && state.global==global) {
            if(state.reference) ++state.vm_reclaimed;
            state.reference=0;pending=false;state.global=0;state.world=0;state.owner=0;state.retired=false;
        }
    } else if(armed.load() && state.owner && state.global==global && state.owner==owner && !state.engine_retired) {
        ++state.owner_retirements;state.engine_retired=true;state.cleanup_thread=GetCurrentThreadId();retire();
    } else if(ours && event.operation->context.global==global && event.operation->context.owner==owner &&
              event.operation->interruption==Interruption::owner) {
        ++state.owner_retirements;state.cleanup_thread=GetCurrentThreadId();
    }
}

}
void start(Execute operation,bool events) noexcept {
    Gate lock;
    if(!armed.load() && operation && dispatch::subscribe(&state,&notify)) {execute=operation;state.events=events;armed=true;}
}
bool call_vm_live() noexcept {return dispatch::call_vm_live();}
bool call_owner_live() noexcept {return dispatch::call_owner_live();}
bool needs_calls() noexcept {return armed.load() && (!requested_stop.load() || pending.load());}
bool configured() noexcept {return armed.load();}
uint64_t revision() noexcept {return dispatch::revision();}
void tick(Context context,uint64_t now) {
    if(!needs_calls() || !context.vm || !context.global || !context.world || !context.owner) return;
    Gate lock(false);if(!lock.held) return; // Includes nested script updates.
    // Other-thread cleanup waits; same-thread notifications reuse this ownership
    // and invalidate the in-flight operation before entering original cleanup.
    ++state.eligible_updates;
    if(context.revision!=dispatch::revision()) {++state.revision_waits;return;}
    if(dispatch::teardown_depth()) {++state.teardown_waits;return;}
    if(dispatch::halted()) state.halted=true;
    if(state.halted) return;
    if(state.global && state.global!=context.global) {
        ++state.vm_waits;
        return;
    }
    if(requested_stop.load()) retire();
    if(!state.engine_retired && state.world && state.world!=context.world) {++state.world_retirements;state.engine_retired=true;retire();}
    if(state.retired) {
        // An explicit Lua shutdown needs the still-live owner. After engine
        // teardown, only release our root; never remove an already-dead handle.
        if(state.events && !state.engine_retired && context.owner!=state.owner) {++state.owner_waits;return;}
        finish_reference(context);
        return;
    }
    if(requested_stop.load() || now<state.next_tick) return;
    // Keep the selected owner between stages as well as during invocation.
    // Otherwise whichever unrelated script updates immediately after a release
    // can take over the next stage, even if it never updates again afterward.
    // Only observed retirement/world replacement permits selecting another owner.
    if(state.owner && !state.engine_retired && state.owner!=context.owner) {
        ++state.owner_waits;return;
    }
    if(!state.reference) {
        if(state.sessions>=8) return;
        const auto action=state.sessions==1?Action::initialize_error:
            state.events && state.sessions==2?Action::initialize_listener_error:
            state.events && state.sessions==3?Action::initialize_rollback:Action::initialize;
        const auto [result,interruption]=perform(context,action,0);
        if(!result.attempted) {++state.rejected;return;}
        if(interruption==Interruption::closed || interruption==Interruption::unknown) {
            ++state.sessions;
            // A root obtained during construction was not yet in state when
            // close_begin ran. Do not publish it into a reused global address.
            if(interruption==Interruption::closed && result.reference>0) ++state.vm_reclaimed;
            return;
        }
        ++state.sessions;state.global=context.global;state.world=context.world;state.owner=context.owner;
        state.calls=0;state.engine_retired=interruption==Interruption::owner;state.reference=result.reference;pending=state.reference>0;state.next_tick=now+250;
        state.last_status=result.status;state.callback_thread=GetCurrentThreadId();
        if(interruption==Interruption::owner) {
            retire();
            if(!result.restored) {failed(result);state.halted=true;}
            return;
        }
        if(result.reference>0 && !result.status && result.restored) ++state.initialized;
        else {failed(result);requested_stop=true;retire();if(!result.restored || !result.reference) state.halted=true;}
    } else if(state.owner==context.owner) {
        const auto [result,interruption]=perform(context,Action::invoke,state.reference);
        if(!result.attempted) {++state.rejected;return;}
        if(interruption!=Interruption::none) {
            ++state.invocations;
            if(interruption==Interruption::owner && !result.restored) {failed(result);state.halted=true;}
            return;
        }
        ++state.calls;++state.invocations;state.next_tick=now+250;state.last_status=result.status;
        state.callback_thread=GetCurrentThreadId();
        if(state.sessions==2 && state.calls==3 && result.status==2 && result.restored && (!state.events || result.deliberate_error)) {
            ++state.expected_errors;retire();
        } else if(state.events && state.sessions==4 && state.calls==1 && !result.status && result.restored && result.value==-410) {
            // This marker is returned only after the deliberately failed
            // initializer removes its listener and checks that delivery stopped.
            ++state.initialization_rollbacks;
        } else if(result.status || !result.restored || !std::isfinite(result.value) || result.value!=double(state.calls) ||
                  (state.events && state.sessions==4 && state.calls==1)) {
            failed(result);requested_stop=true;retire();if(!result.restored) state.halted=true;
        } else if(state.sessions==1 && state.calls==3) {++state.explicit_unloads;retire();}
        else if(state.events && state.sessions==3 && state.calls==4) {++state.listener_error_checks;retire();}
        else if(state.events && state.sessions==4 && state.calls==3) {++state.initialization_recoveries;retire();}
    }
}
void cleanup_begin(uintptr_t global,uint64_t owner) noexcept {dispatch::cleanup_begin(global,owner);}
void cleanup_end() noexcept {dispatch::teardown_end();}
void close_begin(uintptr_t global) noexcept {dispatch::close_begin(global);}
void close_end() noexcept {dispatch::teardown_end();}
void stop() noexcept {requested_stop=true;}
void write(std::ostream& out) {
    State copy;unsigned depth;
    {Gate lock;copy=state;depth=dispatch::teardown_depth();}
    out<<"{\"type\":\"lua_session\",\"schema\":4,\"armed\":"<<(armed.load()?"true":"false")
       <<",\"event_mode\":"<<(copy.events?"true":"false")
       <<",\"stop_requested\":"<<(requested_stop.load()?"true":"false")
       <<",\"active_reference\":"<<(copy.reference>0?"true":"false")<<",\"retired\":"<<(copy.retired?"true":"false")
       <<",\"halted\":"<<(copy.halted?"true":"false")<<",\"sessions\":"<<copy.sessions<<",\"initialized\":"<<copy.initialized
       <<",\"invocations\":"<<copy.invocations<<",\"current_calls\":"<<copy.calls<<",\"released\":"<<copy.released
       <<",\"vm_reclaimed\":"<<copy.vm_reclaimed<<",\"owner_retirements\":"<<copy.owner_retirements
       <<",\"world_retirements\":"<<copy.world_retirements<<",\"explicit_unloads\":"<<copy.explicit_unloads
       <<",\"expected_errors\":"<<copy.expected_errors<<",\"failures\":"<<copy.failures<<",\"rejected\":"<<copy.rejected
       <<",\"listener_error_checks\":"<<copy.listener_error_checks
       <<",\"initialization_rollbacks\":"<<copy.initialization_rollbacks
       <<",\"initialization_recoveries\":"<<copy.initialization_recoveries
       <<",\"eligible_updates\":"<<copy.eligible_updates<<",\"owner_waits\":"<<copy.owner_waits
       <<",\"vm_waits\":"<<copy.vm_waits<<",\"revision_waits\":"<<copy.revision_waits
       <<",\"teardown_waits\":"<<copy.teardown_waits
       <<",\"interrupted_calls\":"<<copy.interrupted_calls<<",\"reentrant_cleanups\":"<<copy.reentrant_cleanups
       <<",\"reentrant_closes\":"<<copy.reentrant_closes
       <<",\"last_status\":"<<copy.last_status<<",\"teardown_depth\":"<<depth
       <<",\"failure_status\":"<<copy.failure_status<<",\"failure_line\":"<<copy.failure_line<<",\"failure_value\":";
    if(std::isfinite(copy.failure_value)) out<<copy.failure_value;else out<<"null";
    out
       <<",\"callback_thread\":"<<copy.callback_thread<<",\"cleanup_thread\":"<<copy.cleanup_thread<<"}\n";
}
#ifdef CRML_LUA_SESSION_TESTING
void reset_for_test(Execute operation,bool events) {
    Gate lock;
    dispatch::reset_for_test();dispatch::subscribe(&state,&notify);
    state={};state.events=events;execute=operation;armed=true;requested_stop=false;pending=false;
}
#endif
}
