#include "diagnostics/navigation_observer.h"
#include "navigation_context.h"
#include "compatibility.h"
#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <ostream>

namespace crml::navigation_observer {
namespace {
using Loading=void(*)(void*,void*,void*,void*,void*,void*,void*,void*,void*,void*);
using Save=void(*)(void*,void*);
Loading original_loading{};
Save original_save{},original_restore{};
uintptr_t image{};
bool attempted{},installed{}; // Bootstrap worker only; hooks remain pinned after stop.
constexpr uintptr_t loading_rva=0x2549ea0,loading_return=0x254ec52;
constexpr uintptr_t save_rva=0x227b590,save_return=0x21633e2;
constexpr uintptr_t restore_rva=0x227b5e0,restore_wrapper=0x21633f0,restore_return=0x215cdb5;
constexpr uint64_t capture_ms=600000,closed=uint64_t{1}<<63;
constexpr uint32_t sample_limit=512;
constexpr size_t phase_count=3,queue_size=256;
constexpr std::array<const char*,phase_count> phases{"loading_request","save_coordinate","restore_coordinate"};
constexpr std::array<const char*,phase_count> prefixes{"loading","save","restore"};
constexpr std::array<uintptr_t,phase_count> returns{loading_return,save_return,restore_return};
constexpr unsigned char restore_bytes[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x7c,0x24,0x10,0x55,0x48,0x8d,0x6c,0x24,0xb0,0x48,0x81,0xec,0x50,0x01,0x00,0x00};
constexpr unsigned char loading_bytes[]{0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x18,0x48,0x89,0x78,0x20,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x68,0x88};
constexpr unsigned char save_bytes[]{0x48,0x8b,0x82,0x08,0x01,0x00,0x00,0x4c,0x8b,0xc9,0x4c,0x8b,0x00,0x48,0x8b,0x42,0x78};
struct Counts {std::atomic<uint64_t> calls{},caller{},sampled{},unwinds{},unreadable{},matched{},dropped{};};
std::array<Counts,phase_count> counts{};
std::array<std::atomic<uint64_t>,phase_count> next_sample{};
std::array<std::atomic<uint32_t>,phase_count> used{};
std::atomic<uint64_t> admission{closed},deadline{},sequence{};
struct Event {
    uint64_t sequence{},time_ms{};
    uint32_t thread{},phase{};
    navigation_context::Status status{navigation_context::Status::unavailable};
    navigation_context::Bundle before{},after{};
    navigation_context::SavedTransform source{};
    navigation_context::RestoreObservation restore{};
    bool sampled{},returned{},matched{},saveposition{};
};
SRWLOCK lock=SRWLOCK_INIT;
std::array<Event,queue_size> queue{};
size_t head{},size{};

bool admit(uint64_t now) noexcept {
    const auto until=deadline.load(std::memory_order_acquire);
    if(!until || now>=until) return false;
    auto state=admission.load(std::memory_order_acquire);
    while(!(state&closed) && state!=closed-1)
        if(admission.compare_exchange_weak(state,state+1,std::memory_order_acquire)) return true;
    return false;
}
bool should_sample(size_t phase,uint64_t now) noexcept {
    if(used[phase].load(std::memory_order_relaxed)>=sample_limit) return false;
    // Restore decisions may occur together during load. Preserve each until the
    // shared per-phase cap, rather than losing all but one to a time throttle.
    if(phase!=2) {
        auto previous=next_sample[phase].load(std::memory_order_relaxed);
        if(now<previous || !next_sample[phase].compare_exchange_strong(previous,now+250,std::memory_order_relaxed)) return false;
    }
    auto count=used[phase].load(std::memory_order_relaxed);
    while(count<sample_limit)
        if(used[phase].compare_exchange_weak(count,count+1,std::memory_order_relaxed)) return true;
    return false;
}
Event begin(size_t phase,uintptr_t caller,const void* arg,const void* saved=nullptr) noexcept {
    Event e{};e.phase=static_cast<uint32_t>(phase);e.time_ms=GetTickCount64();e.thread=GetCurrentThreadId();
    ++counts[phase].calls;
    if(caller!=image+returns[phase]) {
        ++counts[phase].caller;return e;
    }
    if(!should_sample(phase,e.time_ms)) return e;
    e.sampled=true;e.sequence=sequence.fetch_add(1,std::memory_order_relaxed)+1;
    ++counts[phase].sampled;
    if(phase==2) {
        e.status=navigation_context::inspect_restore_source(saved,arg,e.restore);
        if(e.status==navigation_context::Status::ok)e.before=e.restore.current;
    } else if(phase==1) {
        e.status=navigation_context::inspect_save_source(arg,e.source);
        if(e.status==navigation_context::Status::ok)
            e.before={e.source.bundle,e.source.bundle_valid};
    } else e.status=navigation_context::inspect_bundle(arg,e.before);
    if(e.status!=navigation_context::Status::ok) ++counts[phase].unreadable;
    return e;
}
void finish(Event& e,const void* a,const void* b) noexcept {
    const auto phase=e.phase;
    if(!e.returned) ++counts[phase].unwinds;
    if(e.sampled && e.returned) {
        if(phase==2) {
            // Eligibility at entry is not proof of restored state or a stable
            // world identity. No destination is walked after native return.

        } else if(phase==1) {
            navigation_context::SavedTransform after{};
            const auto status=navigation_context::inspect_save_source(b,after);
            if(status==navigation_context::Status::ok) e.after={after.bundle,after.bundle_valid};
            else if(e.status==navigation_context::Status::ok) e.status=status;
            if(e.status==navigation_context::Status::ok) {
                const auto result=navigation_context::inspect_save_result(a,e.source);
                e.matched=result==navigation_context::Status::ok;
                e.saveposition=e.matched;
                if(e.matched) ++counts[phase].matched;
                else e.status=result;
            }
        } else {
            const auto status=navigation_context::inspect_bundle(a,e.after);
            if(status!=navigation_context::Status::ok && e.status==navigation_context::Status::ok) e.status=status;
        }
    }
    if(e.sampled) {
        if(!TryAcquireSRWLockExclusive(&lock)) ++counts[phase].dropped;
        else {
            if(size==queue_size) ++counts[phase].dropped;
            else {queue[(head+size)%queue_size]=e;++size;}
            ReleaseSRWLockExclusive(&lock);
        }
    }
    admission.fetch_sub(1,std::memory_order_release);
}
void loading_hook(void* a,void* b,void* c,void* d,void* e,void* f,void* g,void* h,void* i,void* j) {
    const auto now=GetTickCount64();
    if(!admit(now)) {original_loading(a,b,c,d,e,f,g,h,i,j);return;}
    auto sample=begin(0,reinterpret_cast<uintptr_t>(_ReturnAddress()),a);
    __try {original_loading(a,b,c,d,e,f,g,h,i,j);sample.returned=true;}
    __finally {finish(sample,a,nullptr);}
}
void save_hook(void* destination,void* view) {
    const auto now=GetTickCount64();
    if(!admit(now)) {original_save(destination,view);return;}
    auto sample=begin(1,reinterpret_cast<uintptr_t>(_ReturnAddress()),view);
    __try {original_save(destination,view);sample.returned=true;}
    __finally {finish(sample,destination,view);}
}
void restore_hook(void* saved,void* view) {
    const auto now=GetTickCount64();
    if(!admit(now)) {original_restore(saved,view);return;}
    auto sample=begin(2,reinterpret_cast<uintptr_t>(_ReturnAddress()),view,saved);
    __try {original_restore(saved,view);sample.returned=true;}
    __finally {finish(sample,saved,view);}
}
bool reviewed() noexcept {
    return compatibility::reviewed_build && compatibility::engine_profile==compatibility::EngineProfile::october_patch;
}
bool relative_matches(uintptr_t at,uintptr_t target,unsigned char opcode) noexcept {
    const auto displacement=static_cast<int32_t>(target-(at+5));
    return compatibility::matches(reinterpret_cast<void*>(at),&opcode,1) &&
        compatibility::matches(reinterpret_cast<void*>(at+1),&displacement,4);
}
bool call_matches(uintptr_t at,uintptr_t target) noexcept {return relative_matches(at,target,0xe8);}
void bundle_json(std::ostream& out,const navigation_context::Bundle& bundle) {
    out<<(bundle.valid?"true":"false");
}
void event_json(std::ostream& out,const Event& e) {
    const auto flags=out.flags();
    const auto precision=out.precision();
    out<<std::dec<<std::noshowpos<<std::noboolalpha;
    out<<"Capability location: {\"schema\":1,\"type\":\"sample\",\"sequence\":"<<e.sequence
       <<",\"time_ms\":"<<e.time_ms<<",\"phase\":\""<<phases[e.phase]
       <<"\",\"thread\":"<<e.thread<<",\"status\":\""<<(e.returned?navigation_context::name(e.status):"unwind")
       <<"\",\"before_valid\":";bundle_json(out,e.before);
    out<<",\"before_bundle\":\""<<e.before.value<<'"';
    if(e.phase!=2) {
        out<<",\"after_valid\":";bundle_json(out,e.after);
        out<<",\"after_bundle\":\""<<e.after.value<<'"';
    }
    if(e.phase==2) {
        out<<",\"restore_inputs_observed\":"<<(e.returned && e.status==navigation_context::Status::ok?"true":"false");
        if(e.returned && e.status==navigation_context::Status::ok)out<<",\"restore_reason\":\""<<navigation_context::name(e.restore.reason)
            <<"\",\"saved_bundle\":\""<<e.restore.saved.bundle<<"\"";
        out<<",\"returned\":"<<(e.returned?"true":"false");
    } else if(e.phase==1) {
        out<<",\"matched\":"<<(e.matched?"true":"false");
        if(e.saveposition) {
            out<<",\"saveposition\":["<<std::setprecision(9)
               <<e.source.transform[4]<<','<<e.source.transform[5]<<','<<e.source.transform[6]<<']';
        }
    }
    out<<"}\n";
    out.flags(flags);out.precision(precision);
}
void totals_json(std::ostream& out,uint64_t now,uint64_t pending,bool final) {
    const auto flags=out.flags();
    out<<std::dec<<std::noshowpos<<std::noboolalpha;
    out<<"Capability location: {\"schema\":1,\"type\":\"totals\",\"time_ms\":"<<now
       <<",\"capture_ms\":"<<capture_ms<<",\"status\":\""<<(final?"complete":active()?"active":"closing")
       <<"\",\"pending\":"<<pending;
    for(size_t p=0;p<phase_count;++p) {
        const auto* prefix=prefixes[p];
        out<<",\""<<prefix<<"_calls\":"<<counts[p].calls.load()
           <<",\""<<prefix<<"_caller_rejected\":"<<counts[p].caller.load()
           <<",\""<<prefix<<"_sampled\":"<<counts[p].sampled.load()
           <<",\""<<prefix<<"_unreadable\":"<<counts[p].unreadable.load()
           <<",\""<<prefix<<"_unwinds\":"<<counts[p].unwinds.load()
           <<",\""<<prefix<<"_matched\":"<<counts[p].matched.load()
           <<",\""<<prefix<<"_dropped\":"<<counts[p].dropped.load()
           <<",\""<<prefix<<"_sample_budget_used\":"<<used[p].load();
    }
    out<<"}\n";
    out.flags(flags);
}
}
bool start() noexcept {
    if(attempted || !reviewed()) return false;
    image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!image || !compatibility::matches(reinterpret_cast<void*>(image+loading_rva),loading_bytes,sizeof(loading_bytes)) ||
       !compatibility::matches(reinterpret_cast<void*>(image+save_rva),save_bytes,sizeof(save_bytes)) ||
       !call_matches(image+loading_return-5,image+loading_rva) ||
       !call_matches(image+save_return-5,image+save_rva) ||
       !compatibility::matches(reinterpret_cast<void*>(image+restore_rva),restore_bytes,sizeof(restore_bytes)) ||
       !call_matches(image+restore_return-5,image+restore_wrapper) ||
       !relative_matches(image+restore_wrapper,image+restore_rva,0xe9)) return false;
    attempted=true;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    const auto loading=reinterpret_cast<void*>(image+loading_rva);
    const auto save=reinterpret_cast<void*>(image+save_rva);
    if(MH_CreateHook(loading,reinterpret_cast<void*>(&loading_hook),reinterpret_cast<void**>(&original_loading))!=MH_OK) return false;
    if(MH_CreateHook(save,reinterpret_cast<void*>(&save_hook),reinterpret_cast<void**>(&original_save))!=MH_OK) {
        MH_RemoveHook(loading);return false;
    }
    const auto restore=reinterpret_cast<void*>(image+restore_rva);
    if(MH_CreateHook(restore,reinterpret_cast<void*>(&restore_hook),reinterpret_cast<void**>(&original_restore))!=MH_OK) {
        MH_RemoveHook(save);MH_RemoveHook(loading);return false;
    }
    // A partial enable leaves pinned pass-through trampolines. Admission is
    // closed until all hooks are installed and can never reopen after stop.
    if(MH_EnableHook(loading)!=MH_OK || MH_EnableHook(save)!=MH_OK || MH_EnableHook(restore)!=MH_OK) return false;
    installed=true;
    admission.store(0,std::memory_order_release);
    deadline.store(GetTickCount64()+capture_ms,std::memory_order_release);
    return true;
}
bool active() noexcept {const auto until=deadline.load(std::memory_order_acquire);return until && GetTickCount64()<until;}
void stop() noexcept {deadline.store(0,std::memory_order_release);admission.fetch_or(closed,std::memory_order_acq_rel);}
void poll(std::ostream& out) {
    static uint64_t last{};static bool finished{};
    if(!installed || finished) return;
    const auto now=GetTickCount64();
    const bool closing=!active();
    if(closing) stop();
    const auto pending=admission.load(std::memory_order_acquire)&~closed;
    const bool final=closing && !pending;
    if(last && now-last<1000 && !final) return;
    last=now;
    Event batch[32]{};
    for(size_t n=0;n<queue_size/32;++n) {
        size_t taken{};
        AcquireSRWLockExclusive(&lock);
        while(taken<std::size(batch) && size) {
            batch[taken++]=queue[head];head=(head+1)%queue_size;--size;
        }
        ReleaseSRWLockExclusive(&lock);
        for(size_t j=0;j<taken;++j) event_json(out,batch[j]);
        if(taken<std::size(batch)) break;
    }
    totals_json(out,now,pending,final);
    if(final) finished=true;
    out.flush();
}
#ifdef CRML_NAVIGATION_OBSERVER_TESTING
namespace testing {
namespace {
std::array<void*,10> seen_loading{};
void* seen_destination{};
void* seen_view{};
bool fail_loading{},fail_restore{};
void loading_stub(void* a,void* b,void* c,void* d,void* e,void* f,void* g,void* h,void* i,void* j) {
    seen_loading={a,b,c,d,e,f,g,h,i,j};
    if(fail_loading) RaiseException(0xe043524d,0,0,nullptr);
}
void save_stub(void* destination,void* view) {seen_destination=destination;seen_view=view;}
void restore_stub(void* saved,void* view) {
    seen_destination=saved;seen_view=view;
    if(fail_restore)RaiseException(0xe043524d,0,0,nullptr);
}
bool catch_restore() {
    __try {restore_hook(nullptr,nullptr);}
    __except(GetExceptionCode()==0xe043524d?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
bool catch_native() {
    __try {loading_hook(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr);}
    __except(GetExceptionCode()==0xe043524d?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
}
bool gate() noexcept {
    admission.store(closed);deadline.store(GetTickCount64()+10000);
    if(admit(GetTickCount64())) return false;
    admission.store(0);if(!admit(GetTickCount64())) return false;
    admission.fetch_or(closed);
    const bool ok=!admit(GetTickCount64()) && (admission.load()&~closed)==1;
    admission.fetch_sub(1);deadline.store(0);
    return ok && (admission.load()&~closed)==0;
}
bool callthrough() {
    original_loading=&loading_stub;original_save=&save_stub;original_restore=&restore_stub;
    image=0x10000000;
    admission.store(0);deadline.store(GetTickCount64()+10000);
    void* a[10]{};for(uintptr_t i=0;i<10;++i) a[i]=reinterpret_cast<void*>(i+1);
    loading_hook(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9]);
    bool ok=true;for(size_t i=0;i<10;++i) ok=ok && seen_loading[i]==a[i];
    save_hook(a[0],a[1]);ok=ok && seen_destination==a[0] && seen_view==a[1];
    restore_hook(a[2],a[3]);ok=ok && seen_destination==a[2] && seen_view==a[3];
    const auto before=counts[0].unwinds.load();
    fail_loading=true;ok=ok && catch_native();fail_loading=false;
    const auto restore_before=counts[2].unwinds.load();
    fail_restore=true;ok=ok && catch_restore();fail_restore=false;
    stop();
    restore_hook(a[4],a[5]);ok=ok && seen_destination==a[4] && seen_view==a[5];
    return ok && counts[0].unwinds.load()==before+1 && counts[2].unwinds.load()==restore_before+1 && (admission.load()&~closed)==0;
}
bool callers() noexcept {
    const auto base=uintptr_t{0x10000000};image=base;
    auto e=begin(0,base+loading_return+1,reinterpret_cast<void*>(1));
    auto restore=begin(2,base+restore_return+1,reinterpret_cast<void*>(1),reinterpret_cast<void*>(1));
    return !e.sampled && !restore.sampled && counts[0].caller.load() && counts[2].caller.load() &&
        static_cast<int32_t>(loading_rva-loading_return)==-0x4db2 &&
        static_cast<int32_t>(save_rva-save_return)==0x1181ae;
}
void emit(std::ostream& out) {
    Event e{};e.sequence=42;e.time_ms=123;e.thread=7;e.returned=true;e.phase=0;
    e.status=navigation_context::Status::ok;e.before={0,true};e.after={UINT64_MAX,true};event_json(out,e);
    e.phase=1;e.source.transform[4]=1.25f;e.source.transform[5]=-2.f;e.source.transform[6]=3.f;
    e.matched=true;e.saveposition=true;event_json(out,e);
    e.phase=2;e.restore.saved.bundle=UINT64_MAX;e.restore.reason=navigation_context::RestoreReason::bundle_mismatch;event_json(out,e);
    totals_json(out,124,0,true);
}
}
#endif
}
