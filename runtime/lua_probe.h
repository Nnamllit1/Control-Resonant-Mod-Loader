#pragma once
#include "lua_vm.h"
#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace crml::probe::lua {
using engine::lua::Call;
using engine::lua::Body;
using engine::lua::Api;
using engine::lua::Owner;
// Only the fingerprint-gated boundary hook may provide this engine-thread call.
bool start(uintptr_t image,Call original) noexcept;
bool start_persistent(uintptr_t image) noexcept;
Owner before_call(void* vm,uintptr_t caller,int nargs,int results,int error) noexcept;
void after_call(void* vm,uintptr_t caller,int nargs,int results,int error,int status,Owner owner={});
void write(std::ostream& out);
void stop() noexcept;
#ifdef CRML_LUA_PROBE_TESTING
void configure(Api api,uintptr_t image);
void configure_persistent(bool events=false);
#endif
}
