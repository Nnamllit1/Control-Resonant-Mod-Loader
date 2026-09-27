#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace crml::probe::script_origin {
struct Frame {
    std::array<char,96> source{};
    std::array<char,64> function{};
    uint32_t defined_line{}, prototype{};
};
struct Snapshot {
    bool valid{}, truncated{};
    unsigned count{};
    std::array<Frame,4> frames{};
};
namespace detail {
template<class T> T read(uintptr_t at) noexcept { T v{};std::memcpy(&v,reinterpret_cast<const void*>(at),sizeof(v));return v; }
template<size_t N> bool label(uintptr_t string,std::array<char,N>& out,bool basename) noexcept {
    if(!string) return true; // Debug labels may be stripped.
    if(read<uint8_t>(string)!=5) return false;
    const auto size=read<uint32_t>(string+0x14);
    if(size>4096) return false;
    const auto* bytes=reinterpret_cast<const char*>(string+0x18);
    uint32_t begin{};
    if(basename) for(uint32_t i=0;i<size;++i) if(bytes[i]=='/' || bytes[i]=='\\' || bytes[i]=='@') begin=i+1;
    // Never serialize a partial path, inline chunk contents or arbitrary VM text.
    if(size-begin>=N) return false;
    for(uint32_t i=begin;i<size;++i) {
        const char c=bytes[i];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='.' || c=='-')) return false;
    }
    std::memcpy(out.data(),bytes+begin,size-begin);return true;
}
}
// Build-specific layout, independently traced in the fingerprint-gated engine.
// Only call on the VM's owning thread while its native binding is active.
// No VM calls, allocation, retained engine pointers, or changes to VM state.
inline Snapshot capture(void* state) noexcept {
    Snapshot out{};
    __try {
        if(!state) return out;
        const auto vm=reinterpret_cast<uintptr_t>(state);
        const auto current=detail::read<uintptr_t>(vm+0x20),base=detail::read<uintptr_t>(vm+0x40);
        if(!base || current<base || (current-base)%0x28 || (current-base)/0x28>1024) return out;
        const auto depth=(current-base)/0x28+1;
        for(uintptr_t i=0;i<depth && i<16;++i) {
            const auto value=detail::read<uintptr_t>(current-i*0x28+8);
            if(!value || detail::read<uint32_t>(value+0x10)!=7) continue;
            const auto closure=detail::read<uintptr_t>(value);
            if(!closure || detail::read<uint8_t>(closure)!=7) return {};
            if(detail::read<uint8_t>(closure+3)) continue;
            if(out.count==out.frames.size()) {out.truncated=true;break;}
            const auto proto=detail::read<uintptr_t>(closure+0x18);
            if(!proto) return {};
            Frame frame{};
            // Invalid/stripped labels stay empty; numeric prototype identity remains.
            detail::label(detail::read<uintptr_t>(proto+0x58),frame.source,true);
            detail::label(detail::read<uintptr_t>(proto+0x60),frame.function,false);
            frame.defined_line=detail::read<uint32_t>(proto+0xa4);
            frame.prototype=detail::read<uint32_t>(proto+0xa8);
            out.frames[out.count++]=frame;
        }
        if(depth>16) out.truncated=true;
        out.valid=true;return out;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return {};}
}
}
