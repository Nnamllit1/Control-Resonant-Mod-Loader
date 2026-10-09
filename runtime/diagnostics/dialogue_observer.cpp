#include "diagnostics/dialogue_observer.h"
#include "compatibility.h"
#include <MinHook.h>
#include <intrin.h>
#include <cstring>
#include <ostream>
#ifdef CRML_DIALOGUE_OBSERVER_TESTING
#include <sstream>
#endif

namespace crml::dialogue_observer {
namespace {
constexpr uintptr_t worker_rva=0x1e4a630,caller_rva=0x1e4ac84;
constexpr unsigned char prologue[]{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8b,0xec,0x48,0x83,0xec,0x60,0x4d,0x8b,0xf8,0x48,0x8b,0xf2,0x48};
constexpr uint64_t capture_ms=600000;
constexpr uint64_t max_samples=4096;
using Worker=uint8_t(*)(const void*,const void*,const void*);
Worker original{};
Recorder recorder;
Admissions admissions;
std::atomic<uint64_t> deadline{},sequences{};
uintptr_t image{};
bool installed{};

const char* name(dialogue::Read read) noexcept {
    switch(read) {
    case dialogue::Read::ok:return "selected";
    case dialogue::Read::empty:return "empty";
    case dialogue::Read::arguments:return "arguments";
    case dialogue::Read::identity:return "identity";
    case dialogue::Read::range:return "range";
    case dialogue::Read::encoding:return "encoding";
    case dialogue::Read::memory:return "memory";
    }
    return "unknown";
}
bool caller_matches(uintptr_t caller,uintptr_t base) noexcept {
    return base && caller==base+caller_rva;
}
bool supported() noexcept {
    return compatibility::reviewed_build &&
        compatibility::engine_profile==compatibility::EngineProfile::october_patch;
}
uint8_t forward(const void* ui,const void* source,const void* format,bool admitted,bool valid_caller) {
    uint8_t result{};
    bool returned=false;
    bool changed=false;
    Event event{};
    const Event* sample=nullptr;
    // Do not catch engine exceptions. The finally block only closes our own
    // admission so shutdown can account for a native unwind.
    __try {
        result=original(ui,source,format);
        returned=true;
        changed=result!=0;
        if(admitted && changed && valid_caller) {
            const auto sequence=sequences.fetch_add(1,std::memory_order_relaxed)+1;
            if(sequence<=max_samples) {
                dialogue::Observation copied{};
                event.read=dialogue::inspect_selected(ui,source,copied);
                event.sequence=sequence;
                event.time_ms=GetTickCount64();
                if(event.read==dialogue::Read::ok) {
                    event.allocation=copied.allocation;
                    event.playback_key=copied.playback_key;
                    event.segment=copied.segment;
                    event.text_length=copied.length;
                    event.text_hash=dialogue::text_hash(copied);
                }
                sample=&event;
            }
        }
    } __finally {
        if(admitted) {recorder.end(returned,changed,sample);admissions.leave();}
    }
    return result;
}
uint8_t hook(const void* ui,const void* source,const void* format) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const auto until=deadline.load(std::memory_order_acquire);
    const bool admitted=until && GetTickCount64()<until && admissions.enter();
    if(admitted) recorder.begin();
    return forward(ui,source,format,admitted,caller_matches(caller,image));
}
}
void Recorder::begin() noexcept {
    calls_.fetch_add(1,std::memory_order_relaxed);
    in_flight_.fetch_add(1,std::memory_order_relaxed);
}
void Recorder::end(bool returned,bool changed,const Event* sample) noexcept {
    if(!returned) unwinds_.fetch_add(1,std::memory_order_relaxed);
    if(changed) changes_.fetch_add(1,std::memory_order_relaxed);
    if(changed && !sample) dropped_.fetch_add(1,std::memory_order_relaxed);
    if(sample) {
        samples_.fetch_add(1,std::memory_order_relaxed);
        if(sample->read!=dialogue::Read::ok) rejected_.fetch_add(1,std::memory_order_relaxed);
        if(!TryAcquireSRWLockExclusive(&lock_)) dropped_.fetch_add(1,std::memory_order_relaxed);
        else {
            if(size_==events_.size()) dropped_.fetch_add(1,std::memory_order_relaxed);
            else {events_[(head_+size_)%events_.size()]=*sample;++size_;}
            ReleaseSRWLockExclusive(&lock_);
        }
    }
    in_flight_.fetch_sub(1,std::memory_order_release);
}
size_t Recorder::drain(Event* out,size_t capacity) noexcept {
    if(!out || !capacity || !TryAcquireSRWLockExclusive(&lock_)) return 0;
    size_t n=0;
    while(n<capacity && size_) {out[n++]=events_[head_];head_=(head_+1)%events_.size();--size_;}
    ReleaseSRWLockExclusive(&lock_);return n;
}
Counts Recorder::counts() const noexcept {
    return {calls_.load(),changes_.load(),samples_.load(),rejected_.load(),dropped_.load(),unwinds_.load(),in_flight_.load()};
}
bool start() noexcept {
    if(installed) return true;
    if(!supported()) return false;
    image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* target=reinterpret_cast<void*>(image+worker_rva);
    const auto* call=reinterpret_cast<const unsigned char*>(image+caller_rva-5);
    constexpr unsigned char call_bytes[]{0xe8,0xac,0xf9,0xff,0xff};
    if(!compatibility::matches(target,prologue,sizeof(prologue)) ||
       !compatibility::matches(call,call_bytes,sizeof(call_bytes))) return false;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    if(MH_CreateHook(target,reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&original))!=MH_OK) return false;
    if(MH_EnableHook(target)!=MH_OK) {MH_RemoveHook(target);return false;}
    admissions.open();
    deadline.store(GetTickCount64()+capture_ms,std::memory_order_release);
    installed=true;return true;
}
bool active() noexcept {const auto until=deadline.load(std::memory_order_acquire);return until && GetTickCount64()<until;}
void stop() noexcept {deadline.store(0,std::memory_order_release);admissions.close();}
void poll(std::ostream& log) {
    static uint64_t last{};
    static bool finished{};
    if(!installed || finished) return;
    const auto now=GetTickCount64();
    const bool closing=!active();
    if(closing) stop();
    const bool final=closing && !admissions.pending();
    if(last && now-last<1000 && !final) return;
    last=now;
    // The shared log stream may have been left in hex/showpos mode by another
    // diagnostic. JSON numbers must remain decimal and unsigned.
    struct Flags {
        std::ostream& stream;
        std::ios_base::fmtflags saved;
        ~Flags() {stream.flags(saved);}
    } flags{log,log.flags()};
    log<<std::dec<<std::noshowpos;
    Event events[64]{};
    for(unsigned batch=0;batch<4;++batch) {
        const auto count=recorder.drain(events,std::size(events));
        for(size_t i=0;i<count;++i) {
            const auto& e=events[i];
            log<<"Capability dialogue: {\"schema\":1,\"type\":\"sample\",\"status\":\""<<name(e.read)
               <<"\",\"sequence\":"<<e.sequence<<",\"time_ms\":"<<e.time_ms
               <<",\"allocation\":"<<e.allocation<<",\"playback_key\":"<<e.playback_key
               <<",\"segment\":"<<e.segment<<",\"text_length\":"<<e.text_length
               <<",\"text_hash\":\""<<e.text_hash<<"\"}\n";
        }
        if(count<std::size(events)) break;
    }
    const auto c=recorder.counts();
    log<<"Capability dialogue: {\"schema\":1,\"type\":\"totals\",\"status\":\""
       <<(final?"stopped":closing?"draining":"active")<<"\",\"time_ms\":"<<now
       <<",\"calls\":"<<c.calls<<",\"changes\":"<<c.changes<<",\"samples\":"<<c.samples
       <<",\"rejected\":"<<c.rejected<<",\"dropped\":"<<c.dropped
       <<",\"unwinds\":"<<c.unwinds<<",\"in_flight\":"<<c.in_flight
       <<",\"pending\":"<<admissions.pending()<<"}\n";
    if(final) finished=true;
}
#ifdef CRML_DIALOGUE_OBSERVER_TESTING
namespace {
constexpr DWORD test_exception_code=0xe043524d;
bool invoke_native_failure() {
    __try {hook(nullptr,nullptr,nullptr);}
    __except(GetExceptionCode()==test_exception_code?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
}
bool test_callers() noexcept {
    constexpr uintptr_t base=0x140000000;
    return caller_matches(base+caller_rva,base) && !caller_matches(base+caller_rva-1,base) &&
        !caller_matches(base+caller_rva+1,base) && !caller_matches(base+caller_rva,0);
}
bool test_callthrough() {
    static const void* seen[3]{};
    static unsigned calls{};
    original=+[](const void* ui,const void* source,const void* format)->uint8_t {
        seen[0]=ui;seen[1]=source;seen[2]=format;++calls;return 1;
    };
    unsigned char ui[0x58]{},source[0x70]{};
    const uint32_t key=37,segment=4,allocation=901;
    const uint64_t length=4,capacity=15;
    const float duration=5,elapsed=2;
    std::memcpy(ui,&key,4);std::memcpy(ui+0x30,"line",5);
    std::memcpy(ui+0x40,&length,8);std::memcpy(ui+0x48,&capacity,8);
    std::memcpy(ui+0x50,&segment,4);
    std::memcpy(source+0x4c,&key,4);std::memcpy(source+0x50,&duration,4);
    std::memcpy(source+0x54,&elapsed,4);std::memcpy(source+0x60,&allocation,4);
    int format{};
    admissions.open();
    const auto before=recorder.counts();
    const bool entered=admissions.enter();
    if(entered) recorder.begin();
    const auto result=forward(ui,source,&format,entered,true);
    Event event{};
    const bool captured=recorder.drain(&event,1)==1;
    bool ok=entered && result==1 && calls==1 && seen[0]==ui && seen[1]==source &&
        seen[2]==&format && captured && event.read==dialogue::Read::ok &&
        event.playback_key==key && event.segment==segment && event.allocation==allocation &&
        event.text_length==length && !admissions.pending();
    const auto foreign=admissions.enter();
    if(foreign) recorder.begin();
    forward(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),&format,foreign,false);
    ok=ok && foreign && !recorder.drain(&event,1) && !admissions.pending();
    deadline.store(0);
    hook(reinterpret_cast<void*>(3),reinterpret_cast<void*>(4),&format);
    const auto after=recorder.counts();
    ok=ok && calls==3 && seen[0]==reinterpret_cast<void*>(3) &&
        seen[1]==reinterpret_cast<void*>(4) && after.calls==before.calls+2 &&
        after.changes==before.changes+2 && after.samples==before.samples+1 &&
        after.dropped==before.dropped+1 && after.in_flight==0;
    stop();return ok;
}
bool test_native_unwind() {
    admissions.open();deadline.store(GetTickCount64()+10000);
    original=+[](const void*,const void*,const void*)->uint8_t {
        RaiseException(test_exception_code,0,0,nullptr);return 0;
    };
    const auto before=recorder.counts();
    const bool propagated=invoke_native_failure();
    const auto after=recorder.counts();
    stop();
    return propagated && !admissions.pending() && after.calls==before.calls+1 &&
        after.unwinds==before.unwinds+1 && !after.in_flight;
}
bool test_reporting() {
    installed=true;
    admissions.open();deadline.store(GetTickCount64()+10000);
    Event event{};event.sequence=17;event.time_ms=100;event.read=dialogue::Read::ok;
    event.allocation=41;event.playback_key=43;event.segment=2;
    event.text_length=4;event.text_hash=UINT64_MAX;
    recorder.begin();recorder.end(true,true,&event);
    std::ostringstream log;
    log<<std::hex<<std::showpos;
    const auto flags=log.flags();
    poll(log);
    const auto output=log.str();
    const bool valid=output.find("Capability dialogue: {\"schema\":1,\"type\":\"sample\",\"status\":\"selected\",\"sequence\":17")!=std::string::npos &&
        output.find("\"text_hash\":\"18446744073709551615\"")!=std::string::npos &&
        output.find("\"type\":\"totals\"")!=std::string::npos && log.flags()==flags;
    stop();poll(log);installed=false;
    return valid;
}
#endif
}
