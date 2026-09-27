#pragma once
#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace crml::probe::lua {
using Call = int(*)(void*,int,int,int);
using Body = void(*)(void*,void*);
struct Api {
    int(*load)(void*,const char*,const char*,size_t,int){};
    int(*protect)(void*,Body,void*,ptrdiff_t,ptrdiff_t){};
    void(*settop)(void*,int){};
    Call call{};
};
// Only the fingerprint-gated boundary hook may provide this engine-thread call.
bool start(uintptr_t image,Call original) noexcept;
void after_call(void* vm,uintptr_t caller,int nargs,int results,int error,int status);
void write(std::ostream& out);
void stop() noexcept;
#ifdef CRML_LUA_PROBE_TESTING
void configure(Api api,uintptr_t image);
#endif
}
