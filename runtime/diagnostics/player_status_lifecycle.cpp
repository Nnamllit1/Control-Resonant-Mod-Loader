#include "diagnostics/player_status_lifecycle.h"
#include "compatibility.h"
#include <Windows.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <ostream>
#include <utility>
#ifdef CRML_PLAYER_STATUS_LIFECYCLE_TESTING
#include <sstream>
#include <thread>
#endif

namespace crml::player_status_lifecycle {
namespace {
using Initialize=void(*)(void*,uint32_t);
using Copy=void(*)(const void*,void*,uint32_t);
// All unique callback pairs from the reviewed Status descriptor registrations.
// No assumption about which registration happens first in a particular world.
constexpr std::array<uintptr_t,13> initializers{
    0x1e24d80,0x1e6ede0,0x1fb6b90,0x202e6c0,0x203e0c0,0x20d1f90,0x21e7ff0,
    0x227f9e0,0x243e780,0x280d8f0,0x28ecae0,0x28f88d0,0x29140c0};
constexpr std::array<uintptr_t,13> copiers{
    0x1e24d90,0x1e6edf0,0x1fb6ba0,0x202e6d0,0x203e0d0,0x20d1fa0,0x21e8000,
    0x227f9f0,0x243e790,0x280d900,0x28ecaf0,0x28f88e0,0x29140d0};
constexpr unsigned char initialize_bytes[]{0x8b,0xc2,0x66,0xc7,0x04,0x41,0x01,0x00,0xc3};
constexpr unsigned char copy_bytes[]{0x0f,0xb7,0x01,0x45,0x8b,0xc0,0x66,0x42,0x89,0x04,0x42,0xc3};
std::array<Initialize,13> original_initialize{};
std::array<Copy,13> original_copy{};
constexpr uint64_t closed=uint64_t{1}<<63;
std::atomic<uint64_t> admission{closed},sequence{},initializations{},copies{},unwinds{};
bool attempted{},installed{};

bool enter(bool copy) noexcept {
    auto state=admission.load();
    do {if((state&closed) || state==closed-1) return false;}
    while(!admission.compare_exchange_weak(state,state+1));
    ++sequence;
    if(copy) ++copies;else ++initializations;
    return true;
}
void leave(bool returned) noexcept {
    if(!returned) ++unwinds;
    ++sequence;admission.fetch_sub(1);
}
template<size_t I> void initialize_hook(void* storage,uint32_t row) {
    if(!enter(false)) {original_initialize[I](storage,row);return;}
    bool returned=false;
    __try {original_initialize[I](storage,row);returned=true;}
    __finally {leave(returned);}
}
template<size_t I> void copy_hook(const void* source,void* destination,uint32_t row) {
    if(!enter(true)) {original_copy[I](source,destination,row);return;}
    bool returned=false;
    __try {original_copy[I](source,destination,row);returned=true;}
    __finally {leave(returned);}
}
template<size_t... I> constexpr auto initialize_hooks(std::index_sequence<I...>) {
    return std::array<Initialize,sizeof...(I)>{&initialize_hook<I>...};
}
template<size_t... I> constexpr auto copy_hooks(std::index_sequence<I...>) {
    return std::array<Copy,sizeof...(I)>{&copy_hook<I>...};
}
constexpr auto init_hooks=initialize_hooks(std::make_index_sequence<13>{});
constexpr auto clone_hooks=copy_hooks(std::make_index_sequence<13>{});
bool install_at(const std::array<void*,26>& targets) noexcept {
    for(size_t i=0;i<initializers.size();++i)
        if(!compatibility::matches(targets[2*i],initialize_bytes,sizeof(initialize_bytes)) ||
           !compatibility::matches(targets[2*i+1],copy_bytes,sizeof(copy_bytes))) return false;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    std::array<void*,26> created{};
    size_t count{};
    // Create every trampoline first. None of the hooks may publish coverage
    // while only part of the inventory has been installed.
    for(size_t i=0;i<initializers.size();++i) {
        const auto a=targets[2*i],b=targets[2*i+1];
        if(MH_CreateHook(a,reinterpret_cast<void*>(init_hooks[i]),reinterpret_cast<void**>(&original_initialize[i]))!=MH_OK) break;
        created[count++]=a;
        if(MH_CreateHook(b,reinterpret_cast<void*>(clone_hooks[i]),reinterpret_cast<void**>(&original_copy[i]))!=MH_OK) break;
        created[count++]=b;
    }
    if(count!=created.size()) {
        for(size_t i=0;i<count;++i) MH_RemoveHook(created[i]);
        return false;
    }
    for(size_t i=0;i<count;++i) {
        if(MH_EnableHook(created[i])==MH_OK) continue;
        // Enabled hooks stay pinned in passthrough. Removing them while a
        // thread could be in their entry would be unsafe.
        for(size_t j=i;j<count;++j) MH_RemoveHook(created[j]);
        return false;
    }
    installed=true;admission.store(0);return true;
}
}

bool start() noexcept {
    if(attempted || !compatibility::reviewed_build ||
       compatibility::engine_profile!=compatibility::EngineProfile::october_patch) return false;
    attempted=true;
    const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!image) return false;
    std::array<void*,26> targets{};
    for(size_t i=0;i<initializers.size();++i) {
        targets[2*i]=reinterpret_cast<void*>(image+initializers[i]);
        targets[2*i+1]=reinterpret_cast<void*>(image+copiers[i]);
    }
    return install_at(targets);
}
void stop() noexcept {admission.fetch_or(closed);}
Snapshot snapshot() noexcept {
    Snapshot result{};
    const auto first=sequence.load(),state=admission.load();
    result.available=!(state&closed);result.active=state&~closed;
    result.sequence=sequence.load();
    if(first!=result.sequence && !result.active) result.active=1;
    result.initializations=initializations.load();result.copies=copies.load();result.unwinds=unwinds.load();
    return result;
}
bool unchanged(const Snapshot& before,const Snapshot& after) noexcept {
    return before.available && after.available && !before.active && !after.active && before.sequence==after.sequence;
}
void report(std::ostream& out) {
    const auto state=snapshot();const auto flags=out.flags();
    out<<std::dec<<std::noshowpos<<std::noboolalpha
       <<"Capability status_lifecycle: {\"schema\":1,\"type\":\"totals\",\"status\":\""
       <<(state.available?"active":state.active?"draining":installed?"stopped":"unavailable")
       <<"\",\"time_ms\":"<<GetTickCount64()<<",\"pending\":"<<state.active
       <<",\"available\":"<<(state.available?"true":"false")<<",\"sequence\":"<<state.sequence
       <<",\"active\":"<<state.active<<",\"initializations\":"<<state.initializations
       <<",\"copies\":"<<state.copies<<",\"unwinds\":"<<state.unwinds<<"}\n";
    out.flags(flags);
}
#ifdef CRML_PLAYER_STATUS_LIFECYCLE_TESTING
namespace testing {
void arm() {admission=0;}
void complete_copy() {sequence.fetch_add(2);++copies;}
namespace {
constexpr DWORD failure=0xe043524d;
const void* seen_source{};void* seen_destination{};uint32_t seen_row{};
unsigned calls{};bool fail{};Snapshot during{};
void initialize_stub(void* destination,uint32_t row) {
    ++calls;seen_source=nullptr;seen_destination=destination;seen_row=row;during=snapshot();
    if(fail) RaiseException(failure,0,0,nullptr);
}
void copy_stub(const void* source,void* destination,uint32_t row) {
    ++calls;seen_source=source;seen_destination=destination;seen_row=row;during=snapshot();
    if(fail) RaiseException(failure,0,0,nullptr);
}
bool catch_unwind(bool copy) {
    __try {if(copy) clone_hooks[12](nullptr,nullptr,0);else init_hooks[12](nullptr,0);}
    __except(GetExceptionCode()==failure?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
}
bool callthrough() {
    original_initialize.fill(initialize_stub);original_copy.fill(copy_stub);admission=0;
    const auto first=snapshot();
    int source{},destination{};
    for(size_t i=0;i<init_hooks.size();++i) {
        init_hooks[i](&destination,0xf1234567u);
        if(seen_source || seen_destination!=&destination || seen_row!=0xf1234567u || during.active!=1) return false;
        clone_hooks[i](&source,&destination,0xe1234567u);
        if(seen_source!=&source || seen_destination!=&destination || seen_row!=0xe1234567u || during.active!=1) return false;
    }
    const auto after=snapshot();
    if(after.active || after.sequence!=first.sequence+52 || after.initializations!=first.initializations+13 ||
       after.copies!=first.copies+13 || unchanged(first,after) || !unchanged(after,after)) return false;
    fail=true;const bool a=catch_unwind(false),b=catch_unwind(true);fail=false;
    if(!a || !b || snapshot().active || snapshot().unwinds!=after.unwinds+2) return false;
    stop();const auto stopped=snapshot();const auto old_calls=calls;
    init_hooks[0](&destination,1);clone_hooks[0](&source,&destination,2);
    return calls==old_calls+2 && snapshot().sequence==stopped.sequence && !unchanged(stopped,stopped);
}
bool overlapping() {
    admission=0;const auto before=snapshot();
    std::atomic<unsigned> ready{};std::atomic<bool> release{};
    auto work=[&] {enter(true);++ready;while(!release.load()) std::this_thread::yield();leave(true);};
    std::thread a(work),b(work);while(ready.load()!=2) std::this_thread::yield();
    const auto overlap=snapshot();stop();const bool admitted=enter(false);
    if(admitted) leave(true);
    release=true;a.join();b.join();
    return overlap.active==2 && !admitted && !snapshot().active && !unchanged(before,overlap) && !snapshot().available;
}
bool reporting() {
    std::ostringstream out;out<<std::hex<<std::showpos;const auto flags=out.flags();report(out);
    return out.flags()==flags && out.str().find("\"available\":false")!=std::string::npos &&
           out.str().find("\"active\":0")!=std::string::npos;
}
bool installation() {
    // Self-contained instruction fixtures implement initialize/copy on uint16
    // arrays. Exercise the real MinHook trampolines without starting the game.
    constexpr size_t bytes=26*4096;
    auto* code=static_cast<unsigned char*>(VirtualAlloc(nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!code) return false;
    std::array<void*,26> targets{};
    for(size_t i=0;i<13;++i) {
        targets[2*i]=code+(2*i)*4096;targets[2*i+1]=code+(2*i+1)*4096;
        std::memcpy(targets[2*i],initialize_bytes,sizeof(initialize_bytes));
        std::memcpy(targets[2*i+1],copy_bytes,sizeof(copy_bytes));
    }
    DWORD previous{};
    bool ok=VirtualProtect(code,bytes,PAGE_EXECUTE_READ,&previous)!=0;
    FlushInstructionCache(GetCurrentProcess(),code,bytes);
    stop();
    auto bad=targets;bad[24]=targets[0]; // Late duplicate forces create failure.
    ok=ok && !install_at(bad) && !snapshot().available;
    ok=ok && install_at(targets) && snapshot().available;
    if(ok) {
        const auto before=snapshot();
        std::array<uint16_t,3> destination{};const uint16_t source=0x0704;
        for(size_t i=0;i<13;++i) {
            destination={0xbeef,0xbeef,0xbeef};
            reinterpret_cast<Initialize>(targets[2*i])(destination.data(),1);
            ok=ok && destination==std::array<uint16_t,3>{0xbeef,1,0xbeef};
            reinterpret_cast<Copy>(targets[2*i+1])(&source,destination.data(),2);
            ok=ok && destination==std::array<uint16_t,3>{0xbeef,1,source};
        }
        ok=ok && snapshot().sequence==before.sequence+52;
        stop();const auto stopped=snapshot();
        reinterpret_cast<Initialize>(targets[0])(destination.data(),0);
        ok=ok && destination[0]==1 && snapshot().sequence==stopped.sequence;
    }
    stop();
    // Only this single-threaded test owns these pages and hooks. Production
    // never detaches enabled hooks or frees executable trampolines.
    MH_Uninitialize();VirtualFree(code,0,MEM_RELEASE);
    return ok;
}
}
#endif
}
