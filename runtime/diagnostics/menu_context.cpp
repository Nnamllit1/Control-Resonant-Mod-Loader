#include "diagnostics/menu_context.h"
#include "compatibility.h"
#include <MinHook.h>
#include <intrin.h>
#ifdef CRML_MENU_CONTEXT_TESTING
#include <stdexcept>
#endif

namespace crml::menu_context {
namespace {
uint8_t byte(uintptr_t at) noexcept {return *reinterpret_cast<const volatile uint8_t*>(at);}
Cache cache;
using Scan=void(*)(const void*,const void*,uint8_t*,uint8_t*);
Scan original{};
uintptr_t expected_caller{};
bool installed{};
struct Addresses {uintptr_t scan{},caller{};};
Addresses addresses() noexcept {
    if(!compatibility::reviewed_build) return {};
    switch(compatibility::engine_profile) {
    case compatibility::EngineProfile::october_update:
    case compatibility::EngineProfile::october_patch:
    case compatibility::EngineProfile::october_hotfix:return {0x17e2bb0,0x17e2842};
    default:return {};
    }
}
void dispatch_from(uintptr_t caller,const void* query,const void* stacks,uint8_t* matched,uint8_t* last) {
    const auto epoch=cache.begin();
    const auto a=reinterpret_cast<uintptr_t>(matched),b=reinterpret_cast<uintptr_t>(last);
    const auto contract=prepare(caller,expected_caller,a,b);
    original(query,stacks,matched,last);
    // No catch/finally: an engine unwind must not publish partial output. All
    // four borrowed arguments die here; only two copied bytes can enter cache.
    cache.finish(epoch,contract,a,b,GetTickCount64());
}
void dispatch(const void* query,const void* stacks,uint8_t* matched,uint8_t* last) {
    dispatch_from(reinterpret_cast<uintptr_t>(_ReturnAddress()),query,stacks,matched,last);
}
}
Read prepare(uintptr_t caller,uintptr_t expected,uintptr_t matched,uintptr_t last) noexcept {
    if(!expected || caller!=expected) return Read::caller;
    if(!matched || !last || matched==last) return Read::arguments;
    Read result=Read::ok;
    __try {if(byte(matched) || byte(last)) result=Read::initial_output;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {result=Read::memory;}
    return result;
}
Read inspect(uintptr_t matched,uintptr_t last,Snapshot& out) noexcept {
    out={};
    if(!matched || !last || matched==last) return Read::arguments;
    Read result=Read::ok;
    __try {
        const auto first=byte(matched),second=byte(last);
        if(first>1) result=Read::output_value;
        else {out.matched_active_context=first!=0;out.last_match_flag_5c=second;out.thread=GetCurrentThreadId();}
    }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {result=Read::memory;}
    if(result!=Read::ok) out={};
    return result;
}
uint64_t Cache::begin() noexcept {
    const auto token=epoch_.fetch_add(1,std::memory_order_acq_rel)+1;
    if(in_flight_.fetch_add(1,std::memory_order_acq_rel)) {
        // Poison every invocation in an overlap, whichever one returns last.
        // Loading the current epoch also covers interleaved begin operations.
        const auto through=epoch_.load(std::memory_order_acquire);
        auto previous=overlap_epoch_.load(std::memory_order_acquire);
        while(previous<through && !overlap_epoch_.compare_exchange_weak(previous,through,std::memory_order_acq_rel)) {}
    }
    return token;
}
void Cache::finish(uint64_t token,Read contract,uintptr_t matched,uintptr_t last,uint64_t now) noexcept {
    Snapshot copied{};
    const auto eligible=[&] {
        return token==epoch_.load(std::memory_order_acquire) &&
            token>overlap_epoch_.load(std::memory_order_acquire) && in_flight_.load(std::memory_order_acquire)==1;
    };
    Read status=contract;
    if(eligible() && status==Read::ok) status=inspect(matched,last,copied);
    if(TryAcquireSRWLockExclusive(&lock_)) {
        if(eligible()) {
            state_=copied;status_=status;sampled_at_=now;published_epoch_=token;published_=true;
        } else {state_={};published_=false;}
        ReleaseSRWLockExclusive(&lock_);
    }
    in_flight_.fetch_sub(1,std::memory_order_release);
}
bool Cache::read(Snapshot& out,Read& status,uint64_t now) noexcept {
    out={};status=Read::unavailable;
    if(!TryAcquireSRWLockShared(&lock_)) return false;
    const auto epoch=epoch_.load(std::memory_order_acquire);
    bool ready=false;
    if(in_flight_.load(std::memory_order_acquire)) status=Read::in_flight;
    else if(epoch && overlap_epoch_.load(std::memory_order_acquire)>=epoch) status=Read::overlap;
    else if(published_ && published_epoch_==epoch) {
        if(now<sampled_at_ || now-sampled_at_>100) status=Read::stale;
        else {out=state_;status=status_;ready=true;}
    }
    if(ready && (in_flight_.load(std::memory_order_acquire) || epoch_.load(std::memory_order_acquire)!=epoch)) {
        out={};status=Read::in_flight;ready=false;
    }
    ReleaseSRWLockShared(&lock_);
    return ready;
}
bool start() noexcept {
    if(installed) return true;
    const auto profile=addresses();
    if(!profile.scan) return false;
    const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* target=reinterpret_cast<void*>(image+profile.scan);
    constexpr unsigned char bytes[]{
        0x4c,0x89,0x4c,0x24,0x20,0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,
        0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,
        0x24,0xe1,0x48,0x81,0xec,0xf8,0,0,0};
    if(!compatibility::matches(target,bytes,sizeof(bytes))) return false;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    if(MH_CreateHook(target,reinterpret_cast<void*>(&dispatch),reinterpret_cast<void**>(&original))!=MH_OK) return false;
    expected_caller=image+profile.caller;
    if(MH_EnableHook(target)!=MH_OK) {MH_RemoveHook(target);return false;}
    installed=true;return true;
}
bool read(Snapshot& out,Read& status,uint64_t now) noexcept {return cache.read(out,status,now);}
const char* name(Read status) noexcept {
    switch(status) {
    case Read::ok:return "ok";case Read::arguments:return "arguments";case Read::caller:return "unreviewed_caller";
    case Read::initial_output:return "initial_output";case Read::output_value:return "output_value";
    case Read::memory:return "memory";case Read::in_flight:return "in_flight";case Read::overlap:return "overlap";
    case Read::stale:return "stale";default:return "unavailable";
    }
}
#ifdef CRML_MENU_CONTEXT_TESTING
bool test_addresses(uintptr_t& scan,uintptr_t& caller) noexcept {
    const auto profile=addresses();scan=profile.scan;caller=profile.caller;
    return scan!=0;
}
bool test_dispatch() {
    static const void* seen[4]{};
    original=+[](const void* query,const void* stacks,uint8_t* matched,uint8_t* last) {
        seen[0]=query;seen[1]=stacks;seen[2]=matched;seen[3]=last;*matched=1;*last=0xa5;
    };
    expected_caller=0x1234;
    uint8_t outputs[2]{};
    const auto* query=reinterpret_cast<const void*>(1);
    const auto* stacks=reinterpret_cast<const void*>(2);
    dispatch_from(expected_caller,query,stacks,&outputs[0],&outputs[1]);
    Snapshot out{};Read status{};
    bool ok=seen[0]==query && seen[1]==stacks && seen[2]==&outputs[0] && seen[3]==&outputs[1] &&
        outputs[0]==1 && outputs[1]==0xa5 && read(out,status,GetTickCount64()) && status==Read::ok &&
        out.matched_active_context && out.last_match_flag_5c==0xa5;
    outputs[0]=outputs[1]=0;
    dispatch_from(expected_caller+1,query,stacks,&outputs[0],&outputs[1]);
    ok=ok && outputs[0]==1 && outputs[1]==0xa5 && read(out,status,GetTickCount64()) && status==Read::caller && !out.thread;
    dispatch_from(expected_caller,query,stacks,&outputs[0],&outputs[1]);
    ok=ok && outputs[0]==1 && outputs[1]==0xa5 && read(out,status,GetTickCount64()) && status==Read::initial_output;
    original=+[](const void*,const void*,uint8_t* matched,uint8_t*) {*matched=1;throw std::runtime_error("engine unwind");};
    outputs[0]=outputs[1]=0;
    bool propagated=false;
    try {dispatch_from(expected_caller,query,stacks,&outputs[0],&outputs[1]);}
    catch(const std::runtime_error&) {propagated=true;}
    return ok && propagated && outputs[0]==1 && !read(out,status,GetTickCount64()) && status==Read::in_flight;
}
#endif
}
