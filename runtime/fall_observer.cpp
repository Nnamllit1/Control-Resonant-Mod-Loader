#include "fall_observer.h"
#include "movement_view.h"
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <ostream>

namespace crml::probe::fall_trace {
namespace {
template<class T> T read(uintptr_t at) noexcept { T v{};std::memcpy(&v,reinterpret_cast<const void*>(at),sizeof(v));return v; }
using Four=void(*)(void*,void*,void*,void*);
using Two=void(*)(void*,void*);
using Eight=void(*)(void*,void*,void*,void*,void*,void*,void*,void*);
Four original_inactive{},original_camera{};
Two original_trigger{};
Eight original_recovery{};
fall::Active player_callback{};
std::atomic<bool> enabled{};
std::array<std::atomic<uint64_t>,5> matched{},unreadable{};
std::atomic<uint64_t> dropped{};
std::atomic<uint64_t> identity_unavailable{};
constexpr const char* names[]{"controller","inactive","trigger","recovery","camera"};
struct Event { uint64_t sequence{},tick{};DWORD thread{};Stage stage{};bool before_valid{};State before{},after{}; };
SRWLOCK lock=SRWLOCK_INIT;
std::array<Event,128> events{};
size_t count{};
uint64_t sequence{};
State last_controller{};
uintptr_t controller_world{};
bool have_controller{};

void record(Stage stage,bool valid,const State& before,const State& after,bool first=false) noexcept {
    if(!enabled.load(std::memory_order_acquire)) return;
    if(!first && valid && !changed(before,after)) return;
    if(!TryAcquireSRWLockExclusive(&lock)) { ++dropped;return; }
    if(count==events.size() || sequence>=2048) ++dropped;
    else events[count++]={++sequence,GetTickCount64(),GetCurrentThreadId(),stage,valid,before,after};
    ReleaseSRWLockExclusive(&lock);
}
fall::Player player() noexcept {
    if(!enabled.load(std::memory_order_acquire)) return {};
    const auto p=player_callback();if(!p.entity) ++identity_unavailable;return p;
}
bool camera_matches(const void* view,fall::Player p) noexcept {
    __try {
        uintptr_t chunk{};uint32_t row{};
        const auto data=entity_component(p.world,p.entity,0x6507c6a9,240,chunk,row);
        const auto v=reinterpret_cast<uintptr_t>(view);
        return data && v && read<uintptr_t>(v+24)==chunk && read<uint64_t>(v+32)==row && read<uintptr_t>(v)+row*240ull==data;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool query_matches(const void* query,fall::Player p) noexcept {
    __try { return query && p.world && p.entity && read<uintptr_t>(reinterpret_cast<uintptr_t>(query))==p.world; }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
void finish(Stage stage,fall::Player p,bool valid,const State& before,const void* fade=nullptr) noexcept {
    if(!p.entity) return;
    State after{};
    const auto i=static_cast<size_t>(stage);
    const bool first=matched[i].fetch_add(1)==0;
    if(!snapshot(p,after,fade)) {++unreadable[i];return;}
    if(!valid) ++unreadable[i];
    record(stage,valid,before,after,first);
}
void inactive(void* view,void* anomalies,void* query,void* physics) {
    auto p=player();if(!fall::matches_inactive(view,p)) p={};
    State before{};const bool valid=p.entity && snapshot(p,before);
    original_inactive(view,anomalies,query,physics);
    finish(Stage::inactive,p,valid,before);
}
void trigger(void* view,void* query) {
    auto p=player();if(!query_matches(query,p)) p={};
    State before{};const bool valid=p.entity && snapshot(p,before);
    original_trigger(view,query);
    finish(Stage::trigger,p,valid,before);
}
void recovery(void* view,void* physics,void* time,void* state,void* damage,void* transition,void* event_bus,void* counter) {
    auto p=player();if(!fall::matches_active_recovery(view,p)) p={};
    State before{};const bool valid=p.entity && snapshot(p,before);
    original_recovery(view,physics,time,state,damage,transition,event_bus,counter);
    finish(Stage::recovery,p,valid,before);
}
void camera(void* view,void* mixer,void* requests,void* fade) {
    auto p=player();if(!camera_matches(view,p)) p={};
    State before{};const bool valid=p.entity && snapshot(p,before,fade);
    original_camera(view,mixer,requests,fade);
    finish(Stage::camera,p,valid,before,fade);
}
void scalar(std::ostream& out,float v) { if(std::isfinite(v)) out<<v;else out<<"null"; }
void vector(std::ostream& out,const std::array<float,3>& v) {out<<'[';scalar(out,v[0]);out<<',';scalar(out,v[1]);out<<',';scalar(out,v[2]);out<<']';}
void state_json(std::ostream& out,const State& s) {
    out<<"{\"entity\":"<<s.entity<<",\"source_entity\":"<<s.source
       <<",\"flags\":"<<unsigned(s.flags)<<",\"blocked\":"<<unsigned(s.blocked)<<",\"safe_valid\":"<<unsigned(s.safe_valid)
       <<",\"teleported\":"<<unsigned(s.teleported)<<",\"camera_present\":"<<unsigned(s.camera_present)
       <<",\"camera_active\":"<<unsigned(s.camera_active)<<",\"fade_latched\":"<<unsigned(s.fade_latched)
       <<",\"elapsed\":";scalar(out,s.elapsed);out<<",\"transition_delay\":";scalar(out,s.transition_delay);
    out<<",\"effect_delay\":";scalar(out,s.effect_delay);out<<",\"position\":";vector(out,s.position);
    out<<",\"safe_position\":";vector(out,s.safe_position);
    out<<",\"fade_request\":";
    if(!s.fade_present) out<<"null";
    else {out<<"{\"level_raw\":"<<s.fade_level<<",\"duration\":";scalar(out,s.fade_duration);out<<",\"pending_raw\":"<<unsigned(s.fade_pending)<<",\"aux_raw\":"<<unsigned(s.fade_aux)<<'}';}
    out<<'}';
}
}
bool snapshot(fall::Player p,State& out,const void* fade) noexcept {
    out={};
    __try {
        uintptr_t chunk{};uint32_t row{};
        const auto data=entity_component(p.world,p.entity,0x6507c6a9,240,chunk,row);
        const auto transform=entity_component(p.world,p.entity,0x6cfbb2a9,32,chunk,row);
        if(!data || !transform) return false;
        State s{};s.entity=p.entity;
        s.blocked=read<uint8_t>(data+0xe4);s.flags=read<uint8_t>(data+0xe5);s.safe_valid=read<uint8_t>(data+0x40);
        s.source=read<uint64_t>(data+0xd0);s.elapsed=read<float>(data+0xe0);
        s.effect_delay=read<float>(data+0xb0);s.transition_delay=read<float>(data+0xb4);
        std::memcpy(s.safe_position.data(),reinterpret_cast<const void*>(data+0x20),12);
        std::memcpy(s.position.data(),reinterpret_cast<const void*>(transform+16),12);
        const auto cam=entity_component(p.world,p.entity,0xa342c1c2,16,chunk,row);
        if(cam) {s.camera_present=1;s.camera_active=read<uint8_t>(cam);s.fade_latched=read<uint8_t>(cam+1);}
        const auto teleported=entity_component(p.world,p.entity,0xc55ae319,1,chunk,row);
        if(teleported) s.teleported=read<uint8_t>(teleported);
        if(fade) {
            const auto at=reinterpret_cast<uintptr_t>(fade);s.fade_present=1;
            s.fade_level=read<uint32_t>(at);s.fade_duration=read<float>(at+4);
            s.fade_aux=read<uint8_t>(at+8);s.fade_pending=read<uint8_t>(at+12);
        }
        out=s;return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {out={};return false;}
}
bool changed(const State& a,const State& b) noexcept {
    // Continuous positions/timers are contextual evidence, not per-frame events.
    return a.entity!=b.entity || a.flags!=b.flags || a.blocked!=b.blocked || a.safe_valid!=b.safe_valid ||
        a.camera_present!=b.camera_present || a.camera_active!=b.camera_active || a.fade_latched!=b.fade_latched ||
        a.teleported!=b.teleported || a.fade_present!=b.fade_present ||
        (a.fade_present && (a.fade_level!=b.fade_level || a.fade_pending!=b.fade_pending || a.fade_aux!=b.fade_aux));
}
void controller(fall::Player p) noexcept {
    if(!enabled.load(std::memory_order_acquire)) return;
    State current{};
    if(!snapshot(p,current)) {++unreadable[0];return;}
    ++matched[0];
    if(!TryAcquireSRWLockExclusive(&lock)) {++dropped;return;}
    const bool valid=have_controller && controller_world==p.world && last_controller.entity==p.entity;
    const auto before=last_controller;
    last_controller=current;controller_world=p.world;have_controller=true;
    ReleaseSRWLockExclusive(&lock);
    record(Stage::controller,valid,before,current);
}
bool start(uintptr_t image,fall::Active callback) noexcept {
    if(!image || !callback || player_callback) return false;
    const std::array<uintptr_t,4> rvas{0x25a7230,0x25a8360,0x25a7620,0x25a85f0};
    constexpr unsigned char signatures[4][11]{
        {0x48,0x8b,0xc4,0x56,0x41,0x55,0x41,0x56,0x41,0x57,0x48},
        {0x4c,0x8b,0xdc,0x49,0x89,0x5b,0x10,0x49,0x89,0x6b,0x18},
        {0x4c,0x8b,0xdc,0x4d,0x89,0x4b,0x20,0x49,0x89,0x53,0x10},
        {0x48,0x8b,0xc4,0x48,0x89,0x58,0x10,0x48,0x89,0x70,0x18}};
    void* detours[]{reinterpret_cast<void*>(&inactive),reinterpret_cast<void*>(&trigger),reinterpret_cast<void*>(&recovery),reinterpret_cast<void*>(&camera)};
    void** originals[]{reinterpret_cast<void**>(&original_inactive),reinterpret_cast<void**>(&original_trigger),reinterpret_cast<void**>(&original_recovery),reinterpret_cast<void**>(&original_camera)};
    for(size_t i=0;i<rvas.size();++i) if(std::memcmp(reinterpret_cast<void*>(image+rvas[i]),signatures[i],11)) return false;
    player_callback=callback;
    for(size_t i=0;i<rvas.size();++i)
        if(MH_CreateHook(reinterpret_cast<void*>(image+rvas[i]),detours[i],originals[i])!=MH_OK) return false;
    // No state changes or skipped originals even during partial installation.
    // Trampolines stay pinned and passive if enabling any hook fails.
    for(auto rva:rvas) if(MH_EnableHook(reinterpret_cast<void*>(image+rva))!=MH_OK) return false;
    enabled.store(true,std::memory_order_release);return true;
}
void write(std::ostream& out) {
    std::array<Event,128> batch{};size_t size{};
    AcquireSRWLockExclusive(&lock);size=count;std::copy_n(events.begin(),size,batch.begin());count=0;ReleaseSRWLockExclusive(&lock);
    out<<std::setprecision(9);
    for(size_t i=0;i<size;++i) {
        const auto& e=batch[i];out<<"{\"type\":\"transition\",\"sequence\":"<<e.sequence<<",\"tick_ms\":"<<e.tick<<",\"thread\":"<<e.thread
            <<",\"stage\":\""<<names[static_cast<size_t>(e.stage)]<<"\",\"before\":";
        if(e.before_valid) state_json(out,e.before);else out<<"null";
        out<<",\"after\":";state_json(out,e.after);out<<"}\n";
    }
    out<<"{\"type\":\"counters\",\"tick_ms\":"<<GetTickCount64()<<",\"dropped\":"<<dropped.load()
       <<",\"identity_unavailable\":"<<identity_unavailable.load()<<",\"stages\":{";
    for(size_t i=0;i<matched.size();++i) {if(i) out<<',';out<<'"'<<names[i]<<"\":{\"matched\":"<<matched[i].load()<<",\"unreadable\":"<<unreadable[i].load()<<'}';}
    out<<"}}\n";
}
void stop() noexcept {enabled.store(false,std::memory_order_release);}
#ifdef CRML_FALL_TRACE_TESTING
namespace testing {
void configure(fall::Active callback,void* a,void* b,void* c,void* d) noexcept {
    stop();player_callback=callback;
    original_inactive=reinterpret_cast<Four>(a);original_trigger=reinterpret_cast<Two>(b);
    original_recovery=reinterpret_cast<Eight>(c);original_camera=reinterpret_cast<Four>(d);
    for(auto& n:matched) n=0;for(auto& n:unreadable) n=0;dropped=0;identity_unavailable=0;
    count=0;sequence=0;have_controller=false;controller_world=0;last_controller={};
    enabled.store(true,std::memory_order_release);
}
void invoke(Stage s,const std::array<void*,8>& a) {
    switch(s) {
    case Stage::inactive:inactive(a[0],a[1],a[2],a[3]);break;
    case Stage::trigger:trigger(a[0],a[1]);break;
    case Stage::recovery:recovery(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);break;
    case Stage::camera:camera(a[0],a[1],a[2],a[3]);break;
    default:break;
    }
}
void hold_lock(bool hold) noexcept {if(hold) AcquireSRWLockExclusive(&lock);else ReleaseSRWLockExclusive(&lock);}
}
#endif
}
