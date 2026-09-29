#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Internal ABI for the fingerprinted engine VM, not a public mod API.
namespace crml::engine::lua {
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
    int(*retain)(void*,int){};
    int(*fetch)(void*,int,int){};
    void(*release)(void*,int){};
};
struct Owner {uintptr_t world{};uint64_t entity{},revision{};};
struct Context {void* vm{};uintptr_t global{},world{};uint64_t owner{},revision{};};
struct Error {const char* kind{"none"};unsigned line{};};
struct Frame {
    uintptr_t context{},world{},global{},environment{};
    ptrdiff_t top{},base{},ci{};
    size_t size{};
    std::array<unsigned char,64*24> values{};
};
template<class T> T read(uintptr_t address) noexcept {
    T value;std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));return value;
}
Error error_details(void* vm,const char* label) noexcept;
bool capture(void* vm,Frame& frame,uintptr_t spare=96) noexcept;
bool same_frame(const Frame& a,const Frame& b,bool top) noexcept;
bool valid_owner(Owner owner) noexcept;
}
