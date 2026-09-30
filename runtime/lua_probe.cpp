#include "compatibility.h"
#include "lua_probe.h"
#include "lua_executor.h"
#include "lua_lifetime.h"
#include "lua_session.h"
#include "lua_references.h"
#include "lua_probe_bytecode.h"
#ifdef CRML_LUA_SOURCE
#include "lua_source.h"
#endif
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
bool source_mode() noexcept {
#ifdef CRML_LUA_SOURCE
    return engine::lua::source::requested();
#else
    return false;
#endif
}
bool source_calls() noexcept {
#ifdef CRML_LUA_SOURCE
    return engine::lua::source::needs_calls();
#else
    return false;
#endif
}
struct Result {
    int load{-1},call{-1},protect{-1};
    double value{};
    bool number{},restored{},passed{};
    bool environment_created{},environment_verified{};
    unsigned error_line{};
    const char* error_kind{"none"};
};
struct Report { DWORD thread{}; bool passed{}; std::array<Result,5> steps{}; } report;

using engine::lua::read;
using engine::lua::Frame;
using engine::lua::capture;
using engine::lua::same_frame;
using engine::lua::valid_owner;
void error_details(void* vm,const char* label,Result& result) noexcept {
    const auto detail=engine::lua::error_details(vm,label);
    result.error_kind=detail.kind;result.error_line=detail.line;
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
session::Result persistent_execute(session::Context context,session::Action action,int reference) {
    namespace native=engine::lua;
    const bool error=action==session::Action::initialize_error;
    const bool listener_error=action==session::Action::initialize_listener_error;
    const bool rollback=action==session::Action::initialize_rollback;
    const auto bytes=events_probe?std::span<const unsigned char>(bytecode::persistent_events):
        error?std::span<const unsigned char>(bytecode::persistent_error):std::span<const unsigned char>(bytecode::persistent);
    // Invocation errors are attributed to their diagnostic source, independently
    // of the initialization action that selected the retained controller.
    const char* label=events_probe?"=crml_persistent_events":
        (error || action==session::Action::invoke)?"=crml_persistent_error":"=crml_persistent";
    const auto operation=action==session::Action::invoke?native::Action::invoke:
        action==session::Action::unload?native::Action::unload:
        action==session::Action::release?native::Action::release:native::Action::initialize;
    native::Options options;
    options.returns=native::Returns::number;
    options.environment_arguments=events_probe?(rollback?3u:listener_error?2u:error?1u:0u):0u;
    options.global_must_be_nil="__crml_persistent_counter";
    const auto execution=native::execute(api,context,operation,reference,{bytes,label},options,
        {&session::call_vm_live,&session::call_owner_live});
    session::Result result;
    result.attempted=execution.attempted;result.restored=execution.restored;
    result.released=execution.released;result.reference=execution.reference;
    result.status=execution.status;result.value=execution.value;result.shutdown=execution.shutdown;
    result.release_attempted=execution.release_attempted;result.error_line=execution.error.line;
    result.deliberate_error=action==session::Action::invoke && execution.status &&
        std::strcmp(execution.error.kind,"nil_call")==0 &&
        execution.error.line==(events_probe?bytecode::persistent_events_error_line:bytecode::persistent_error_error_line);
    return result;
}
}
bool start(uintptr_t image,Call original) noexcept {
#if defined(CRML_LUA_PROBE) || defined(CRML_LUA_PROBE_TESTING) || defined(CRML_LUA_SOURCE)
#if defined(CRML_LUA_SOURCE) && !defined(CRML_LUA_PROBE) && !defined(CRML_LUA_PROBE_TESTING)
    if(!source_mode()) return false;
#endif
    if(!image || !original) return false;
    constexpr unsigned char signatures[3][12]{
        {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x48,0x89},
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89},
        {0x48,0x63,0xc2,0x4c,0x8b,0xc1,0x85,0xd2,0x78,0x4c,0x48,0x8d}};
    const uintptr_t rvas[]{0x2c27b70,0x2c434f0,0x2c4d040};
    for(size_t i=0;i<3;++i) if(!compatibility::matches(reinterpret_cast<void*>(image+rvas[i]),signatures[i],12)) return false;
    constexpr unsigned char site[]{0xe8,0xb1,0x51,0x24,0x01,0x85,0xc0};
    if(!compatibility::matches(reinterpret_cast<void*>(image+0x1a0aada),site,sizeof(site))) return false;
    constexpr unsigned char push[]{0x48,0x8b,0x41,0x08,0x48,0x89,0x10,0x44,0x89,0x40,0x08};
    if(!compatibility::matches(reinterpret_cast<void*>(image+0x2c4ec50),push,sizeof(push))) return false;
    constexpr uintptr_t environment_rvas[]{0x2c4f290,0x2c4d0b0,0x2c4f6e0,0x2c4f320,0x2c4fa90,0x2c4f030};
    constexpr unsigned char environment_signatures[][12]{
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89},
        {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0xf6,0x41},
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48},
        {0x48,0x83,0xec,0x28,0x45,0x8b,0xd8,0x4c,0x8b,0xd1,0x85,0xd2},
        {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8d,0x59,0x08,0x4c,0x8b},
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89}};
    for(size_t i=0;i<6;++i) if(!compatibility::matches(reinterpret_cast<void*>(image+environment_rvas[i]),environment_signatures[i],12)) return false;
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
#if defined(CRML_LUA_PROBE) || defined(CRML_LUA_SOURCE)
    if(!enabled.load() || !image) return false;
    constexpr unsigned char retain[]{0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20};
    constexpr unsigned char fetch[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10};
    constexpr unsigned char release[]{0x85,0xd2,0x7e,0x46,0x57,0x48,0x83,0xec,0x20};
    if(!compatibility::matches(reinterpret_cast<void*>(image+0x2c507a0),retain,sizeof(retain)) ||
       !compatibility::matches(reinterpret_cast<void*>(image+0x2c4f1d0),fetch,sizeof(fetch)) ||
       !compatibility::matches(reinterpret_cast<void*>(image+0x2c508d0),release,sizeof(release))) return false;
    api.retain=reinterpret_cast<decltype(api.retain)>(image+0x2c507a0);
    api.fetch=reinterpret_cast<decltype(api.fetch)>(image+0x2c4f1d0);
    api.release=reinterpret_cast<decltype(api.release)>(image+0x2c508d0);
    if(!references::start(image)) return false;
