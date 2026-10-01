#include "media_service.h"
#include "compatibility.h"
#include <MinHook.h>
#include <intrin.h>

namespace crml::media {
namespace {
using Active=bool(*)(void*);
using Elapsed=uint32_t(*)(void*);
Active original{};
Elapsed elapsed{};
uintptr_t image{};
// Copy only the logical resource name from the original startup metadata.
// Resolved movie paths can contain installation-specific filesystem paths.
bool sample(void* object,char (&name)[256],uint32_t& milliseconds) noexcept {
    __try {
        milliseconds=elapsed(object);
        const char* path=*reinterpret_cast<const char* const*>(image+0x5a0b018);
        if(!path) return false;
        for(size_t i=0;i<sizeof(name);++i) {name[i]=path[i];if(!name[i]) return i!=0;}
        return false;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool route_at(void* object,uintptr_t caller,bool active,uint64_t now) {
    // Both sites are inside the original boot loop after initialization.
    // Only the top-of-loop query controls its normal exit and cleanup path.
    if(caller!=image+0x3d4c0d && caller!=image+0x3d4c8d) return active;
    auto& service=process_service();
    if(!service.capabilities()) return active;
    // Startup can poll this query millions of times while pumping work. Keep
    // native reads, name copying and publication to once per 16 ms per site.
    // The original query still runs on every call. Natural completion always
    // invalidates immediately; only presentation sampling is throttled.
    // Independent clocks prevent the observational input site from starving
    // the top-of-loop site, which alone may consume a queued skip.
    struct SampleClock {uintptr_t object{};uint64_t sample{};};
    static thread_local SampleClock clocks[2]{};
    auto& clock=clocks[caller==image+0x3d4c0d?0:1];
    const auto identity=reinterpret_cast<uintptr_t>(object);
    if(active && clock.object==identity && now>=clock.sample && now-clock.sample<16) return active;
    clock.object=active?identity:0;clock.sample=now;
    char name[256]{};uint32_t milliseconds{};
    const bool readable=!active || sample(object,name,milliseconds);
    if(!readable) {service.observe_at(0,false,false,0,{},0,now,false);return active;}
    const bool skip=service.observe_at(reinterpret_cast<uintptr_t>(object),active,true,milliseconds,
        active?std::string_view(name):std::string_view{},CRML_MEDIA_MAPPED_NAME,now,caller==image+0x3d4c0d);
    return active && !skip;
}
__declspec(noinline) bool query(void* object) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool active=original(object);
    return route_at(object,caller,active,GetTickCount64());
}
}
std::string start() {
    if(original) return "Media adapter already initialized";
    if(!compatibility::reviewed_build) return "Media adapter unavailable: executable differs from the reviewed build";
    image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const unsigned char active_bytes[]{0x48,0x8b,0x01,0x48,0x85,0xc0,0x74,0x12,0x80,0x79,0x30,0x00,0x75,0x09,0xf6,0x80,0xad,0,0,0,4,0x75,3,0xb0};
    const unsigned char elapsed_bytes[]{0x48,0x8b,0x09,0x48,0x85,0xc9,0x0f,0x85,0x54,0x1a,0x5a,0x01,0x33,0xc0,0xc3};
    auto* target=reinterpret_cast<void*>(image+0x188cef0);
    if(!compatibility::matches(target,active_bytes,sizeof(active_bytes)) ||
       !compatibility::matches(reinterpret_cast<void*>(image+0x188cf20),elapsed_bytes,sizeof(elapsed_bytes)))
        return "Media adapter unavailable: playback methods differ";
    // Validate original readiness branch and both calls before interpreting a
    // return address as authorization to end startup playback.
    const unsigned char ready[]{0x40,0x84,0xed,0x74,0x10};
    const unsigned char call_skip[]{0xe8,0xe3,0x82,0x4b,0x01};
    const unsigned char call_input[]{0xe8,0x63,0x82,0x4b,0x01};
    if(!compatibility::matches(reinterpret_cast<void*>(image+0x3d4c00),ready,sizeof(ready)) ||
       !compatibility::matches(reinterpret_cast<void*>(image+0x3d4c08),call_skip,sizeof(call_skip)) ||
       !compatibility::matches(reinterpret_cast<void*>(image+0x3d4c88),call_input,sizeof(call_input)))
        return "Media adapter unavailable: startup skip gate differs";
    elapsed=reinterpret_cast<Elapsed>(image+0x188cf20);
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return "Media adapter unavailable: hook initialization failed";
    if(MH_CreateHook(target,reinterpret_cast<void*>(&query),reinterpret_cast<void**>(&original))!=MH_OK)
        return "Media adapter unavailable: playback hook conflict";
    if(MH_EnableHook(target)!=MH_OK) {MH_RemoveHook(target);original=nullptr;return "Media adapter unavailable: playback hook failed";}
    process_service().enable(true);
    return "Media adapter armed; awaiting initialized startup playback";
}
void stop() noexcept {process_service().enable(false);}
}
