#pragma once
#include <cstdint>
#include <iosfwd>

namespace crml::probe::lua::lifetime {
// Read-only diagnostics. These identifiers are not handles for engine access.
bool start(uintptr_t image) noexcept;
bool active() noexcept;
void observe(void* vm,uintptr_t world,uint64_t owner) noexcept;
void write(std::ostream& out);
void stop() noexcept;
#ifdef CRML_LUA_LIFETIME_TESTING
namespace testing {
using Close=void(*)(void*);
using Cleanup=void(*)(void*,void*,void*,void*,void*,void*,void*,uint64_t);
void configure(Close close,Cleanup cleanup);
void close(void* vm);
void cleanup(void* state,void* b,void* c,void* d,void* e,void* f,void* g,uint64_t owner);
}
#endif
}
