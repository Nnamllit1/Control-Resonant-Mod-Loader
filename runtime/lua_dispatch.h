#pragma once
#include "lua_vm.h"

namespace crml::engine::lua::dispatch {
// One gate covers all CRML execution and teardown notifications for engine Lua.
// Nested execution skips; nested bookkeeping on the same thread is allowed.
struct Gate {
    bool held{};
    explicit Gate(bool wait=true) noexcept;
    ~Gate();
    Gate(const Gate&)=delete;
    Gate& operator=(const Gate&)=delete;
};
enum class Interruption {none,owner,closed,unknown};
struct Operation {
    Context context;
    const void* host;
    Interruption interruption{};
    Operation(Context,const void* host) noexcept;
    ~Operation();
    Operation(const Operation&)=delete;
    Operation& operator=(const Operation&)=delete;
private:
    Operation* previous;
    friend void notify(bool,uintptr_t,uint64_t) noexcept;
};
struct Event {
    bool close;
    uintptr_t global;
    uint64_t owner;
    const Operation* operation;
};
using Observer=void(*)(void*,Event) noexcept;
// Observers must have stable lifetimes, perform bookkeeping only, and never
// subscribe/unsubscribe or call Lua from inside their notification callback.
bool subscribe(void* host,Observer observer) noexcept;
void unsubscribe(void* host) noexcept;
bool configured() noexcept;
// An unsafe native outcome disables all CRML Lua hosts for this process. Engine
// teardown notifications still run, but execution cannot resume automatically.
void halt() noexcept;
bool halted() noexcept;
uint64_t revision() noexcept;
unsigned teardown_depth() noexcept; // Requires the gate.
bool call_vm_live() noexcept;
bool call_owner_live() noexcept;
void cleanup_begin(uintptr_t global,uint64_t owner) noexcept;
void close_begin(uintptr_t global) noexcept;
void teardown_end() noexcept;
#ifdef CRML_LUA_SESSION_TESTING
// Test setup only, with no operations or teardown in flight.
void reset_for_test() noexcept;
bool in_operation_for_test() noexcept;
#endif
}
