#pragma once
#include <cstdint>
#include <iosfwd>

namespace crml::probe::lua::session {
struct Context {void* vm{};uintptr_t global{},world{};uint64_t owner{},revision{};};
enum class Action {initialize, initialize_error, invoke, release};
struct Result {
    bool attempted{},restored{},released{};
    int reference{},status{};
    double value{};
};
using Execute=Result(*)(Context,Action,int);
// A bounded diagnostic, not an arbitrary-script scheduler. The embedded
// callbacks perform arithmetic only and cannot enter engine teardown.
void start(Execute execute) noexcept;
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
void reset_for_test(Execute execute);
#endif
}
