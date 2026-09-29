#include "lua_dispatch.h"
#include <Windows.h>
#include <array>
#include <atomic>

namespace crml::engine::lua::dispatch {
namespace {
SRWLOCK mutex=SRWLOCK_INIT;
thread_local unsigned gate_depth{};
thread_local Operation* current{};
std::atomic<uint64_t> generation{};
std::atomic<unsigned> subscribers{};
std::atomic<bool> faulted{};
unsigned depth{};
struct Subscription {void* host{};Observer observer{};};
std::array<Subscription,8> observers{};
}
Gate::Gate(bool wait) noexcept {
    if(gate_depth) {if(wait) {++gate_depth;held=true;} return;}
    if(wait) AcquireSRWLockExclusive(&mutex);
    else if(!TryAcquireSRWLockExclusive(&mutex)) return;
    ++gate_depth;held=true;
}
Gate::~Gate() {if(held && !--gate_depth) ReleaseSRWLockExclusive(&mutex);}
Operation::Operation(Context value,const void* owner) noexcept:context(value),host(owner),previous(current) {current=this;}
Operation::~Operation() {current=previous;}
bool subscribe(void* host,Observer observer) noexcept {
    if(!host || !observer) return false;
    Gate lock;
    for(auto& entry:observers) if(entry.host==host) return entry.observer==observer;
    for(auto& entry:observers) if(!entry.host) {entry={host,observer};++subscribers;return true;}
    return false;
}
void unsubscribe(void* host) noexcept {
    if(!host) return;
    Gate lock;
    for(auto& entry:observers) if(entry.host==host) {entry={};--subscribers;return;}
}
bool configured() noexcept {return subscribers.load()!=0;}
void halt() noexcept {Gate lock;if(!faulted.exchange(true)) ++generation;}
bool halted() noexcept {return faulted.load();}
uint64_t revision() noexcept {return generation.load(std::memory_order_acquire);}
unsigned teardown_depth() noexcept {return depth;}
bool call_vm_live() noexcept {
    return !faulted.load() && (!current || (current->interruption!=Interruption::closed && current->interruption!=Interruption::unknown));
}
bool call_owner_live() noexcept {return !faulted.load() && (!current || current->interruption==Interruption::none);}
void notify(bool close,uintptr_t global,uint64_t owner) noexcept {
    Gate lock;
    ++depth;++generation;
    if(!global) faulted=true;
    for(auto* operation=current;operation;operation=operation->previous) {
        if(!global) operation->interruption=Interruption::unknown;
        else if(operation->interruption!=Interruption::unknown && operation->context.global==global) {
            if(close) operation->interruption=Interruption::closed;
            else if(operation->context.owner==owner && operation->interruption==Interruption::none)
                operation->interruption=Interruption::owner;
        }
    }
    for(const auto& entry:observers) if(entry.observer) entry.observer(entry.host,{close,global,owner,current});
}
void cleanup_begin(uintptr_t global,uint64_t owner) noexcept {notify(false,global,owner);}
void close_begin(uintptr_t global) noexcept {notify(true,global,0);}
void teardown_end() noexcept {Gate lock;if(depth) --depth;}
#ifdef CRML_LUA_SESSION_TESTING
void reset_for_test() noexcept {Gate lock;observers={};subscribers=0;generation=0;depth=0;faulted=false;}
bool in_operation_for_test() noexcept {return current!=nullptr;}
#endif
}
