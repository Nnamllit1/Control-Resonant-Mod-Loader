#include "lua_probe.h"
#include "lua_lifetime.h"
#include "lua_session.h"
#include "lua_references.h"
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
bool events_probe{};
std::atomic<bool> enabled{};
std::atomic<unsigned> state{}; // 0 waiting, 1 executing, 2 report available
std::atomic<uint64_t> rejected{};
struct Result {
    int load{-1},call{-1},protect{-1};
    double value{};
    bool number{},restored{},passed{};
    bool environment_created{},environment_verified{};
    unsigned error_line{};
    const char* error_kind{"none"};
};
struct Report { DWORD thread{}; bool passed{}; std::array<Result,5> steps{}; } report;

template<class T> T read(uintptr_t address) noexcept {
    T value;std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));return value;
}
// Read only a bounded error object before stack cleanup. Never emit its text:
// engine errors may contain asset paths, identifiers or other private data.
void error_details(void* vm,const char* label,Result& result) noexcept {
    result.error_kind="unavailable";
    __try {
        const auto l=reinterpret_cast<uintptr_t>(vm);
        const auto top=read<uintptr_t>(l+8),base=read<uintptr_t>(l+0x10);
        if(top<base || top-base<24 || read<uint32_t>(top-8)!=5) return;
        const auto string=read<uintptr_t>(top-24);
        if(!string || read<uint8_t>(string)!=5) return;
        const auto size=read<uint32_t>(string+0x14);
        if(!size || size>4096) return;
        char text[513]{};
        std::memcpy(text,reinterpret_cast<const void*>(string+0x18),size<512?size:512);
        const char* name=label[0]=='='?label+1:label;
        const auto length=std::strlen(name);
        const char* start=text[0]=='='?text+1:text;
        if(!std::strncmp(start,name,length) && start[length]==':') {
            const char* cursor=start+length+1;
            unsigned line=0,digits=0;
            while(*cursor>='0' && *cursor<='9' && digits<5) {line=line*10+unsigned(*cursor++-'0');++digits;}
            if(digits && *cursor==':' && line) result.error_line=line;
        }
        result.error_kind=std::strstr(text,"no active script")?"no_active_script":
            std::strstr(text,"required ECS environment missing")?"missing_ecs":
            std::strstr(text,"invalid event handler handle")?"invalid_handler":
            std::strstr(text,"attempt to call a nil value")?"nil_call":
            std::strstr(text,"assertion failed")?"assertion":"other";
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        result.error_kind="unavailable";result.error_line=0;
    }
}
struct Frame {
    uintptr_t context{},world{},global{},environment{};
    ptrdiff_t top{},base{},ci{};
    size_t size{};
    std::array<unsigned char,64*24> values{};
};
// No API calls here; a bad layout only rejects this observation.
bool capture(void* vm,Frame& f,uintptr_t spare=96) noexcept {
    __try {
        const auto l=reinterpret_cast<uintptr_t>(vm);
        if(!l || read<uint8_t>(l+3)) return false;
        const auto stack=read<uintptr_t>(l+0x30),base=read<uintptr_t>(l+0x10);
        const auto top=read<uintptr_t>(l+8),last=read<uintptr_t>(l+0x28);
        const auto ci=read<uintptr_t>(l+0x20),baseci=read<uintptr_t>(l+0x40);
        if(!stack || base<stack || top<=base || last<top || last-top<spare ||
           spare>f.values.size() || top-base>f.values.size()-spare || (top-stack)%24 || (base-stack)%24 ||
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
    const auto l=reinterpret_cast<uintptr_t>(vm);
    const int saved=static_cast<int>((read<uintptr_t>(l+8)-read<uintptr_t>(l+0x10))/24);
    int env=0;
    constexpr int globals=-10002;
    constexpr const char* marker="__crml_probe_environment_v1";
    if(w.owner) {
        // Follow the engine's entity environment construction. All allocating
        // APIs run inside the existing error barrier; the table stays rooted.
        if(api.raw_field(vm,globals,marker)!=0) return;
        api.settop(vm,saved);
        api.new_table(vm,0,3);env=saved+1;
        api.new_table(vm,0,1);
        api.push_value(vm,globals);api.set_field(vm,-2,"__index");
        api.readonly(vm,-1,1);
        if(!api.set_metatable(vm,env)) return;
        api.push_value(vm,env);api.set_field(vm,env,"_ENV");
        api.push_entity(vm,w.owner,1);api.set_field(vm,env,"self");
        result.environment_created=true;
    }
    result.load=api.load(vm,w.label,reinterpret_cast<const char*>(w.bytes),w.size,env);
    if(result.load) return;
    if(w.owner) api.push_entity(vm,w.owner,1);
    result.call=api.call(vm,w.owner?1:0,w.owner?2:1,0);
    if(result.call) return;
    const auto top=read<uintptr_t>(reinterpret_cast<uintptr_t>(vm)+8);
    const auto value=top-(w.owner?48:24);
    result.number=read<uint32_t>(value+0x10)==3;
    if(result.number) result.value=read<double>(value);
    // The optional second event result carries a caught dispatch/removal error.
    // Classify it while rooted, without retaining or logging the original text.
    if(w.owner && result.number && (result.value==-201 || result.value==-202 || result.value==-206))
        error_details(vm,w.label,result);
    if(w.owner) {
        const int results_top=env+2;
        const int tag=api.raw_field(vm,env,marker);
        const auto cell=read<uintptr_t>(l+8)-24;
        const bool private_value=tag==3 && read<double>(cell)==42;
        api.settop(vm,results_top);
        const bool shared_unchanged=api.raw_field(vm,globals,marker)==0;
        api.settop(vm,results_top);
        result.environment_verified=private_value && shared_unchanged;
    }
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
        if(result.protect || result.load>0 || result.call>0) error_details(vm,work.label,result);
        Frame before_cleanup;
        if(capture(vm,before_cleanup,0) && same_frame(initial,before_cleanup,false) && before_cleanup.top>=initial.top) {
            api.settop(vm,static_cast<int>((initial.top-initial.base)/24));
            Frame after;
            result.restored=capture(vm,after) && same_frame(initial,after,true);
        }
        const bool expected=i==1 ? result.call==2 : result.call==0 && result.number && std::isfinite(result.value) &&
            (i==3 ? result.value>=0 && result.value<=127 && std::floor(result.value)==result.value : result.value==(i==4?127:42));
        result.passed=result.protect==0 && result.load==0 && expected && result.restored && (i!=4 || result.environment_verified);
        if(!result.passed) {report.passed=false;break;}
    }
}
struct PersistentWork {
    session::Context context;
    session::Action action;
    int reference{};
    session::Result result;
};
void persistent_body(void* vm,void* user) {
    auto& w=*static_cast<PersistentWork*>(user);
    auto& result=w.result;
    const auto l=reinterpret_cast<uintptr_t>(vm);
    const int saved=static_cast<int>((read<uintptr_t>(l+8)-read<uintptr_t>(l+0x10))/24);
    if(w.action==session::Action::release || w.action==session::Action::unload) {
        if(w.action==session::Action::unload) {
            if(api.fetch(vm,-10000,w.reference)!=7) result.status=-301;
            else {
                api.push_value(vm,-1); // A non-nil command; no new push API needed.
                result.status=api.call(vm,1,1,0);
                if(!result.status) {
                    const auto value=read<uintptr_t>(l+8)-24;
                    if(read<uint32_t>(value+16)!=3) result.status=-307;
                    else {result.value=read<double>(value);result.shutdown=true;}
                }
            }
        }
        api.release(vm,w.reference);result.released=true;return;
    }
    if(w.action==session::Action::invoke) {
        if(api.fetch(vm,-10000,w.reference)!=7) {result.status=-301;return;}
        result.status=api.call(vm,0,1,0);
        if(result.status) {
            Result diagnostic;
            error_details(vm,events_probe?"=crml_persistent_events":"=crml_persistent_error",diagnostic);
            result.error_line=diagnostic.error_line;
            result.deliberate_error=std::strcmp(diagnostic.error_kind,"nil_call")==0 &&
                diagnostic.error_line==(events_probe?bytecode::persistent_events_error_line:bytecode::persistent_error_error_line);
            return;
        }
        const auto value=read<uintptr_t>(l+8)-24;
        if(read<uint32_t>(value+16)!=3) {result.status=-302;return;}
        result.value=read<double>(value);return;
    }
    constexpr const char* marker="__crml_persistent_counter";
    if(api.raw_field(vm,-10002,marker)!=0) {result.status=-303;return;}
    api.settop(vm,saved);
    api.new_table(vm,0,3);const int env=saved+1;
    api.new_table(vm,0,1);
    api.push_value(vm,-10002);api.set_field(vm,-2,"__index");
    api.readonly(vm,-1,1);
    if(!api.set_metatable(vm,env)) {result.status=-304;return;}
    api.push_value(vm,env);api.set_field(vm,env,"_ENV");
    api.push_entity(vm,w.context.owner,1);api.set_field(vm,env,"self");
    const bool error=w.action==session::Action::initialize_error;
    const bool listener_error=w.action==session::Action::initialize_listener_error;
    const bool rollback=w.action==session::Action::initialize_rollback;
    const auto* code=events_probe?bytecode::persistent_events:error?bytecode::persistent_error:bytecode::persistent;
    const auto size=events_probe?sizeof(bytecode::persistent_events):error?sizeof(bytecode::persistent_error):sizeof(bytecode::persistent);
    result.status=api.load(vm,events_probe?"=crml_persistent_events":error?"=crml_persistent_error":"=crml_persistent",
        reinterpret_cast<const char*>(code),size,env);
    if(result.status) return;
    // Three truthy arguments select initialization rollback; two select a
    // listener error; one selects a controller error. Reuse rooted values.
    const int arguments=events_probe?(rollback?3:listener_error?2:error?1:0):0;
    for(int i=0;i<arguments;++i) api.push_value(vm,env);
    result.status=api.call(vm,arguments,1,0);
    if(result.status) return;
    if(read<uint32_t>(read<uintptr_t>(l+8)-8)!=7) {result.status=-305;return;}
    // Event construction has no engine side effects. Register only when this
    // retained controller is invoked, so a failed retain cannot strand a listener.
    result.reference=api.retain(vm,-1); // Copies without popping; stack stays rooted.
    if(result.reference<=0) result.status=-306;
}
session::Result persistent_execute(session::Context context,session::Action action,int reference) {
    Frame initial;
    if(!capture(context.vm,initial,144) || initial.global!=context.global || initial.world!=context.world ||
       !valid_owner({context.world,context.owner})) return {};
    PersistentWork work{context,action,reference,{}};work.result.attempted=true;
    const int status=api.protect(context.vm,&persistent_body,&work,initial.top,0);
    if(status) work.result.status=status;
    Frame before_cleanup;
    if(capture(context.vm,before_cleanup,0) && same_frame(initial,before_cleanup,false) && before_cleanup.top>=initial.top) {
        api.settop(context.vm,static_cast<int>((initial.top-initial.base)/24));
        Frame after;
        work.result.restored=capture(context.vm,after) && same_frame(initial,after,true);
    }
    return work.result;
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
    constexpr uintptr_t environment_rvas[]{0x2c4f290,0x2c4d0b0,0x2c4f6e0,0x2c4f320,0x2c4fa90,0x2c4f030};
    constexpr unsigned char environment_signatures[][12]{
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89},
        {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0xf6,0x41},
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48},
        {0x48,0x83,0xec,0x28,0x45,0x8b,0xd8,0x4c,0x8b,0xd1,0x85,0xd2},
        {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8d,0x59,0x08,0x4c,0x8b},
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89}};
    for(size_t i=0;i<6;++i) if(std::memcmp(reinterpret_cast<void*>(image+environment_rvas[i]),environment_signatures[i],12)) return false;
    api={reinterpret_cast<decltype(api.load)>(image+rvas[0]),reinterpret_cast<decltype(api.protect)>(image+rvas[1]),
         reinterpret_cast<decltype(api.settop)>(image+rvas[2]),original,reinterpret_cast<decltype(api.push_entity)>(image+0x2c4ec50)};
    api.new_table=reinterpret_cast<decltype(api.new_table)>(image+environment_rvas[0]);
    api.push_value=reinterpret_cast<decltype(api.push_value)>(image+environment_rvas[1]);
    api.set_field=reinterpret_cast<decltype(api.set_field)>(image+environment_rvas[2]);
    api.readonly=reinterpret_cast<decltype(api.readonly)>(image+environment_rvas[3]);
    api.set_metatable=reinterpret_cast<decltype(api.set_metatable)>(image+environment_rvas[4]);
    api.raw_field=reinterpret_cast<decltype(api.raw_field)>(image+environment_rvas[5]);
    image_base=image;enabled.store(true,std::memory_order_release);return true;
