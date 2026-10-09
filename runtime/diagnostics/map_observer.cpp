#include "diagnostics/map_observer.h"
#include "map_projection.h"
#include "compatibility.h"
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <ostream>

namespace crml::map_observer {
namespace {
using Project=void*(*)(void*,const void*,const void*);
using Transform=void*(*)(void*,const void*);
using Rectangle=void*(*)(void*,const void*);
Project original_project{};
Transform original_transform{};
Rectangle original_rectangle{};
constexpr uintptr_t project_rva=0x1f52de0,transform_rva=0x1f529c0,rectangle_rva=0x1f34840;
constexpr unsigned char project_bytes[]{0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x57,0x48,0x81,0xec,0xc0};
constexpr unsigned char transform_bytes[]{0x4c,0x8b,0xdc,0x48,0x81,0xec,0x18,0x01,0,0,0xc5,0xfa,0x10,0x42,0x68,0xc5};
constexpr unsigned char rectangle_bytes[]{0x48,0x8b,0xc4,0x48,0x89,0x58,0x10,0x57,0x48,0x81,0xec,0xa0,0,0,0};
constexpr uint64_t closed=uint64_t{1}<<63;
uintptr_t image{};
bool attempted{},installed{};
bool diagnostic_enabled{true};
SnapshotCallback snapshot_callback{};
const char* startup_stage="not_started";
MH_STATUS startup_result=MH_OK;
std::atomic<uint64_t> admission{closed},deadline{},next_sample{},next_snapshot{},used{},calls{},dropped{},unwinds{},unrecognized{};
struct Event {uint64_t sequence{},time{};uint32_t thread{};bool readable{},transform{},stable{},predicted{},compared{},matched{};float error{};};
struct Frame {
    Event event{};
    const void* district{}; // Call-local only; never queued or retained.
    std::array<float,10> fields{};
    std::array<float,3> point{};
    map_projection::District projection{};
};
thread_local Frame* frame{};
struct Pending {
    map_projection::District projection{};
    const void* district{}; // TLS identity only, discarded at the next native call.
    const void* caller_slot{};
    uint64_t time{};
    bool valid{};
};
thread_local Pending pending{};
SRWLOCK queue_lock=SRWLOCK_INIT;
std::array<Event,64> queue{};
size_t head{},size{};
bool copy(void* dst,const void* src,size_t bytes) noexcept {
    __try {if(!src)return false;std::memcpy(dst,src,bytes);return true;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
}
bool admit(uint64_t now) noexcept {
    (void)now;
    auto v=admission.load(std::memory_order_acquire);
    while(!(v&closed) && v!=closed-1)
        if(admission.compare_exchange_weak(v,v+1,std::memory_order_acquire))return true;
    return false;
}
uint64_t sample(uint64_t now) noexcept {
    const auto until=deadline.load(std::memory_order_acquire);
    if(!diagnostic_enabled || !until || now>=until)return false;
    if(used.load()>=512)return false;
    auto previous=next_sample.load();
    if(now<previous || !next_sample.compare_exchange_strong(previous,now+250))return false;
    auto n=used.load();while(n<512)if(used.compare_exchange_weak(n,n+1))return n+1;
    return false;
}
bool select_snapshot(uint64_t now) noexcept {
    if(!snapshot_callback)return false;
    auto previous=next_snapshot.load(std::memory_order_relaxed);
    return now>=previous && next_snapshot.compare_exchange_strong(previous,now+50,std::memory_order_relaxed);
}
bool player_append(const void* caller_slot) noexcept {
    // The append frame pushes three registers and reserves 0x90 bytes. The
    // projector's return-address slot is therefore 0xb0 bytes below the
    // append's own return address. Read the latter only while this call lives.
    const auto slot=reinterpret_cast<uintptr_t>(caller_slot);
    if(!slot || slot>UINTPTR_MAX-0xb0)return false;
    uintptr_t outer_return{};
    return copy(&outer_return,reinterpret_cast<const void*>(slot+0xb0),sizeof(outer_return)) &&
        outer_return==image+0x1f541e6;
}
void enqueue(const Event& e) noexcept {
    if(!TryAcquireSRWLockExclusive(&queue_lock)){++dropped;return;}
    if(size==queue.size())++dropped;
    else {queue[(head+size)%queue.size()]=e;++size;}
    ReleaseSRWLockExclusive(&queue_lock);
}
void begin(Frame& f,const void* point,const void* district,uint64_t now) noexcept {
    f.district=district;f.event.time=now;f.event.thread=GetCurrentThreadId();
    f.event.readable=copy(f.point.data(),point,sizeof(f.point)) && district &&
        copy(f.fields.data(),reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(district)+0x74),sizeof(f.fields));
    if(!f.event.readable)return;
    for(size_t i=0;i<3;++i){f.projection.minimum[i]=f.fields[i];f.projection.maximum[i]=f.fields[i+3];}
    for(size_t i=0;i<2;++i){f.projection.offset[i]=f.fields[i+6];f.projection.scale[i]=f.fields[i+8];}
}
void finish(Frame& f,const void* result,const void* point) noexcept {
    std::array<float,2> actual{},predicted{};
    std::array<float,10> fields{};
    std::array<float,3> point_after{};
    f.event.stable=f.event.readable && copy(fields.data(),reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(f.district)+0x74),sizeof(fields)) &&
        copy(point_after.data(),point,sizeof(point_after)) && fields==f.fields && point_after==f.point;
    f.event.predicted=f.event.stable && f.event.transform && map_projection::normalized(f.projection,f.point,predicted);
    if(f.event.predicted && copy(actual.data(),result,sizeof(actual)) && std::isfinite(actual[0]) && std::isfinite(actual[1])) {
        f.event.compared=true;
        f.event.error=std::max(std::abs(actual[0]-predicted[0]),std::abs(actual[1]-predicted[1]));
        const float magnitude=std::max(std::abs(actual[0]),std::abs(actual[1]));
        f.event.matched=std::isfinite(f.event.error) && f.event.error<=0.0001f*(1+magnitude);
        if(!std::isfinite(f.event.error))f.event.error=0;
    }
    if(f.event.sequence)enqueue(f.event);
}
void capture_transform(void* out,const void* district,uintptr_t caller) noexcept {
    auto* f=frame;
    if(f && f->district==district && caller==image+0x1f52e32) {
        std::array<float,8> transform{};
        f->event.transform=copy(transform.data(),out,sizeof(transform));
        if(f->event.transform) {
            for(size_t i=0;i<4;++i)f->projection.rotation[i]=transform[i];
            for(size_t i=0;i<3;++i)f->projection.translation[i]=transform[i+4];
        }
    }
 }
void* transform_hook(void* out,const void* district) {
    void* result=original_transform(out,district);
    capture_transform(out,district,reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return result;
}
void* project_call(void* out,const void* point,const void* district,uintptr_t caller,const void* caller_slot) {
    const auto now=GetTickCount64();
    if(!admit(now))return original_project(out,point,district);
    void* result{};Frame local{};auto* previous=frame;bool selected{},live_selected{},returned{};
    __try {
        ++calls;
        // Ordinary map marker admission, not arbitrary callers of this helper.
        if(caller!=image+0x1f53945)++unrecognized;
        else {
            pending.valid=false;
            local.event.sequence=sample(now);
            live_selected=player_append(caller_slot) && select_snapshot(now);
            if(local.event.sequence || live_selected){selected=true;begin(local,point,district,now);frame=&local;}
        }
        result=original_project(out,point,district);returned=true;
        if(selected) {
            finish(local,out,point);
            if(live_selected && local.event.matched) {
                pending.projection=local.projection;
                pending.district=district;
                pending.caller_slot=caller_slot;
                pending.time=now;
                pending.valid=true;
            }
        }
    } __finally {
        frame=previous;
        if(!returned)++unwinds;
        admission.fetch_sub(1,std::memory_order_release);
    }
    return result;
}
void* project_hook(void* out,const void* point,const void* district) {
    return project_call(out,point,district,reinterpret_cast<uintptr_t>(_ReturnAddress()),_AddressOfReturnAddress());
}
void* rectangle_call(void* out,const void* district,uintptr_t caller,const void* caller_slot) {
    const auto now=GetTickCount64();
    if(!admit(now))return original_rectangle(out,district);
    void* result{};bool returned{};
    __try {
        const Pending selected=pending;
        pending.valid=false;
        result=original_rectangle(out,district);returned=true;
        if(selected.valid && caller==image+0x1f53985 && selected.caller_slot==caller_slot &&
           selected.district==district && now>=selected.time && now-selected.time<=100 && snapshot_callback) {
            std::array<float,4> rectangle{};
            if(copy(rectangle.data(),out,sizeof(rectangle)) &&
               std::all_of(rectangle.begin(),rectangle.end(),[](float f){return std::isfinite(f);}) &&
               rectangle[2]>0 && rectangle[3]>0)
                snapshot_callback(selected.projection,rectangle,now);
        }
    } __finally {
        if(!returned)++unwinds;
        admission.fetch_sub(1,std::memory_order_release);
    }
    return result;
}
void* rectangle_hook(void* out,const void* district) {
    return rectangle_call(out,district,reinterpret_cast<uintptr_t>(_ReturnAddress()),_AddressOfReturnAddress());
}
bool call_matches(uintptr_t at,uintptr_t target) noexcept {
    const int32_t delta=static_cast<int32_t>(target-(at+5));
    return compatibility::matches(reinterpret_cast<void*>(at),"\xe8",1) &&
        compatibility::matches(reinterpret_cast<void*>(at+1),&delta,4);
}
}
bool start() noexcept {return start(nullptr,true);}
bool start(SnapshotCallback callback,bool diagnostics) noexcept {
    startup_stage="profile";
    if(attempted || !compatibility::reviewed_build || compatibility::engine_profile!=compatibility::EngineProfile::october_patch)return false;
    attempted=true;image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* p=reinterpret_cast<void*>(image+project_rva);auto* t=reinterpret_cast<void*>(image+transform_rva);auto* r=reinterpret_cast<void*>(image+rectangle_rva);
    startup_stage="signatures";
    if(!compatibility::matches(p,project_bytes,sizeof(project_bytes)) || !compatibility::matches(t,transform_bytes,sizeof(transform_bytes)) ||
       !compatibility::matches(r,rectangle_bytes,sizeof(rectangle_bytes)) ||
       !call_matches(image+0x1f53940,image+project_rva) || !call_matches(image+0x1f52e2d,image+transform_rva) ||
       !call_matches(image+0x1f53980,image+rectangle_rva) || !call_matches(image+0x1f541e1,image+0x1f538f0))return false;
    startup_stage="initialize";
    startup_result=MH_Initialize();if(startup_result!=MH_OK && startup_result!=MH_ERROR_ALREADY_INITIALIZED)return false;
    startup_stage="project_create";
    if((startup_result=MH_CreateHook(p,reinterpret_cast<void*>(&project_hook),reinterpret_cast<void**>(&original_project)))!=MH_OK)return false;
    startup_stage="transform_create";
    if((startup_result=MH_CreateHook(t,reinterpret_cast<void*>(&transform_hook),reinterpret_cast<void**>(&original_transform)))!=MH_OK){MH_RemoveHook(p);return false;}
    startup_stage="rectangle_create";
    if((startup_result=MH_CreateHook(r,reinterpret_cast<void*>(&rectangle_hook),reinterpret_cast<void**>(&original_rectangle)))!=MH_OK){MH_RemoveHook(t);MH_RemoveHook(p);return false;}
    startup_stage="transform_enable";
    if((startup_result=MH_EnableHook(t))!=MH_OK){MH_RemoveHook(r);MH_RemoveHook(t);MH_RemoveHook(p);return false;}
    // Already-enabled code stays pinned on partial failure; TLS remains empty.
    startup_stage="rectangle_enable";
    if((startup_result=MH_EnableHook(r))!=MH_OK){MH_RemoveHook(r);MH_RemoveHook(p);return false;}
    startup_stage="project_enable";
    if((startup_result=MH_EnableHook(p))!=MH_OK){MH_RemoveHook(p);return false;}
    startup_stage="ready";
    snapshot_callback=callback;diagnostic_enabled=diagnostics;
    installed=true;deadline.store(diagnostics?GetTickCount64()+600000:0,std::memory_order_release);
    admission.store(0,std::memory_order_release);return true;
}
void report_startup(std::ostream& out) {
    out<<"Map projection startup: stage="<<startup_stage<<" hook_status="<<MH_StatusToString(startup_result)<<'\n';
}
void stop() noexcept {deadline.store(0,std::memory_order_release);admission.fetch_or(closed,std::memory_order_acq_rel);}
void poll(std::ostream& out) {
    static uint64_t last{};static bool finished{};
    if(!installed || !diagnostic_enabled || finished)return;
    const auto now=GetTickCount64();
    if(now>=deadline.load())deadline.store(0,std::memory_order_release);
    const bool final=!deadline.load() && !(admission.load()&~closed);
    if(last && now-last<1000 && !final)return;last=now;
    std::array<Event,64> batch{};size_t n{};
    AcquireSRWLockExclusive(&queue_lock);while(size){batch[n++]=queue[head];head=(head+1)%queue.size();--size;}ReleaseSRWLockExclusive(&queue_lock);
    const auto flags=out.flags();out<<std::dec<<std::noshowpos<<std::noboolalpha;
    for(size_t i=0;i<n;++i){const auto& e=batch[i];
        out<<"Capability map_projection: {\"schema\":1,\"type\":\"sample\",\"sequence\":"<<e.sequence<<",\"time_ms\":"<<e.time
           <<",\"status\":\""<<(e.readable?"ok":"unreadable")<<"\",\"thread\":"<<e.thread<<",\"readable\":"<<(e.readable?"true":"false")<<",\"transform\":"<<(e.transform?"true":"false")
           <<",\"stable\":"<<(e.stable?"true":"false")<<",\"predicted\":"<<(e.predicted?"true":"false")
           <<",\"compared\":"<<(e.compared?"true":"false")<<",\"matched\":"<<(e.matched?"true":"false")<<",\"max_error\":"<<e.error<<"}\n";}
    out<<"Capability map_projection: {\"schema\":1,\"type\":\"totals\",\"status\":\""<<(final?"stopped":"active")<<"\",\"calls\":"<<calls.load()
       <<",\"samples\":"<<used.load()<<",\"dropped\":"<<dropped.load()<<",\"unwinds\":"<<unwinds.load()<<",\"caller_rejected\":"<<unrecognized.load()<<"}\n";
    out.flags(flags);out.flush();if(final)finished=true;
}
}