#ifdef CRML_LUA_SOURCE
    if(source_mode()) return engine::lua::source::attach(api);
#endif
    events_probe=true;
    session::start(&persistent_execute,true);return true;
#else
    (void)image;return false;
#endif
}
Owner before_call(void* vm,uintptr_t caller,int nargs,int results,int error) noexcept {
    const auto revision=session::revision();
    const bool sources=source_mode();
    if((sources?!source_calls():!enabled.load(std::memory_order_acquire) && !session::needs_calls()) || (!sources && state.load()==1) ||
       caller!=image_base+0x1a0aadf || nargs!=1 || results!=0 || error!=1) return {};
    if(!sources && state.load()!=0 && !lifetime::active() && !session::needs_calls()) return {};
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
    const bool sources=source_mode();
    if((sources?!source_calls():!enabled.load(std::memory_order_acquire) && !session::needs_calls()) || (!sources && state.load()==1) || status ||
       caller!=image_base+0x1a0aadf || nargs!=1 || results!=0 || error!=1) return;
    Frame frame;
    if(!capture(vm,frame) || frame.size>60*24 || frame.world!=owner.world || !valid_owner(owner)) {++rejected;return;}
#ifdef CRML_LUA_SOURCE
    if(sources) {engine::lua::source::tick({vm,frame.global,frame.world,owner.entity,owner.revision},GetTickCount64());return;}
#endif
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
    if(source_mode()) {out<<"{\"type\":\"lua_probe\",\"schema\":4,\"enabled\":false,\"mode\":\"source\"}\n";return;}
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
