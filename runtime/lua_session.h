#pragma once
#include "lua_vm.h"
#include <cstdint>
#include <iosfwd>

namespace crml::probe::lua::session {
using engine::lua::Context;
enum class Action {initialize, initialize_error, invoke, release, unload, initialize_listener_error, initialize_rollback};
struct Result {
    bool attempted{},restored{},released{};
    int reference{},status{};
    double value{};
    bool shutdown{};
    bool deliberate_error{};
    unsigned error_line{};
    bool release_attempted{}; // Set before native unref, including an ambiguous failure.
};
using Execute=Result(*)(Context,Action,int);
// Native provider checks after calls that can reenter engine teardown. These
// describe the current protected operation only, never a saved VM pointer.
bool call_vm_live() noexcept;
bool call_owner_live() noexcept;
// A bounded diagnostic, not an arbitrary-script scheduler. The embedded
// callbacks perform arithmetic or dispatch their own bounded counting listener.
// Observed teardown can invalidate a call; this is not an arbitrary-script API
// or protection against closing a VM from within its own interpreter stack.
void start(Execute execute,bool events=false) noexcept;
bool configured() noexcept;
uint64_t revision() noexcept;
bool needs_calls() noexcept;
void tick(Context context,uint64_t now);
// These pairs bracket the original engine cleanup, including other threads.
// They never call Lua. Every begin must have an end after normal return.
void cleanup_begin(uintptr_t global,uint64_t owner) noexcept;
void cleanup_end() noexcept;
void close_begin(uintptr_t global) noexcept;
void close_end() noexcept;
void stop() noexcept;
void write(std::ostream& out);
#ifdef CRML_LUA_SESSION_TESTING
void reset_for_test(Execute execute,bool events=false);
#endif
}
