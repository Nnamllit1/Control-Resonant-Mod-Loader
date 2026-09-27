#include "lua_probe.h"
#include "lua_probe_bytecode.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ostream>

namespace crml::probe::lua {
namespace {
Api api{};
uintptr_t image_base{};
std::atomic<bool> enabled{};
std::atomic<unsigned> state{}; // 0 waiting, 1 executing, 2 report available
std::atomic<uint64_t> rejected{};
struct Result {
    int load{-1},call{-1},protect{-1};
    double value{};
    bool number{},restored{},passed{};
};
struct Report { DWORD thread{}; bool passed{}; std::array<Result,5> steps{}; } report;

template<class T> T read(uintptr_t address) noexcept {
    T value;std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));return value;
}
struct Frame {
    uintptr_t context{},world{},global{},environment{};
    ptrdiff_t top{},base{},ci{};
    size_t size{};
    std::array<unsigned char,64*24> values{};
};
// No API calls here; a bad layout only rejects this observation.
bool capture(void* vm,Frame& f) noexcept {
    __try {
        const auto l=reinterpret_cast<uintptr_t>(vm);
        if(!l || read<uint8_t>(l+3)) return false;
        const auto stack=read<uintptr_t>(l+0x30),base=read<uintptr_t>(l+0x10);
        const auto top=read<uintptr_t>(l+8),last=read<uintptr_t>(l+0x28);
        const auto ci=read<uintptr_t>(l+0x20),baseci=read<uintptr_t>(l+0x40);
        if(!stack || base<stack || top<=base || last<top || last-top<48 ||
           top-base>f.values.size() || (top-stack)%24 || (base-stack)%24 ||
           !ci || ci!=baseci || read<uintptr_t>(ci)!=base) return false;
        if(read<uint32_t>(base+0x10)!=7) return false; // Existing engine error handler.
        f.context=read<uintptr_t>(l+0x78);
        f.global=read<uintptr_t>(l+0x18);f.environment=read<uintptr_t>(l+0x58);
        if(!f.context || !f.global || !f.environment) return false;
        f.world=read<uintptr_t>(f.context);
        // A debugger may suspend on the deliberate error instead of unwinding.
        if(!f.world || read<uintptr_t>(f.global+0xd28)) return false;
        f.top=top-stack;f.base=base-stack;f.ci=ci-baseci;f.size=top-base;
        std::memcpy(f.values.data(),reinterpret_cast<void*>(base),f.size);
        return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
bool same_frame(const Frame& a,const Frame& b,bool top) noexcept {
    return a.context==b.context && a.world==b.world && a.global==b.global &&
        a.environment==b.environment && a.base==b.base && a.ci==b.ci && (!top ||
        (a.top==b.top && a.size==b.size && !std::memcmp(a.values.data(),b.values.data(),a.size)));
}
bool valid_owner(Owner owner) noexcept {
    __try {
        const uint32_t index=static_cast<uint32_t>(owner.entity),generation=static_cast<uint32_t>(owner.entity>>32);
        if(!owner.world || !owner.entity || index==UINT32_MAX || index>=read<uint64_t>(owner.world+0x58510)) return false;
        const auto slots=read<uintptr_t>(owner.world+0x584e8);
        return slots && read<uint32_t>(slots+uintptr_t(index)*8)==generation;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
struct Work { const unsigned char* bytes;size_t size;const char* label;Result* result;uint64_t owner{}; };
void execute(void* vm,void* user) {
    auto& w=*static_cast<Work*>(user);auto& result=*w.result;
    result.load=api.load(vm,w.label,reinterpret_cast<const char*>(w.bytes),w.size,0);
    if(result.load) return;
    if(w.owner) api.push_entity(vm,w.owner,1);
    result.call=api.call(vm,w.owner?1:0,1,0);
    if(result.call) return;
    const auto top=read<uintptr_t>(reinterpret_cast<uintptr_t>(vm)+8);
    result.number=read<uint32_t>(top-24+0x10)==3;
    if(result.number) result.value=read<double>(top-24);
}
void run(void* vm,const Frame& initial,Owner owner) {
    report={};report.thread=GetCurrentThreadId();report.passed=true;
    const unsigned char* chunks[]{bytecode::arithmetic,bytecode::error,bytecode::arithmetic,bytecode::bindings,bytecode::events};
    const size_t sizes[]{sizeof(bytecode::arithmetic),sizeof(bytecode::error),sizeof(bytecode::arithmetic),sizeof(bytecode::bindings),sizeof(bytecode::events)};
    const char* labels[]{"=crml_probe_arithmetic","=crml_probe_error","=crml_probe_recovery","=crml_probe_bindings","=crml_probe_events"};
    for(size_t i=0;i<report.steps.size();++i) {
        auto& result=report.steps[i];Work work{chunks[i],sizes[i],labels[i],&result,i==4?owner.entity:0};
        // Engine error barrier includes loading/allocation, not just execution.
        // Its saved top is an offset, so stack growth cannot stale a raw pointer.
        result.protect=api.protect(vm,&execute,&work,initial.top,0);
        Frame before_cleanup;
        if(capture(vm,before_cleanup) && same_frame(initial,before_cleanup,false) && before_cleanup.top>=initial.top) {
            api.settop(vm,static_cast<int>((initial.top-initial.base)/24));
            Frame after;
            result.restored=capture(vm,after) && same_frame(initial,after,true);
        }
        const bool expected=i==1 ? result.call==2 : result.call==0 && result.number && std::isfinite(result.value) &&
            (i==3 ? result.value>=0 && result.value<=127 && std::floor(result.value)==result.value : result.value==(i==4?127:42));
        result.passed=result.protect==0 && result.load==0 && expected && result.restored;
        if(!result.passed) {report.passed=false;break;}
    }
}
}
bool start(uintptr_t image,Call original) noexcept {
#if defined(CRML_LUA_PROBE) || defined(CRML_LUA_PROBE_TESTING)
    if(!image || !original) return false;
    constexpr unsigned char signatures[3][12]{
        {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x48,0x89},
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89},
        {0x48,0x63,0xc2,0x4c,0x8b,0xc1,0x85,0xd2,0x78,0x4c,0x48,0x8d}};
    const uintptr_t rvas[]{0x2c27b70,0x2c434f0,0x2c4d040};
    for(size_t i=0;i<3;++i) if(std::memcmp(reinterpret_cast<void*>(image+rvas[i]),signatures[i],12)) return false;
    constexpr unsigned char site[]{0xe8,0xb1,0x51,0x24,0x01,0x85,0xc0};
    if(std::memcmp(reinterpret_cast<void*>(image+0x1a0aada),site,sizeof(site))) return false;
    constexpr unsigned char push[]{0x48,0x8b,0x41,0x08,0x48,0x89,0x10,0x44,0x89,0x40,0x08};
    if(std::memcmp(reinterpret_cast<void*>(image+0x2c4ec50),push,sizeof(push))) return false;
    api={reinterpret_cast<decltype(api.load)>(image+rvas[0]),reinterpret_cast<decltype(api.protect)>(image+rvas[1]),
         reinterpret_cast<decltype(api.settop)>(image+rvas[2]),original,reinterpret_cast<decltype(api.push_entity)>(image+0x2c4ec50)};
    image_base=image;enabled.store(true,std::memory_order_release);return true;
#else
    (void)image;(void)original;return false;
#endif
}
Owner before_call(void* vm,uintptr_t caller,int nargs,int results,int error) noexcept {
    if(!enabled.load(std::memory_order_acquire) || state.load()!=0 ||
       caller!=image_base+0x1a0aadf || nargs!=1 || results!=0 || error!=1) return {};
    Frame frame;
    if(!capture(vm,frame) || frame.size<72) return {};
    // Copy only the value of the non-GC entity argument while it is still rooted
    // on the incoming call's stack. Do not retain the closure or its environment.
    uint32_t tag{},kind{};uint64_t entity{};
    const auto* argument=frame.values.data()+frame.size-24;
    std::memcpy(&entity,argument,8);std::memcpy(&kind,argument+8,4);std::memcpy(&tag,argument+16,4);
    Owner owner{frame.world,entity};
    return tag==2 && kind==1 && valid_owner(owner)?owner:Owner{};
}
void after_call(void* vm,uintptr_t caller,int nargs,int results,int error,int status,Owner owner) {
    if(!enabled.load(std::memory_order_acquire) || state.load()!=0 || status ||
       caller!=image_base+0x1a0aadf || nargs!=1 || results!=0 || error!=1) return;
    Frame frame;
    if(!capture(vm,frame) || frame.size>62*24 || frame.world!=owner.world || !valid_owner(owner)) {++rejected;return;}
    unsigned expected=0;
    if(!state.compare_exchange_strong(expected,1)) return;
    run(vm,frame,owner);
    state.store(2,std::memory_order_release);
}
void write(std::ostream& out) {
    const auto s=state.load(std::memory_order_acquire);
    out<<"{\"type\":\"lua_probe\",\"schema\":2,\"enabled\":"<<(enabled.load()?"true":"false")
       <<",\"state\":"<<s<<",\"rejected\":"<<rejected.load();
    if(s==2) {
        out<<",\"thread\":"<<report.thread<<",\"passed\":"<<(report.passed?"true":"false")<<",\"steps\":[";
        for(size_t i=0;i<report.steps.size();++i) {
            if(i) out<<',';const auto& r=report.steps[i];
            out<<"{\"load\":"<<r.load<<",\"call\":"<<r.call<<",\"protect\":"<<r.protect<<",\"value\":";
            if(r.number && std::isfinite(r.value)) out<<r.value;else out<<"null";
            out<<",\"restored\":"<<(r.restored?"true":"false")<<",\"passed\":"<<(r.passed?"true":"false")<<'}';
        }
        out<<']';
    }
    out<<"}\n";
}
void stop() noexcept {enabled.store(false,std::memory_order_release);}
#ifdef CRML_LUA_PROBE_TESTING
void configure(Api functions,uintptr_t image) {
    enabled=false;api=functions;image_base=image;report={};state=0;rejected=0;enabled=true;
}
#endif
}
