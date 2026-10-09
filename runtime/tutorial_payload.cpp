#include "tutorial_payload.h"
#include "compatibility.h"
#include <Windows.h>
#include <array>
#include <cstring>

namespace crml::tutorial {
bool PayloadNative::bind(uintptr_t image,PayloadNative& out) noexcept {
    out={};
    if(!compatibility::reviewed_build || compatibility::engine_profile!=compatibility::EngineProfile::october_patch ||
       image!=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))) return false;
    struct Site {uintptr_t rva;std::array<unsigned char,32> bytes;};
    constexpr Site sites[]{
        {0x31de4f0,{64,83,72,131,236,48,197,248,16,2,76,139,74,8,69,51,192,72,141,84,36,32,72,139,217,197,248,17,68,36,32,232}},
        {0x31de030,{72,131,236,40,76,139,193,139,17,139,194,193,232,24,129,226,255,255,255,0,193,226,3,61,128,0,0,0,115,9,141,12}},
        {0x20371d0,{0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x98,0xf0,0xff,0xff,0xb8,0x68,0x10,0x00,0x00,0xe8,0xcd}},
        {0x204e0d0,{72,137,92,36,8,72,137,116,36,16,87,72,131,236,32,72,139,249,72,139,25,139,65,8,72,105,240,136,0,0,0,72}}
    };
    for(const auto& site:sites)
        if(!compatibility::matches(reinterpret_cast<void*>(image+site.rva),site.bytes.data(),site.bytes.size())) return false;
    out={image,reinterpret_cast<decltype(assign)>(image+0x31de4f0),reinterpret_cast<decltype(destroy_string)>(image+0x31de030),
        reinterpret_cast<decltype(build)>(image+0x20371d0),reinterpret_cast<decltype(destroy_pages)>(image+0x204e0d0)};
    return true;
}
namespace {
bool make_native(const PayloadNative& native,std::string_view title,std::string_view body,PageVector& out) {
    const auto assign=native.assign;const auto destroy_string=native.destroy_string;
    const auto build=native.build;const auto destroy_pages=native.destroy_pages;
    if(out.data || out.count || out.capacity || !assign || !destroy_string || !build || !destroy_pages ||
       title.size()>512 || body.empty() || body.size()>8192) return false;
    alignas(16) unsigned char source[0x90]{};
    constexpr uint64_t string40=0x04000004,string24=0x02000002;
    std::memcpy(source+0x10,&string40,8);std::memcpy(source+0x38,&string40,8);
    std::memcpy(source+0x74,&string24,8);source[0x70]=1;
    const NativeStringView heading{title.data(),title.size()},message{body.data(),body.size()};
    const PageSource pages{1,source};
    __try {
        assign(source+0x10,&heading);assign(source+0x38,&message);
        build(&out,&pages);
        destroy_string(source+0x74);destroy_string(source+0x38);destroy_string(source+0x10);
        return out.data && out.count==1 && out.capacity>=1;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
}
bool PayloadNative::make(std::string_view title,std::string_view body,PageVector& out) const noexcept {
    try {return make_native(*this,title,body,out);}
    catch(...) {return false;}
}
bool PayloadNative::destroy(PageVector& pages) const noexcept {
    if(!destroy_pages) return false;
    __try {destroy_pages(&pages);return !pages.data && !pages.count && !pages.capacity;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
}
