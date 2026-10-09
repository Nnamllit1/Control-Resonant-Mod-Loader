#include "diagnostics/input_context.h"
#include "compatibility.h"
#include <MinHook.h>
#ifdef CRML_INPUT_CONTEXT_TESTING
#include <stdexcept>
#endif

namespace crml::input_context {
namespace {
template<class T> T load(uintptr_t at) noexcept {
    // Each recheck must issue a fresh scalar read. Volatile prevents compiler
    // reuse; it does not synchronize with the engine or extend object lifetime.
    return *reinterpret_cast<const volatile T*>(at);
}
Read inspect_inner(uintptr_t object,uintptr_t expected,uintptr_t vtable,Snapshot& out) noexcept {
    if(!object || !expected || !vtable) return Read::arguments;
    if(object!=expected) return Read::identity;
    const auto view=object+0x148;
    const auto connection=load<uintptr_t>(object);
    const auto raw=load<uint32_t>(object+0x570);
    const auto derived=load<uint16_t>(object+0x578);
    out.raw_flags=raw;out.derived_flags=derived;out.thread=GetCurrentThreadId();
    // The engine getter selects an empty fallback while disconnected; stale
    // embedded view data is not evidence of an active player input view.
    if(!connection) {
        return load<uintptr_t>(object) || load<uint32_t>(object+0x570)!=raw ||
            load<uint16_t>(object+0x578)!=derived?Read::changed:Read::ok;
    }
    out.connected=true;
    const auto backend=load<uintptr_t>(view);
    const auto generation=load<uint64_t>(view+8);
    const auto predicate=load<uintptr_t>(view+0x90);
    const auto source_a=load<uintptr_t>(view+0x40),source_b=load<uintptr_t>(view+0x48),source_c=load<uintptr_t>(view+0x50);
    const auto current_generation=backend?load<uint64_t>(backend+0x5028):0;
    out.backend=backend!=0;
    out.generation_matches=backend && (generation==UINT64_MAX || generation==current_generation);
    // Match the reader's short-circuit order. A stale-generation view need not
    // have a live predicate/source object and must not cause those dereferences.
    if(out.generation_matches && (!predicate || load<uintptr_t>(predicate)!=vtable || load<uintptr_t>(predicate+8)!=object))
        return Read::predicate;
    out.predicate_enabled=out.generation_matches && (derived&2)!=0;
    out.source_checked=out.predicate_enabled;
    const bool inspect_source_c=out.source_checked && !source_a && !source_b && source_c;
    const bool source_c_enabled=inspect_source_c && load<uint8_t>(source_c+0x10)!=0;
    out.source=out.source_checked && (source_a || source_b || source_c_enabled);
    // Rechecks detect observed churn, not an engine lock or an ownership grant.
    if(load<uintptr_t>(object)!=connection || load<uint32_t>(object+0x570)!=raw || load<uint16_t>(object+0x578)!=derived ||
       load<uintptr_t>(view)!=backend || load<uint64_t>(view+8)!=generation ||
       load<uintptr_t>(view+0x90)!=predicate || load<uintptr_t>(view+0x40)!=source_a ||
       load<uintptr_t>(view+0x48)!=source_b || load<uintptr_t>(view+0x50)!=source_c ||
       (backend && load<uint64_t>(backend+0x5028)!=current_generation) ||
       (out.generation_matches && (load<uintptr_t>(predicate)!=vtable || load<uintptr_t>(predicate+8)!=object)) ||
       (inspect_source_c && (load<uint8_t>(source_c+0x10)!=0)!=source_c_enabled)) return Read::changed;
    return Read::ok;
}
Cache cache;
using Update=void(*)(void*);
Update original{};
uintptr_t expected_object{},expected_vtable{};
bool installed{}; // Bootstrap worker only; hook state is fixed before enable.
struct Addresses {uintptr_t update{},object{},predicate_vtable{};};
Addresses addresses() noexcept {
    if(!compatibility::reviewed_build) return {};
    switch(compatibility::engine_profile) {
    case compatibility::EngineProfile::october_update:return {0x29d5b80,0x5de85b0,0x4dbf5b0};
    case compatibility::EngineProfile::october_hotfix:return {0x29d5b60,0x5df0560,0x4dc78d0};
    case compatibility::EngineProfile::october_patch:return {0x29d5c10,0x5d0f580,0x4d1db30};
    default:return {};
    }
}
void dispatch(void* object) {
    cache.begin();
    original(object);
    // Unwinding is not completion: no sample is published, and the cache remains
    // unavailable rather than presenting a partially updated object as current.
    cache.finish(reinterpret_cast<uintptr_t>(object),expected_object,expected_vtable,GetTickCount64());
}
}
Read inspect(uintptr_t object,uintptr_t expected,uintptr_t vtable,Snapshot& out) noexcept {
    out={};Read result{};
    __try {result=inspect_inner(object,expected,vtable,out);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {result=Read::memory;}
    if(result!=Read::ok) out={};
    return result;
}
void Cache::begin() noexcept {
    in_flight_.fetch_add(1,std::memory_order_acq_rel);
    epoch_.fetch_add(1,std::memory_order_acq_rel);
}
void Cache::finish(uintptr_t object,uintptr_t expected,uintptr_t vtable,uint64_t now) noexcept {
    const auto epoch=epoch_.load(std::memory_order_acquire);
    Snapshot copied{};
    const auto status=in_flight_.load(std::memory_order_acquire)==1?inspect(object,expected,vtable,copied):Read::unavailable;
    if(TryAcquireSRWLockExclusive(&lock_)) {
        if(epoch_.load(std::memory_order_acquire)==epoch && in_flight_.load(std::memory_order_acquire)==1) {
            state_=copied;status_=status;sampled_at_=now;published_epoch_=epoch;published_=true;
        } else {state_={};published_=false;}
        ReleaseSRWLockExclusive(&lock_);
    }
    in_flight_.fetch_sub(1,std::memory_order_release);
}
bool Cache::read(Snapshot& out,Read& status,uint64_t now) noexcept {
    out={};status=Read::unavailable;
    if(!TryAcquireSRWLockShared(&lock_)) return false;
    bool ready=published_ && !in_flight_.load(std::memory_order_acquire) &&
        published_epoch_==epoch_.load(std::memory_order_acquire) && now>=sampled_at_ && now-sampled_at_<=100;
    if(ready) {
        out=state_;status=status_;
        ready=!in_flight_.load(std::memory_order_acquire) && published_epoch_==epoch_.load(std::memory_order_acquire);
        if(!ready) {out={};status=Read::unavailable;}
    }
    ReleaseSRWLockShared(&lock_);
    return ready;
}
bool start() noexcept {
    if(installed) return true;
    // Independently reviewed profiles; no relocation guess for other builds.
    const auto profile=addresses();
    if(!profile.update) return false;
    const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* target=reinterpret_cast<void*>(image+profile.update);
    constexpr unsigned char bytes[]{0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55,0x57,0x41,0x56,0x48,0x8d,0xac,0x24,0xa0,0xf3,0xff,0xff,0x48,0x81,0xec,0x60,0x0d,0,0,0x48,0x8b,0xf1};
    if(!compatibility::matches(target,bytes,sizeof(bytes))) return false;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    if(MH_CreateHook(target,reinterpret_cast<void*>(&dispatch),reinterpret_cast<void**>(&original))!=MH_OK) return false;
    expected_object=image+profile.object;expected_vtable=image+profile.predicate_vtable;
    if(MH_EnableHook(target)!=MH_OK) {MH_RemoveHook(target);return false;}
    installed=true;return true;
}
bool read(Snapshot& out,Read& status,uint64_t now) noexcept {return cache.read(out,status,now);}
const char* name(Read status) noexcept {
    switch(status) {
    case Read::ok:return "ok";case Read::arguments:return "arguments";case Read::identity:return "identity";
    case Read::predicate:return "predicate";case Read::changed:return "changed";case Read::memory:return "memory";
    default:return "unavailable";
    }
}
#ifdef CRML_INPUT_CONTEXT_TESTING
bool test_addresses(uintptr_t& update,uintptr_t& object,uintptr_t& predicate_vtable) noexcept {
    const auto profile=addresses();
    update=profile.update;object=profile.object;predicate_vtable=profile.predicate_vtable;
    return update!=0;
}
bool test_dispatch() {
    static void* seen{};
    original=+[](void* object) {seen=object;};
    dispatch(reinterpret_cast<void*>(1));
    Snapshot out{};Read status{};
    bool ok=seen==reinterpret_cast<void*>(1) && read(out,status,GetTickCount64()) && status==Read::arguments;
    original=+[](void*) {throw std::runtime_error("engine unwind");};
    bool propagated=false;
    try {dispatch(nullptr);}catch(const std::runtime_error&) {propagated=true;}
    return ok && propagated && !read(out,status,GetTickCount64());
}
#endif
}
