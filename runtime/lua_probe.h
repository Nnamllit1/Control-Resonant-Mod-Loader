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
    void(*push_entity)(void*,uint64_t,int){};
    void(*new_table)(void*,int,int){};
    void(*push_value)(void*,int){};
    void(*set_field)(void*,int,const char*){};
    void(*readonly)(void*,int,int){};
    int(*set_metatable)(void*,int){};
    int(*raw_field)(void*,int,const char*){};
};
struct Owner { uintptr_t world{};uint64_t entity{}; };
// Only the fingerprint-gated boundary hook may provide this engine-thread call.
bool start(uintptr_t image,Call original) noexcept;
Owner before_call(void* vm,uintptr_t caller,int nargs,int results,int error) noexcept;
void after_call(void* vm,uintptr_t caller,int nargs,int results,int error,int status,Owner owner={});
void write(std::ostream& out);
void stop() noexcept;
#ifdef CRML_LUA_PROBE_TESTING
void configure(Api api,uintptr_t image);
#endif
}