#else
    (void)image;(void)original;return false;
#endif
}
bool start_persistent(uintptr_t image) noexcept {
#ifdef CRML_LUA_PROBE
    if(!enabled.load() || !image) return false;
    constexpr unsigned char retain[]{0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20};
    constexpr unsigned char fetch[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10};
    constexpr unsigned char release[]{0x85,0xd2,0x7e,0x46,0x57,0x48,0x83,0xec,0x20};
    if(std::memcmp(reinterpret_cast<void*>(image+0x2c507a0),retain,sizeof(retain)) ||
       std::memcmp(reinterpret_cast<void*>(image+0x2c4f1d0),fetch,sizeof(fetch)) ||
       std::memcmp(reinterpret_cast<void*>(image+0x2c508d0),release,sizeof(release))) return false;
    api.retain=reinterpret_cast<decltype(api.retain)>(image+0x2c507a0);
    api.fetch=reinterpret_cast<decltype(api.fetch)>(image+0x2c4f1d0);
    api.release=reinterpret_cast<decltype(api.release)>(image+0x2c508d0);
    if(!references::start(image)) return false;
    events_probe=true;
    session::start(&persistent_execute,true);return true;
#else
    (void)image;return false;
#endif
}
Owner before_call(void* vm,uintptr_t caller,int nargs,int results,int error) noexcept {
    const auto revision=session::revision();
    if((!enabled.load(std::memory_order_acquire) && !session::needs_calls()) || state.load()==1 ||
       caller!=image_base+0x1a0aadf || nargs!=1 || results!=0 || error!=1) return {};
    if(state.load()!=0 && !lifetime::active() && !session::needs_calls()) return {};
    Frame frame;
    if(!capture(vm,frame) || frame.size<72) return {};
    // Copy only the value of the non-GC entity argument while it is still rooted
    // on the incoming call's stack. Do not retain the closure or its environment.
    uint32_t tag{},kind{};uint64_t entity{};
    const auto* argument=frame.values.data()+frame.size-24;
    std::memcpy(&entity,argument,8);std::memcpy(&kind,argument+8,4);std::memcpy(&tag,argument+16,4);
    Owner owner{frame.world,entity,revision};
    if(tag!=2 || kind!=1 || !valid_owner(owner)) return {};
    lifetime::observe(vm,owner.world,owner.entity);
    return owner;
}
void after_call(void* vm,uintptr_t caller,int nargs,int results,int error,int status,Owner owner) {
    if((!enabled.load(std::memory_order_acquire) && !session::needs_calls()) || state.load()==1 || status ||
       caller!=image_base+0x1a0aadf || nargs!=1 || results!=0 || error!=1) return;
    Frame frame;
    if(!capture(vm,frame) || frame.size>60*24 || frame.world!=owner.world || !valid_owner(owner)) {++rejected;return;}
    if(state.load()==0) {
        unsigned expected=0;
        if(!state.compare_exchange_strong(expected,1)) return;
        run(vm,frame,owner);
        state.store(2,std::memory_order_release);
        return;
    }
    if(report.passed && session::needs_calls()) session::tick({vm,frame.global,frame.world,owner.entity,owner.revision},GetTickCount64());
}
void write(std::ostream& out) {
    const auto s=state.load(std::memory_order_acquire);
    out<<"{\"type\":\"lua_probe\",\"schema\":4,\"enabled\":"<<(enabled.load()?"true":"false")
       <<",\"state\":"<<s<<",\"rejected\":"<<rejected.load();
    if(s==2) {
        out<<",\"thread\":"<<report.thread<<",\"passed\":"<<(report.passed?"true":"false")<<",\"steps\":[";
        for(size_t i=0;i<report.steps.size();++i) {
            if(i) out<<',';const auto& r=report.steps[i];
            out<<"{\"load\":"<<r.load<<",\"call\":"<<r.call<<",\"protect\":"<<r.protect<<",\"value\":";
            if(r.number && std::isfinite(r.value)) out<<r.value;else out<<"null";
            out<<",\"restored\":"<<(r.restored?"true":"false")<<",\"passed\":"<<(r.passed?"true":"false")
               <<",\"error_kind\":\""<<r.error_kind<<"\",\"error_line\":"<<r.error_line
               <<",\"environment_created\":"<<(r.environment_created?"true":"false")
               <<",\"environment_verified\":"<<(r.environment_verified?"true":"false")<<'}';
        }
        out<<']';
    }
    out<<"}\n";
}
void stop() noexcept {enabled.store(false,std::memory_order_release);session::stop();}
#ifdef CRML_LUA_PROBE_TESTING
void configure(Api functions,uintptr_t image) {
    enabled=false;api=functions;image_base=image;report={};state=0;rejected=0;enabled=true;
}
void configure_persistent(bool events) {
    events_probe=events;
#ifdef CRML_LUA_SESSION_TESTING
    session::reset_for_test(&persistent_execute,events);
#else
    session::start(&persistent_execute,events);
#endif
}
#endif
}
