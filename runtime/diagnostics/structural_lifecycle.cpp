#include "diagnostics/structural_lifecycle.h"
#include "compatibility.h"
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <ostream>
#ifdef CRML_STRUCTURAL_LIFECYCLE_TESTING
#include <array>
#include <sstream>
#include <thread>
#endif

namespace crml::structural_lifecycle {
namespace {
// Three reviewed world wrappers agree on these eleven integer/pointer arguments.
using Flush=void(*)(void*,void*,void*,void*,void*,void*,void*,void*,void*,uint8_t,void*);
using Destroy=void(*)(void*);
Flush original_flush{};
Destroy original_destroy{};
constexpr uintptr_t flush_rva=0x1d74e80,destroy_rva=0x1d89b80;
constexpr unsigned char flush_bytes[]{0x48,0x8b,0xc4,0x4c,0x89,0x48,0x20,0x4c,0x89,0x40,0x18,0x48,0x89,0x50,0x10};
constexpr unsigned char destroy_bytes[]{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20};
constexpr uint64_t closed=uint64_t{1}<<63;
std::atomic<uint64_t> admission{closed};
std::atomic<bool> installed{};
bool attempted{};
// Default sequential consistency is intentional: overlapping hook intervals must
// never appear quiet because counter loads/stores were reordered.
std::atomic<uint64_t> sequence{},flushes{},teardowns{},unwinds{};

bool enter(bool teardown) noexcept {
    auto state=admission.load();
    do {if((state&closed) || state==closed-1) return false;}
    while(!admission.compare_exchange_weak(state,state+1));
    ++sequence;
    if(teardown) ++teardowns;else ++flushes;
    return true;
}
void leave(bool returned) noexcept {
    if(!returned) ++unwinds;
    ++sequence;
    admission.fetch_sub(1);
}
void flush_hook(void* commands,void* world,void* registry,void* tags,void* generations,
                void* locations,void* archetypes,void* jobs,void* auxiliary,uint8_t mode,void* output) {
    if(!enter(false)) {
        original_flush(commands,world,registry,tags,generations,locations,archetypes,jobs,auxiliary,mode,output);
        return;
    }
    bool returned=false;
    __try {
        original_flush(commands,world,registry,tags,generations,locations,archetypes,jobs,auxiliary,mode,output);
        returned=true;
    } __finally {leave(returned);}
}
void destroy_hook(void* world) {
    if(!enter(true)) {original_destroy(world);return;}
    bool returned=false;
    __try {original_destroy(world);returned=true;}
    __finally {leave(returned);}
}
bool attach(uintptr_t image,uintptr_t rva,void* hook,void** original) noexcept {
    const auto target=reinterpret_cast<void*>(image+rva);
    if(MH_CreateHook(target,hook,original)!=MH_OK) return false;
    if(MH_EnableHook(target)==MH_OK) return true;
    MH_RemoveHook(target);return false;
}
}
bool start() noexcept {
    // Bootstrap and stop are serialized by the observer's control thread.
    // Calls already inside native code at attachment are not observed. This is
    // capture coverage, not a scheduler barrier or permission to mutate state.
    if(attempted || !compatibility::reviewed_build ||
       compatibility::engine_profile!=compatibility::EngineProfile::october_patch) return false;
    attempted=true;
    const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!image || !compatibility::matches(reinterpret_cast<void*>(image+flush_rva),flush_bytes,sizeof(flush_bytes)) ||
       !compatibility::matches(reinterpret_cast<void*>(image+destroy_rva),destroy_bytes,sizeof(destroy_bytes))) return false;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    if(!attach(image,flush_rva,reinterpret_cast<void*>(&flush_hook),reinterpret_cast<void**>(&original_flush))) return false;
    // Once enabled, hook code stays pinned even after partial installation or
    // capture shutdown; disabled observers only forward their original calls.
    if(!attach(image,destroy_rva,reinterpret_cast<void*>(&destroy_hook),reinterpret_cast<void**>(&original_destroy))) return false;
    installed.store(true);admission.store(0);return true;
}
void stop() noexcept {admission.fetch_or(closed);}
Snapshot snapshot() noexcept {
    Snapshot result{};
    const auto first=sequence.load();
    const auto state=admission.load();
    result.available=!(state&closed);
    result.active=state&~closed;
    result.sequence=sequence.load();
    // Include a crossing interval as busy even if it ended during these loads.
    if(first!=result.sequence && !result.active) result.active=1;
    result.flushes=flushes.load();result.teardowns=teardowns.load();result.unwinds=unwinds.load();
    return result;
}
bool quiet_interval(const Snapshot& before,const Snapshot& after) noexcept {
    return before.available && after.available && !before.active && !after.active && before.sequence==after.sequence;
}
void report(std::ostream& out) {
    const auto state=snapshot();const auto flags=out.flags();
    out<<std::dec<<std::noshowpos<<std::noboolalpha
       <<"Capability structural_lifecycle: {\"schema\":1,\"type\":\"totals\",\"status\":\""
       <<(state.available?"active":state.active?"draining":installed.load()?"stopped":"unavailable")
       <<"\",\"time_ms\":"<<GetTickCount64()<<",\"pending\":"<<state.active<<",\"available\":"
       <<(state.available?"true":"false")<<",\"sequence\":"<<state.sequence<<",\"active\":"<<state.active
       <<",\"flushes\":"<<state.flushes<<",\"teardowns\":"<<state.teardowns<<",\"unwinds\":"<<state.unwinds<<"}\n";
    out.flags(flags);
}
#ifdef CRML_STRUCTURAL_LIFECYCLE_TESTING
namespace testing {
void arm() {admission=0;}
bool begin_flush() {return enter(false);}
void end_flush() {leave(true);}
namespace {
constexpr DWORD failure=0xe043524d;
std::array<uintptr_t,11> seen{};
unsigned calls{};
bool fail{},nested{};
Snapshot during{},during_destroy{};
void destroy_stub(void* world) {
    seen[0]=reinterpret_cast<uintptr_t>(world);++calls;during=snapshot();during_destroy=during;
    if(fail) RaiseException(failure,0,0,nullptr);
}
void flush_stub(void* a,void* b,void* c,void* d,void* e,void* f,void* g,void* h,void* i,uint8_t j,void* k) {
    seen={reinterpret_cast<uintptr_t>(a),reinterpret_cast<uintptr_t>(b),reinterpret_cast<uintptr_t>(c),
          reinterpret_cast<uintptr_t>(d),reinterpret_cast<uintptr_t>(e),reinterpret_cast<uintptr_t>(f),
          reinterpret_cast<uintptr_t>(g),reinterpret_cast<uintptr_t>(h),reinterpret_cast<uintptr_t>(i),j,reinterpret_cast<uintptr_t>(k)};
    ++calls;during=snapshot();
    if(nested) {destroy_hook(b);during=snapshot();}
    if(fail) RaiseException(failure,0,0,nullptr);
}
bool invoke(bool destroy) {
    __try {
        if(destroy) destroy_hook(reinterpret_cast<void*>(77));
        else flush_hook(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),reinterpret_cast<void*>(3),
                        reinterpret_cast<void*>(4),reinterpret_cast<void*>(5),reinterpret_cast<void*>(6),
                        reinterpret_cast<void*>(7),reinterpret_cast<void*>(8),reinterpret_cast<void*>(9),0xfe,reinterpret_cast<void*>(11));
    } __except(GetExceptionCode()==failure?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
}
bool callthrough() {
    original_flush=&flush_stub;original_destroy=&destroy_stub;admission=0;
    const auto before=snapshot();
    if(invoke(false) || seen!=std::array<uintptr_t,11>{1,2,3,4,5,6,7,8,9,0xfe,11} || during.active!=1) return false;
    auto after=snapshot();
    if(after.active || after.sequence!=before.sequence+2 || quiet_interval(before,after) || !quiet_interval(after,after)) return false;
    nested=true;invoke(false);nested=false;
    if(during.active!=1 || during_destroy.active!=2 || snapshot().active || snapshot().teardowns!=before.teardowns+1) return false;
    fail=true;
    const bool flush_unwind=invoke(false),destroy_unwind=invoke(true);fail=false;
    after=snapshot();
    if(!flush_unwind || !destroy_unwind || after.active || after.unwinds!=before.unwinds+2) return false;
    stop();const auto stopped=snapshot();const auto called=calls;
    invoke(false);invoke(true);
    return calls==called+2 && !snapshot().available && snapshot().sequence==stopped.sequence && !quiet_interval(stopped,stopped);
}
bool concurrent() {
    admission=0;
    const auto before=snapshot();
    std::atomic<unsigned> ready{};std::atomic<bool> release{};
    auto work=[&] {enter(false);++ready;while(!release.load()) std::this_thread::yield();leave(true);};
    std::thread a(work),b(work);
    while(ready.load()!=2) std::this_thread::yield();
    const auto overlap=snapshot();stop();const auto stopped=snapshot();
    const auto late_admitted=enter(false);if(late_admitted) leave(true);
    release=true;a.join();b.join();
    const auto after=snapshot();stop();
    return overlap.active==2 && !quiet_interval(before,overlap) && !quiet_interval(overlap,after) &&
        !stopped.available && stopped.active==2 && !late_admitted &&
        !after.active && after.sequence==before.sequence+4;
}
bool reporting() {
    std::ostringstream out;out<<std::hex<<std::showpos;const auto flags=out.flags();report(out);
    return out.flags()==flags && out.str().find("\"available\":false")!=std::string::npos &&
           out.str().find("\"active\":0")!=std::string::npos &&
           out.str().find("\"status\":\"unavailable\"")!=std::string::npos &&
           out.str().find("\"pending\":0")!=std::string::npos &&
           out.str().find("\"time_ms\":")!=std::string::npos;
}
}
#endif
}
