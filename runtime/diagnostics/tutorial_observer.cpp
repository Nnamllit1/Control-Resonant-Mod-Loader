#include "diagnostics/tutorial_observer.h"
#include "compatibility.h"
#include "tutorial_panel.h"
#include <MinHook.h>
#include <intrin.h>
#include <limits>
#include <ostream>
#ifdef CRML_TUTORIAL_OBSERVER_TESTING
#include <sstream>
#include <stdexcept>
#include <thread>
#endif

namespace crml::tutorial_observer {
namespace {
constexpr size_t index(Phase p) noexcept {return static_cast<size_t>(p);}
uint64_t ticks() noexcept {LARGE_INTEGER n{};QueryPerformanceCounter(&n);return n.QuadPart;}
uint64_t mix(uint64_t n) noexcept {
    n^=n>>30;n*=0xbf58476d1ce4e5b9ULL;n^=n>>27;n*=0x94d049bb133111ebULL;return n^(n>>31);
}
struct Site {uintptr_t rva,caller;std::array<unsigned char,32> bytes;};
constexpr Site sites[]{
    {0x2037720,0x20442c5,{0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xe0,0x01,0x00,0x00,0xc5,0xf8,0x29}},
    {0x1fe9040,0x1fea159,{0x48,0x8b,0xc4,0x4c,0x89,0x48,0x20,0x4c,0x89,0x40,0x18,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x68,0xa9,0x48,0x81,0xec,0x98,0x00}},
    {0x1e61470,0x1e62743,{0x4c,0x89,0x4c,0x24,0x20,0x4c,0x89,0x44,0x24,0x18,0x55,0x53,0x56,0x57,0x41,0x55,0x41,0x57,0x48,0x8d,0x6c,0x24,0xe9,0x48,0x81,0xec,0xc8,0x00,0x00,0x00,0xc5,0xfb}},
    {0x1e60d50,0x1e629f9,{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x4c,0x89,0x74,0x24,0x20,0x55,0x48,0x8d,0x6c,0x24,0xa9,0x48,0x81,0xec,0xe0,0x00,0x00}},
    {0x20384a0,0x2042ed7,{0x4c,0x8b,0xdc,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xb8,0x00,0x00,0x00,0xc5,0xfc,0x10,0x09,0xc5,0xfc,0x10,0x51,0x20,0xc5}}
};
static_assert(std::size(sites)==phase_count);
constexpr Site dispatch_sites[]{
    {tutorial::request_dispatch_rva,0,{72,129,236,232,0,0,0,72,139,1,69,15,183,152,200,0,0,0,73,193,227,10,76,3,152,128,132,5,0,72,139,128}},
    {tutorial::request_job_rva,0,{64,87,72,129,236,0,1,0,0,76,139,1,51,255,15,183,194,73,139,76,192,80,72,133,201,116,10,65,139,132,128,72}},
    {tutorial::panel_dispatch_rva,0,{72,137,92,36,16,85,86,87,65,84,65,85,65,86,65,87,72,131,236,112,77,139,208,72,139,1,65,15,183,144,200,0}}
};
// The initializer also runs through an archetype job wrapper. Its aggregate
// builder resolves the same two component hashes in the same order.
constexpr uintptr_t initializer_job_return=0x2044d65;
constexpr uintptr_t requests_job_return=0x20445a7;
struct JobCaller {Phase phase;uintptr_t caller;};
constexpr JobCaller job_callers[]{{Phase::initialize,initializer_job_return},{Phase::requests,requests_job_return}};
constexpr uintptr_t reviewed_image_size=0x6301000;
uintptr_t expected_caller(Phase phase,uintptr_t caller,uintptr_t base) noexcept {
    if(!base || index(phase)>=phase_count) return 0;
    const auto primary=base+sites[index(phase)].caller;
    if(caller==primary) return primary;
    for(const auto& job:job_callers)
        if(phase==job.phase && caller==base+job.caller) return caller;
    return 0;
}
uint32_t caller_offset(uintptr_t caller,uintptr_t base) noexcept {
    return base && caller>=base && caller-base<reviewed_image_size?
        static_cast<uint32_t>(caller-base):UINT32_MAX;
}
Recorder recorder;
Admissions admissions;
std::atomic<uint64_t> deadline{};
std::array<std::atomic<uint64_t>,phase_count> next_sample{};
std::array<std::atomic<uint32_t>,phase_count> sample_count{};
uintptr_t image{};
uint64_t salt{},frequency{};
bool installed{};
using Init=void(*)(const void*,const void*);
using Hint=void(*)(const void*,void*,const void*,const void*,void*);
using Panel=void(*)(const void*,void*,const void*,const void*,void*,void*,void*);
using Events=void(*)(const void*,void*,void*,void*);
using Requests=void(*)(const void*);
Init original_init{};Hint original_hint{};Panel original_panel{};Events original_events{};
Requests original_requests{};
using Dispatch=void(*)(const void*,uint16_t,const void*);
Dispatch original_dispatch{},original_job{},original_panel_dispatch{};
tutorial::PanelNative panel_native{};

bool sample_at(Phase phase,uint64_t now) noexcept {
    auto& count=sample_count[index(phase)];
    if(count.load(std::memory_order_relaxed)>=1024) return false;
    auto& next=next_sample[index(phase)];auto previous=next.load(std::memory_order_relaxed);
    if(now<previous || !next.compare_exchange_strong(previous,now+250,std::memory_order_relaxed)) return false;
    auto used=count.load(std::memory_order_relaxed);
    while(used<1024)
        if(count.compare_exchange_weak(used,used+1,std::memory_order_relaxed)) return true;
    return false;
}

// Sampling only limits records/argument reads. Counts cover every call in the
// capture window. No engine-owned object is retained beyond this stack frame.
struct Span {
    Event event{};
    bool enabled{},sampled{};
    Span(Phase phase,uintptr_t caller,const void* query) noexcept {
        const auto now=GetTickCount64(),until=deadline.load(std::memory_order_acquire);
        if(!until || now>=until || !admissions.enter()) return;
        enabled=true;event=recorder.enter(phase,GetCurrentThreadId(),ticks());
        sampled=sample_at(phase,now);
        if(sampled) {
            event.caller_rva=caller_offset(caller,image);
            event.query=inspect(phase,caller,expected_caller(phase,caller,image),query,salt,event.object);
            if(phase==Phase::requests && event.query==Query::ok) {
                tutorial::Identity owner{};
                event.identity=tutorial::RequestScope::identify(query,owner);
                if(event.identity==tutorial::IdentityStatus::ok) {
                    event.world=mix(owner.world^salt);
                    event.entity=mix(owner.entity^event.world^salt);
                }
            }
        }
    }
    void finish() noexcept {
        if(!enabled) return;
        enabled=false;
        event.end=ticks();
        if(!sampled) event.sequence=0;
        recorder.leave(event);
        admissions.leave();
    }
    ~Span() {finish();}
};
// Keep native SEH cleanup separate from frames containing C++ destructors.
// Never handle/swallow an engine exception; only release our own observation.
template<class Call> void forward_observed(Span& span,Call call) {
    __try {call();span.event.returned=true;}
    __finally {span.finish();}
}
void init_hook(const void* a,const void* b) {
    Span span(Phase::initialize,reinterpret_cast<uintptr_t>(_ReturnAddress()),a);
    forward_observed(span,[&] {original_init(a,b);});
}
void hint_hook(const void* a,void* b,const void* c,const void* d,void* e) {
    Span span(Phase::hint_sync,reinterpret_cast<uintptr_t>(_ReturnAddress()),a);
    forward_observed(span,[&] {original_hint(a,b,c,d,e);});
}
void panel_hook(const void* a,void* b,const void* c,const void* d,void* e,void* f,void* g) {
    Span span(Phase::panel_sync,reinterpret_cast<uintptr_t>(_ReturnAddress()),a);
    forward_observed(span,[&] {original_panel(a,b,c,d,e,f,g);});
}
void events_hook(const void* a,void* b,void* c,void* d) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    Span span(Phase::panel_events,caller,a);
    if(span.sampled && span.event.query==Query::ok) {
        tutorial::Identity owner{};
        span.event.identity=tutorial::PanelScope::identify({a,b,c,d,caller},owner);
        if(span.event.identity==tutorial::IdentityStatus::ok) {
            span.event.world=mix(owner.world^salt);
            span.event.entity=mix(owner.entity^span.event.world^salt);
        }
    }
    forward_observed(span,[&] {original_events(a,b,c,d);});
}
void requests_hook(const void* a) {
    Span span(Phase::requests,reinterpret_cast<uintptr_t>(_ReturnAddress()),a);
    forward_observed(span,[&] {original_requests(a);});
}
void dispatch_hook(const void* view,uint16_t id,const void* system) {
    if(!active()) {original_dispatch(view,id,system);return;}
    tutorial::RequestScope::invoke(view,id,system,tutorial::Dispatch::direct,image,original_dispatch);
}
void job_hook(const void* view,uint16_t id,const void* system) {
    if(!active()) {original_job(view,id,system);return;}
    tutorial::RequestScope::invoke(view,id,system,tutorial::Dispatch::archetype,image,original_job);
}
void panel_dispatch_hook(const void* view,uint16_t id,const void* system) {
    if(!active()) {original_panel_dispatch(view,id,system);return;}
    tutorial::PanelScope::invoke(view,id,system,panel_native,original_panel_dispatch);
}
bool supported() noexcept {
    return compatibility::reviewed_build && compatibility::engine_profile==compatibility::EngineProfile::october_patch;
}
}
Query inspect(Phase phase,uintptr_t caller,uintptr_t expected,const void* query,uint64_t key,uint64_t& object) noexcept {
    object=0;
    if(!expected || caller!=expected) return Query::caller;
    if(index(phase)>=phase_count || !query) return Query::range;
    uintptr_t base{},row{};
    // Init/events have two component arrays, sync one, requests seven.
    // Copy only TutorialData base/index; never traverse a table or page here.
    __try {
        const auto* words=static_cast<const volatile uintptr_t*>(query);
        base=words[phase==Phase::requests?4:phase==Phase::initialize?1:0];
        row=words[phase==Phase::requests?8:(phase==Phase::initialize || phase==Phase::panel_events)?3:2];
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return Query::memory;}
    if(!base || row>(std::numeric_limits<uintptr_t>::max()-base)/0x70) return Query::range;
    object=mix((base+row*0x70)^key);return Query::ok;
}
Event Recorder::enter(Phase phase,uint32_t thread,uint64_t tick) noexcept {
    Event e{};e.phase=phase;e.thread=thread;e.begin=tick;
    e.sequence=sequence_.fetch_add(1,std::memory_order_relaxed)+1;
    auto& count=counts_[index(phase)];count.calls.fetch_add(1,std::memory_order_relaxed);
    count.active.fetch_add(1,std::memory_order_seq_cst);
    for(size_t i=0;i<phase_count;++i)
        if(counts_[i].active.load(std::memory_order_seq_cst)>(i==index(phase)?1u:0u)) e.overlap|=1u<<i;
    if(e.overlap) count.overlaps.fetch_add(1,std::memory_order_relaxed);
    return e;
}
void Recorder::leave(Event e) noexcept {
    auto& count=counts_[index(e.phase)];
    if(!e.returned) count.unwinds.fetch_add(1,std::memory_order_relaxed);
    if(e.sequence && e.query!=Query::ok) count.rejected.fetch_add(1,std::memory_order_relaxed);
    // Active covers observation overhead as well as the original engine call.
    count.active.fetch_sub(1,std::memory_order_seq_cst);
    if(!e.sequence) return;
    if(!TryAcquireSRWLockExclusive(&lock_)) {count.dropped.fetch_add(1,std::memory_order_relaxed);return;}
    if(size_==events_.size()) count.dropped.fetch_add(1,std::memory_order_relaxed);
    else {events_[(head_+size_)%events_.size()]=e;++size_;}
    ReleaseSRWLockExclusive(&lock_);
}
size_t Recorder::drain(Event* out,size_t capacity) noexcept {
    if(!out || !capacity || !TryAcquireSRWLockExclusive(&lock_)) return 0;
    size_t n=0;
    while(n<capacity && size_) {out[n++]=events_[head_];head_=(head_+1)%events_.size();--size_;}
    ReleaseSRWLockExclusive(&lock_);return n;
}
Counts Recorder::counts(Phase p) const noexcept {
    const auto& c=counts_[index(p)];
    return {c.calls.load(),c.overlaps.load(),c.unwinds.load(),c.rejected.load(),c.dropped.load(),c.active.load()};
}
bool start() noexcept {
    if(installed) return true;
    if(!supported()) return false;
    image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    // Bind before MinHook changes the checked dispatcher prologue. Capture
    // remains read-only: no observation calls close_owned or registers data.
    if(!tutorial::PanelNative::bind(image,panel_native)) return false;
    for(const auto& site:sites) {
        if(!compatibility::matches(reinterpret_cast<void*>(image+site.rva),site.bytes.data(),site.bytes.size())) return false;
        const auto* call=reinterpret_cast<const unsigned char*>(image+site.caller-5);
        const int32_t offset=static_cast<int32_t>(site.rva-site.caller);
        if(!compatibility::matches(call,"\xe8",1) || !compatibility::matches(call+1,&offset,4)) return false;
    }
    for(const auto& job:job_callers) {
        const auto* job_call=reinterpret_cast<const unsigned char*>(image+job.caller-5);
        const int32_t job_offset=static_cast<int32_t>(sites[index(job.phase)].rva-job.caller);
        if(!compatibility::matches(job_call,"\xe8",1) || !compatibility::matches(job_call+1,&job_offset,4)) return false;
    }
    for(const auto& site:dispatch_sites)
        if(!compatibility::matches(reinterpret_cast<void*>(image+site.rva),site.bytes.data(),site.bytes.size())) return false;
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    void* hooks[]{reinterpret_cast<void*>(&init_hook),reinterpret_cast<void*>(&hint_hook),reinterpret_cast<void*>(&panel_hook),reinterpret_cast<void*>(&events_hook),reinterpret_cast<void*>(&requests_hook),reinterpret_cast<void*>(&dispatch_hook),reinterpret_cast<void*>(&job_hook),reinterpret_cast<void*>(&panel_dispatch_hook)};
    void** originals[]{reinterpret_cast<void**>(&original_init),reinterpret_cast<void**>(&original_hint),reinterpret_cast<void**>(&original_panel),reinterpret_cast<void**>(&original_events),reinterpret_cast<void**>(&original_requests),reinterpret_cast<void**>(&original_dispatch),reinterpret_cast<void**>(&original_job),reinterpret_cast<void**>(&original_panel_dispatch)};
    uintptr_t targets[std::size(hooks)]{};
    static_assert(std::size(hooks)==phase_count+std::size(dispatch_sites) && std::size(originals)==std::size(hooks));
    for(size_t i=0;i<std::size(targets);++i)
        targets[i]=image+(i<phase_count?sites[i].rva:dispatch_sites[i-phase_count].rva);
    size_t created=0;
    for(;created<std::size(targets);++created)
        if(MH_CreateHook(reinterpret_cast<void*>(targets[created]),hooks[created],originals[created])!=MH_OK) break;
    if(created!=std::size(targets)) {
        while(created) MH_RemoveHook(reinterpret_cast<void*>(targets[--created]));
        return false;
    }
    // Enable only our hooks. Never apply MinHook's global queued operations.
    // On a partial enable failure retain trampolines: another thread may still
    // be returning through one. With deadline=0 these hooks are pass-through.
    for(const auto target:targets)
        if(MH_EnableHook(reinterpret_cast<void*>(target))!=MH_OK) return false;
    LARGE_INTEGER hz{};QueryPerformanceFrequency(&hz);frequency=hz.QuadPart;
    salt=mix(ticks()^GetCurrentProcessId());
    admissions.open();
    deadline.store(GetTickCount64()+600000,std::memory_order_release);
    installed=true;return true;
}
bool active() noexcept {const auto until=deadline.load(std::memory_order_acquire);return until && GetTickCount64()<until;}
void stop() noexcept {deadline.store(0,std::memory_order_release);admissions.close();}
void poll(std::ostream& log) {
    static uint64_t last{};static bool header{},finished{},closure_reported{};
    const auto now=GetTickCount64();
    if(!installed || finished) return;
    const bool closing=!active();
    if(closing) stop();
    // Snapshot before draining: after a closed gate reaches zero, every admitted
    // span has already queued its event or counted its drop. No late entry can
    // race a final report, even if it had read the previous deadline.
    const auto pending=admissions.pending();
    const bool final=closing && !pending;
    if(last && now-last<1000 && !final && (!closing || closure_reported)) return;
    last=now;
    if(!header) {
        log<<"Tutorial observation: schema=6 qpc_hz="<<frequency<<" capture_ms=600000 max_samples_per_phase=1024; phase4=requests; sampling is global per phase, not per caller or row; identity=1 means scoped request or panel-event entity round-trip; salted world/entity tokens are not save IDs or world lifetime generations; caller_rva=4294967295 means outside the reviewed executable\n";
        header=true;
    }
    Event events[32]{};
    for(size_t batch=0;batch<8;++batch) {
        const auto count=recorder.drain(events,32);
        for(size_t i=0;i<count;++i) {
            const auto& e=events[i];
            log<<"Tutorial span: phase="<<index(e.phase)<<" seq="<<e.sequence<<" begin="<<e.begin<<" end="<<e.end
                <<" thread="<<e.thread<<" storage="<<e.object<<" caller_rva="<<e.caller_rva<<" query="<<static_cast<unsigned>(e.query)
                <<" identity="<<static_cast<unsigned>(e.identity)<<" world="<<e.world<<" entity="<<e.entity
                <<" overlap_at_entry="<<e.overlap<<" returned="<<e.returned<<'\n';
        }
        if(count<32) break;
    }
    for(size_t i=0;i<phase_count;++i) {
        const auto c=recorder.counts(static_cast<Phase>(i));
        const auto used=sample_count[i].load(std::memory_order_relaxed);
        log<<"Tutorial totals: phase="<<i<<" calls="<<c.calls<<" overlaps="<<c.overlaps<<" unwinds="<<c.unwinds
           <<" rejected_samples="<<c.rejected<<" dropped_samples="<<c.dropped<<" in_flight="<<c.in_flight
           <<" sample_budget_used="<<used<<" sample_budget_exhausted="<<(used>=1024)<<'\n';
    }
    if(closing) {
        closure_reported=true;
        finished=final;
        log<<"Tutorial capture admission closed: admitted_pending="<<pending<<" final="<<final
           <<"; observed spans only, not an engine reader-retirement fence.\n";
    }
    log.flush();
}
#ifdef CRML_TUTORIAL_OBSERVER_TESTING
namespace {
constexpr DWORD test_exception_code=0xe043524d;
bool invoke_native_failure(Requests hook) {
    __try {hook(nullptr);}
    __except(GetExceptionCode()==test_exception_code?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
bool invoke_dispatch_failure(Dispatch hook) {
    __try {hook(nullptr,123,nullptr);}
    __except(GetExceptionCode()==test_exception_code?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
}
bool test_native_unwind() {
    admissions.open();deadline.store(GetTickCount64()+10000);
    original_requests=+[](const void*) {RaiseException(test_exception_code,0,0,nullptr);};
    const auto before=recorder.counts(Phase::requests);
    const bool caught=invoke_native_failure(&requests_hook);
    const auto after=recorder.counts(Phase::requests);
    original_dispatch=original_job=original_panel_dispatch=+[](const void*,uint16_t,const void*) {RaiseException(test_exception_code,0,0,nullptr);};
    bool scope_restored=true;
    for(auto hook:{&dispatch_hook,&job_hook,&panel_dispatch_hook}) {
        const bool dispatch_caught=invoke_dispatch_failure(hook);
        tutorial::Identity identity{};
        scope_restored=scope_restored && dispatch_caught &&
            tutorial::RequestScope::identify(nullptr,identity)==tutorial::IdentityStatus::unavailable &&
            tutorial::PanelScope::identify({},identity)==tutorial::IdentityStatus::unavailable;
    }
    stop();
    return caught && scope_restored && !admissions.pending() && after.in_flight==before.in_flight && after.unwinds==before.unwinds+1;
}
bool test_supported() noexcept {return supported();}
bool test_callers() noexcept {
    constexpr uintptr_t base=0x140000000;
    for(size_t i=0;i<phase_count;++i) {
        const auto phase=static_cast<Phase>(i);
        const auto caller=base+sites[i].caller;
        if(expected_caller(phase,caller,base)!=caller || expected_caller(phase,caller+1,base)) return false;
        for(const auto& job:job_callers) {
            const auto accepted=expected_caller(phase,base+job.caller,base);
            if(accepted!=(phase==job.phase?base+job.caller:0)) return false;
            if(expected_caller(phase,base+job.caller+1,base)) return false;
        }
    }
    return expected_caller(Phase::initialize,base+initializer_job_return,base)==base+initializer_job_return &&
        !expected_caller(Phase::initialize,initializer_job_return,0) &&
        caller_offset(base+initializer_job_return,base)==initializer_job_return &&
        caller_offset(base-1,base)==UINT32_MAX && caller_offset(base+reviewed_image_size,base)==UINT32_MAX;
}
bool test_sampling() noexcept {
    for(size_t i=0;i<phase_count;++i) {sample_count[i].store(0);next_sample[i].store(0);}
    bool ok=sample_at(Phase::requests,1000) && !sample_at(Phase::requests,1000) &&
        !sample_at(Phase::requests,1249) && sample_at(Phase::requests,1250);
    for(uint64_t i=2;i<1024;++i) ok=sample_at(Phase::requests,1000+i*250) && ok;
    ok=ok && !sample_at(Phase::requests,1000+1024*250) &&
        !sample_at(Phase::requests,600000) && sample_count[index(Phase::requests)].load()==1024 &&
        sample_at(Phase::hint_sync,600000);
    for(size_t i=0;i<phase_count;++i) {sample_count[i].store(0);next_sample[i].store(0);}
    return ok;
}
bool test_reporting() {
    static std::atomic<bool> entered{},resume{};
    original_requests=+[](const void*) {
        entered.store(true);entered.notify_one();resume.wait(false);
    };
    next_sample[index(Phase::requests)].store(0);
    sample_count[index(Phase::requests)].store(0);
    installed=true;frequency=1;admissions.open();deadline.store(GetTickCount64()+10000);
    std::thread worker([] {requests_hook(nullptr);});
    entered.wait(false);
    stop();
    std::ostringstream partial;
    poll(partial);
    const bool incomplete=partial.str().find("admitted_pending=1 final=0")!=std::string::npos;
    resume.store(true);resume.notify_one();worker.join();
    std::ostringstream complete;
    poll(complete);
    const auto report=complete.str();
    poll(complete);
    installed=false;
    return incomplete && report.find("admitted_pending=0 final=1")!=std::string::npos &&
        report.find("Tutorial span: phase=4")!=std::string::npos && complete.str()==report;
}
bool test_callthrough() {
    static const void* seen[7]{};static unsigned calls{};
    static uint16_t seen_id{};
    static tutorial::IdentityStatus dispatch_status{};
    original_dispatch=original_job=+[](const void* view,uint16_t id,const void* system) {
        seen[0]=view;seen[1]=system;seen_id=id;++calls;
        tutorial::Identity identity{};dispatch_status=tutorial::RequestScope::identify(nullptr,identity);
    };
    original_panel_dispatch=+[](const void* view,uint16_t id,const void* system) {
        seen[0]=view;seen[1]=system;seen_id=id;++calls;
        tutorial::Identity identity{};dispatch_status=tutorial::PanelScope::identify({},identity);
    };
    original_init=+[](const void* a,const void* b) {seen[0]=a;seen[1]=b;++calls;};
    original_hint=+[](const void* a,void* b,const void* c,const void* d,void* e) {
        seen[0]=a;seen[1]=b;seen[2]=c;seen[3]=d;seen[4]=e;++calls;
    };
    original_panel=+[](const void* a,void* b,const void* c,const void* d,void* e,void* f,void* g) {
        seen[0]=a;seen[1]=b;seen[2]=c;seen[3]=d;seen[4]=e;seen[5]=f;seen[6]=g;++calls;
    };
    original_events=+[](const void* a,void* b,void* c,void* d) {seen[0]=a;seen[1]=b;seen[2]=c;seen[3]=d;++calls;};
    original_requests=+[](const void* a) {seen[0]=a;++calls;};
    int values[7]{};bool ok=true;
    // Exercise both disabled pass-through and enabled instrumentation, including
    // the three stack arguments in the seven-argument native panel ABI.
    for(bool enabled:{false,true}) {
        if(enabled) admissions.open();else admissions.close();
        deadline.store(enabled?GetTickCount64()+10000:0);
        const auto previous=calls;
        init_hook(&values[0],&values[1]);
        for(size_t i=0;i<2;++i) ok=ok && seen[i]==&values[i];
        hint_hook(&values[0],&values[1],&values[2],&values[3],&values[4]);
        for(size_t i=0;i<5;++i) ok=ok && seen[i]==&values[i];
        panel_hook(&values[0],&values[1],&values[2],&values[3],&values[4],&values[5],&values[6]);
        for(size_t i=0;i<7;++i) ok=ok && seen[i]==&values[i];
        events_hook(&values[0],&values[1],&values[2],&values[3]);
        for(size_t i=0;i<4;++i) ok=ok && seen[i]==&values[i];
        requests_hook(&values[0]);
        ok=ok && seen[0]==&values[0] && calls==previous+5;
        // Deliberately invalid pointers must remain unread in disabled hooks;
        // enabled hooks reject the scope without changing forwarded arguments.
        dispatch_hook(reinterpret_cast<void*>(1),0xabcd,reinterpret_cast<void*>(2));
        ok=ok && seen[0]==reinterpret_cast<void*>(1) && seen[1]==reinterpret_cast<void*>(2) && seen_id==0xabcd;
        ok=ok && dispatch_status==(enabled?tutorial::IdentityStatus::scope:tutorial::IdentityStatus::unavailable);
        job_hook(reinterpret_cast<void*>(3),17,reinterpret_cast<void*>(4));
        ok=ok && seen[0]==reinterpret_cast<void*>(3) && seen[1]==reinterpret_cast<void*>(4) && seen_id==17 && calls==previous+7;
        tutorial::Identity identity{};
        ok=ok && tutorial::RequestScope::identify(nullptr,identity)==tutorial::IdentityStatus::unavailable;
        panel_dispatch_hook(reinterpret_cast<void*>(5),18,reinterpret_cast<void*>(6));
        ok=ok && seen[0]==reinterpret_cast<void*>(5) && seen[1]==reinterpret_cast<void*>(6) && seen_id==18 && calls==previous+8;
        ok=ok && dispatch_status==(enabled?tutorial::IdentityStatus::scope:tutorial::IdentityStatus::unavailable) &&
            tutorial::PanelScope::identify({},identity)==tutorial::IdentityStatus::unavailable;
    }
    original_init=+[](const void*,const void*) {++calls;throw std::runtime_error("engine unwind");};
    bool caught=false;
    try {init_hook(nullptr,nullptr);} catch(const std::runtime_error&) {caught=true;}
    const auto count=recorder.counts(Phase::initialize);
    original_requests=+[](const void*) {++calls;throw std::runtime_error("request worker unwind");};
    bool request_caught=false;
    try {requests_hook(nullptr);} catch(const std::runtime_error&) {request_caught=true;}
    const auto request_count=recorder.counts(Phase::requests);
    original_dispatch=original_job=original_panel_dispatch=+[](const void*,uint16_t,const void*) {throw std::runtime_error("dispatcher unwind");};
    unsigned dispatch_unwinds=0;
    for(auto hook:{&dispatch_hook,&job_hook,&panel_dispatch_hook}) {
        try {hook(nullptr,0,nullptr);} catch(const std::runtime_error&) {++dispatch_unwinds;}
        tutorial::Identity identity{};
        ok=ok && tutorial::RequestScope::identify(nullptr,identity)==tutorial::IdentityStatus::unavailable;
        ok=ok && tutorial::PanelScope::identify({},identity)==tutorial::IdentityStatus::unavailable;
    }
    stop();
    return ok && dispatch_unwinds==3 && caught && count.calls==2 && count.unwinds==1 && request_caught &&
        request_count.calls==2 && request_count.unwinds==1 && !request_count.in_flight;
}
#endif
}
