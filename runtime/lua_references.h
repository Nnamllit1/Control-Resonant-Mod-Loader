#pragma once
#include <cstdint>
#include <iosfwd>

namespace crml::probe::lua::references {
// Observes only the self-authored persistent-event listener. Never calls Lua.
bool start(uintptr_t image) noexcept;
void close(uintptr_t global) noexcept;
void write(std::ostream& out);
#ifdef CRML_LUA_REFERENCES_TESTING
namespace testing {
void configure(int(*retain)(void*,int),void(*release)(void*,int),uintptr_t image);
int retain(void* vm,int index,uintptr_t caller);
void release(void* vm,int reference,uintptr_t caller);
}
#endif
}
