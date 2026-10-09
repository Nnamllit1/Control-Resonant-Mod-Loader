#include "dialogue_observation.h"
#include <Windows.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace crml::dialogue {
namespace {
template<class T> T load(const unsigned char* p,size_t n) noexcept {
    T value;std::memcpy(&value,p+n,sizeof(value));return value;
}
bool utf8(const char* p,size_t n) noexcept {
    for(size_t i=0;i<n;) {
        const auto c=static_cast<unsigned char>(p[i++]);
        if(!c) return false;
        if(c<0x80) continue; // Native formatting may contain newline/control markup.
        uint32_t cp{};size_t tail{};uint32_t minimum{};
        if(c>=0xc2 && c<=0xdf) {cp=c&31;tail=1;minimum=0x80;}
        else if(c>=0xe0 && c<=0xef) {cp=c&15;tail=2;minimum=0x800;}
        else if(c>=0xf0 && c<=0xf4) {cp=c&7;tail=3;minimum=0x10000;}
        else return false;
        if(n-i<tail) return false;
        while(tail--) {const auto d=static_cast<unsigned char>(p[i++]);if((d&0xc0)!=0x80) return false;cp=(cp<<6)|(d&63);}
        if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return false;
    }
    return true;
}
Read record(const unsigned char* row,Observation& out) noexcept {
    out.playback_key=load<uint32_t>(row,0);
    out.segment=load<uint32_t>(row,0x50);
    out.forced=load<uint8_t>(row,0x54);
    const auto length=load<uint64_t>(row,0x40),capacity=load<uint64_t>(row,0x48);
    if(out.forced>1 || length>4096 || length>capacity || (capacity<16 && capacity!=15)) return Read::range;
    if(!length) return Read::empty;
    if(out.segment==UINT32_MAX) return Read::identity;
    const auto* text=capacity>15?load<const char*>(row,0x30):reinterpret_cast<const char*>(row+0x30);
    if(!text || reinterpret_cast<uintptr_t>(text)>(std::numeric_limits<uintptr_t>::max)()-length) return Read::range;
    std::memcpy(out.text,text,static_cast<size_t>(length));out.length=static_cast<uint32_t>(length);
    if(text[length]!=0 || !utf8(out.text,out.length)) return Read::encoding;
    return Read::ok;
}
Read selected(const void* ui,const void* source,Observation& out) noexcept {
    if(!ui || !source) return Read::arguments;
    const auto* p=static_cast<const unsigned char*>(source);
    const auto result=record(static_cast<const unsigned char*>(ui),out);
    if(result!=Read::ok) return result;
    if(out.playback_key!=load<uint32_t>(p,0x4c)) return Read::identity;
    out.allocation=load<uint32_t>(p,0x60);
    out.duration=load<float>(p,0x50);out.elapsed=load<float>(p,0x54);
    if(!std::isfinite(out.duration) || !std::isfinite(out.elapsed) || out.elapsed<0 || out.duration<=out.elapsed) return Read::range;
    return Read::ok;
}
Read published(const void* state,uint32_t slot,Observation& out) noexcept {
    if(!state || slot>=3) return Read::arguments;
    const auto* p=static_cast<const unsigned char*>(state);
    const auto size=load<uint32_t>(p,8);
    // The native producer publishes exactly the first three records. Bound the
    // vector metadata as well; malformed counts must not look like empty slots.
    if(size>64) return Read::range;
    if(slot>=size) return Read::empty;
    const auto base=load<uintptr_t>(p,0);
    if(!base || base>(std::numeric_limits<uintptr_t>::max)()-0x58*3) return Read::range;
    return record(reinterpret_cast<const unsigned char*>(base+slot*0x58),out);
}
}
Read inspect_selected(const void* row,const void* source,Observation& out) noexcept {
    out={};Read result{};
    __try {result=selected(row,source,out);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {result=Read::memory;}
    if(result!=Read::ok) out={};return result;
}
Read inspect_published(const void* state,uint32_t slot,Observation& out) noexcept {
    out={};Read result{};
    __try {result=published(state,slot,out);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {result=Read::memory;}
    if(result!=Read::ok) out={};return result;
}
uint64_t text_hash(const Observation& out) noexcept {
    if(out.length>4096) return 0;
    uint64_t hash=14695981039346656037ULL;
    for(uint32_t i=0;i<out.length;++i) {hash^=static_cast<unsigned char>(out.text[i]);hash*=1099511628211ULL;}
    return hash;
}
}
