#include "diagnostics/sonar_observer.h"
#include "compatibility.h"
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include <ostream>

namespace crml::sonar_observer {
namespace {
using Geometry=void*(*)(void*,const void*,const void*,const void*,const void*,float,float,uint8_t);
using Worker=void(*)(const void*,const void*,const void*,const void*);
using Facts=void(*)(const void*,const void*,const void*,const void*,const void*,const void*,const float*);
Geometry original_geometry{};
Worker original_worker{};
Facts original_facts{};
constexpr uintptr_t geometry_rva=0x1e78280;
constexpr uintptr_t worker_rva=0x1e68cb0;
constexpr uintptr_t facts_rva=0x1e6ad20;
constexpr unsigned char geometry_bytes[]{0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18,0x41,0x56};
constexpr unsigned char worker_bytes[]{0x4c,0x8b,0xdc,0x55,0x41,0x56,0x48,0x81,0xec,0xe8,0x01,0x00,0x00};
constexpr unsigned char facts_bytes[]{0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x4c,0x89,0x40,0x18};
constexpr uint64_t closed=uint64_t{1}<<63;
std::atomic<uint64_t> admission{closed},next_sample{},next_geometry_sample{},calls{},matched{},rejected{},unwinds{},heartbeats{},published{};
std::atomic<float> interpolation{-1};
std::atomic<uint64_t> interpolation_time{};
std::atomic<bool> math_mismatch{};
uintptr_t image{};
bool attempted{},installed{};
SnapshotCallback snapshot_callback{};
const char* startup_stage="not_started";
MH_STATUS startup_result=MH_OK;
struct Pending {
    sonar_projection::Frame frame{};
    const void* caller_slot{};
    uint64_t time{};
    float error{};
    bool valid{};
};
thread_local Pending pending{};
thread_local float worker_comparison_error{};
bool copy(void* dst,const void* src,size_t size) noexcept {
    __try {if(!src)return false;std::memcpy(dst,src,size);return true;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
}
bool admit() noexcept {
    auto current=admission.load(std::memory_order_acquire);
    while(!(current&closed) && current!=closed-1)
        if(admission.compare_exchange_weak(current,current+1,std::memory_order_acquire))return true;
    return false;
}
bool call_matches(uintptr_t at,uintptr_t target) noexcept {
    const int32_t delta=static_cast<int32_t>(target-(at+5));
    return compatibility::matches(reinterpret_cast<void*>(at),"\xe8",1) &&
        compatibility::matches(reinterpret_cast<void*>(at+1),&delta,4);
}
bool player_worker(const void* caller_slot) noexcept {
    // Wrapper: push r13/r14/r15; sub rsp,0x1a0. Its return address is
    // 0x1c0 above the geometry call's return-address slot.
    const auto slot=reinterpret_cast<uintptr_t>(caller_slot);
    if(!slot || slot>UINTPTR_MAX-0x1c0)return false;
    uintptr_t outer_return{};
    return copy(&outer_return,reinterpret_cast<const void*>(slot+0x1c0),sizeof(outer_return)) &&
           outer_return==image+0x1e68ed3;
}
bool sample(uint64_t now) noexcept {
    auto previous=next_sample.load(std::memory_order_relaxed);
    return now>=previous && next_sample.compare_exchange_strong(previous,now+16,std::memory_order_relaxed);
}
bool geometry_sample(uint64_t now) noexcept {
    auto previous=next_geometry_sample.load(std::memory_order_relaxed);
    return now>=previous && next_geometry_sample.compare_exchange_strong(previous,now+50,std::memory_order_relaxed);
}
bool read_frame(sonar_projection::Frame& out,const void* reference,const void* plane,
                const void* origin,const void* target,float near_radius,float far_radius,uint8_t flag) noexcept {
    out.near_radius=near_radius;out.far_radius=far_radius;out.flag=flag;
    return copy(out.reference.data(),reference,sizeof(out.reference)) &&
           copy(out.plane.data(),plane,sizeof(out.plane)) &&
           copy(out.origin.data(),origin,sizeof(out.origin)) &&
           copy(out.target.data(),target,sizeof(out.target));
}
bool same_frame(const sonar_projection::Frame& a,const sonar_projection::Frame& b) noexcept {
    return a.reference==b.reference && a.plane==b.plane && a.origin==b.origin &&
           a.target==b.target && a.near_radius==b.near_radius &&
           a.far_radius==b.far_radius && a.flag==b.flag;
}
bool read_result(sonar_projection::NativeResult& out,const void* result) noexcept {
    std::array<unsigned char,0x30> bytes{};
    if(!copy(bytes.data(),result,bytes.size()))return false;
    std::memcpy(&out.radial_class,bytes.data(),4);
    std::memcpy(&out.vertical_class,bytes.data()+4,4);
    std::memcpy(out.origin.data(),bytes.data()+0x10,16);
    std::memcpy(&out.distance,bytes.data()+0x20,4);
    std::memcpy(&out.scale,bytes.data()+0x24,4);
    std::memcpy(out.uv.data(),bytes.data()+0x28,8);
    return true;
}
void* geometry_call(void* result,const void* reference,const void* plane,const void* origin,
                    const void* target,float near_radius,float far_radius,uint8_t flag,
                    uintptr_t caller,const void* caller_slot) {
    if(!admit())return original_geometry(result,reference,plane,origin,target,near_radius,far_radius,flag);
    const auto now=GetTickCount64();
    void* returned_value{};bool returned{};
    __try {
        ++calls;
        const bool first=caller==image+0x1e77ddd;
        const bool second=caller==image+0x1e77e6a;
        if(first)pending.valid=false;
        const bool selected=(first && player_worker(caller_slot) && geometry_sample(now)) ||
                            (second && pending.valid && pending.caller_slot==caller_slot &&
                             now>=pending.time && now-pending.time<=100);
        sonar_projection::Frame before{},after{};
        const bool readable=selected && read_frame(before,reference,plane,origin,target,near_radius,far_radius,flag);
        returned_value=original_geometry(result,reference,plane,origin,target,near_radius,far_radius,flag);
        returned=true;
        if(readable && read_frame(after,reference,plane,origin,target,near_radius,far_radius,flag) && same_frame(before,after)) {
            sonar_projection::NativeResult native{};
            if(read_result(native,result)) {
                if(first) {
                    float error{};
                    if(sonar_projection::compare_native(before,native,error)) {
                        pending.frame=before;
                        pending.caller_slot=caller_slot;pending.time=now;pending.error=error;pending.valid=true;
                    } else {
                        sonar_projection::Model model{};
                        if(sonar_projection::model_from_frame(before,model))math_mismatch.store(true,std::memory_order_release);
                        ++rejected;
                    }
                } else if(second) {
                    const auto first_capture=pending;
                    pending.valid=false;
                    float error{};
                    if(before.target==first_capture.frame.target &&
                       before.plane==first_capture.frame.plane &&
                       sonar_projection::compare_native(before,native,error)) {
                        worker_comparison_error=std::max(first_capture.error,error);
                        ++matched;
                    } else {
                        sonar_projection::Model model{};
                        if(sonar_projection::model_from_frame(before,model))math_mismatch.store(true,std::memory_order_release);
                        ++rejected;
                    }
                }
            } else ++rejected;
        } else if(selected)++rejected;
    } __finally {
        if(!returned)++unwinds;
        admission.fetch_sub(1,std::memory_order_release);
    }
    return returned_value;
}
void* geometry_hook(void* result,const void* reference,const void* plane,const void* origin,
                    const void* target,float near_radius,float far_radius,uint8_t flag) {
    return geometry_call(result,reference,plane,origin,target,near_radius,far_radius,flag,
                         reinterpret_cast<uintptr_t>(_ReturnAddress()),_AddressOfReturnAddress());
}
bool read_model(sonar_projection::Model& out,uintptr_t camera,uintptr_t plane,
                uintptr_t transform,uint64_t row,uint64_t camera_offset) noexcept {
    if(row>0x100000||camera>UINTPTR_MAX-row*0xb0-camera_offset-16||
       plane>UINTPTR_MAX-row*0x10-16||transform>UINTPTR_MAX-row*0x20-0x20)return false;
    std::array<float,4> origin{};
    if(!copy(out.reference.data(),reinterpret_cast<void*>(camera+row*0xb0+camera_offset),16)||
       !copy(out.plane.data(),reinterpret_cast<void*>(plane+row*0x10),16)||
       !copy(origin.data(),reinterpret_cast<void*>(transform+row*0x20+0x10),16))return false;
    out.origin={origin[0],origin[1],origin[2]};
    return true;
}
bool read_worker(sonar_projection::Snapshot& out,const void* row_ptr,const void* sonar) noexcept {
    // Worker receives the four component-base pointers in [rcx+0..0x18],
    // and the matching player row index at [rcx+0x28]. It performs these
    // exact stride/index loads before entering the per-marker loop.
    std::array<uint64_t,6> row{};
    uint8_t state{},hidden{};
    if(!sonar||!copy(row.data(),row_ptr,sizeof(row))||!copy(&state,sonar,1)||
       !copy(&hidden,reinterpret_cast<const uint8_t*>(sonar)+0x80,1)||
       !state||!(state&2)||hidden)return false;
    const auto index=row[5];
    if(!read_model(out.current,row[2],row[3],row[0],index,0x60)||
       !read_model(out.previous,row[2],row[3],row[1],index,0x80)||
       !copy(&out.current.near_radius,reinterpret_cast<void*>(image+0x5c74578),4)||
       !copy(&out.current.far_radius,reinterpret_cast<void*>(image+0x5c745c8),4))return false;
    out.previous.near_radius=out.current.near_radius;
    out.previous.far_radius=out.current.far_radius;
    const auto now=GetTickCount64(),blend_time=interpolation_time.load(std::memory_order_acquire);
    out.interpolation=interpolation.load(std::memory_order_relaxed);
    out.comparison_error=worker_comparison_error;
    return blend_time&&now>=blend_time&&now-blend_time<=250&&sonar_projection::valid(out);
}
bool same_model(const sonar_projection::Model& a,const sonar_projection::Model& b) noexcept {
    return a.reference==b.reference&&a.plane==b.plane&&a.origin==b.origin&&
           a.near_radius==b.near_radius&&a.far_radius==b.far_radius;
}
void worker_call(const void* row,const void* sonar,const void* markers,const void* district,uintptr_t caller) {
    if(caller!=image+0x1e71fde||!admit()){
        original_worker(row,sonar,markers,district);return;
    }
    bool returned{};sonar_projection::Snapshot snapshot{};
    const bool ready=read_worker(snapshot,row,sonar);
    __try {
        original_worker(row,sonar,markers,district);
        returned=true;
        ++heartbeats;
        const auto now=GetTickCount64();
        sonar_projection::Snapshot after{};
        const bool stable=ready&&read_worker(after,row,sonar)&&
            same_model(snapshot.current,after.current)&&same_model(snapshot.previous,after.previous);
        if(stable&&!math_mismatch.load(std::memory_order_acquire)&&snapshot_callback&&sample(now)) {
            snapshot.comparison_error=worker_comparison_error;
            snapshot_callback(snapshot,now);
            ++published;
        }
    } __finally {
        if(!returned)++unwinds;
        admission.fetch_sub(1,std::memory_order_release);
    }
}
void worker_hook(const void* row,const void* sonar,const void* markers,const void* district) {
    worker_call(row,sonar,markers,district,reinterpret_cast<uintptr_t>(_ReturnAddress()));
}
void facts_call(const void* a,const void* b,const void* c,const void* d,
                const void* e,const void* f,const float* blend,uintptr_t caller) {
    if(caller!=image+0x1e70fca||!admit()){
        original_facts(a,b,c,d,e,f,blend);return;
    }
    float value{};bool returned{};
    const bool readable=copy(&value,blend,4)&&std::isfinite(value)&&value>=0&&value<=1;
    __try {
        original_facts(a,b,c,d,e,f,blend);
        returned=true;
        if(readable){interpolation.store(value,std::memory_order_relaxed);
                     interpolation_time.store(GetTickCount64(),std::memory_order_release);}
    } __finally {
        if(!returned)++unwinds;
        admission.fetch_sub(1,std::memory_order_release);
    }
}
void facts_hook(const void* a,const void* b,const void* c,const void* d,
                const void* e,const void* f,const float* blend) {
    facts_call(a,b,c,d,e,f,blend,reinterpret_cast<uintptr_t>(_ReturnAddress()));
}
}
bool start(SnapshotCallback callback) noexcept {
    startup_stage="profile";
    if(attempted || !callback || !compatibility::reviewed_build ||
       compatibility::engine_profile!=compatibility::EngineProfile::october_patch)return false;
    attempted=true;image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* geometry=reinterpret_cast<void*>(image+geometry_rva);
    auto* worker=reinterpret_cast<void*>(image+worker_rva);
    auto* facts=reinterpret_cast<void*>(image+facts_rva);
    startup_stage="signatures";
    if(!compatibility::matches(geometry,geometry_bytes,sizeof(geometry_bytes)) ||
       !compatibility::matches(worker,worker_bytes,sizeof(worker_bytes)) ||
       !compatibility::matches(facts,facts_bytes,sizeof(facts_bytes)) ||
       !call_matches(image+0x1e77dd8,geometry_rva+image) ||
       !call_matches(image+0x1e77e65,geometry_rva+image) ||
       !call_matches(image+0x1e68ece,image+0x1e77ce0) ||
       !call_matches(image+0x1e71fd9,worker_rva+image) ||
       !call_matches(image+0x1e70fc5,facts_rva+image))return false;
    startup_stage="initialize";
    startup_result=MH_Initialize();if(startup_result!=MH_OK && startup_result!=MH_ERROR_ALREADY_INITIALIZED)return false;
    startup_stage="create";
    if((startup_result=MH_CreateHook(geometry,reinterpret_cast<void*>(&geometry_hook),reinterpret_cast<void**>(&original_geometry)))!=MH_OK)return false;
    if((startup_result=MH_CreateHook(worker,reinterpret_cast<void*>(&worker_hook),reinterpret_cast<void**>(&original_worker)))!=MH_OK)return false;
    if((startup_result=MH_CreateHook(facts,reinterpret_cast<void*>(&facts_hook),reinterpret_cast<void**>(&original_facts)))!=MH_OK)return false;
    startup_stage="enable";
    if((startup_result=MH_EnableHook(geometry))!=MH_OK)return false;
    if((startup_result=MH_EnableHook(worker))!=MH_OK)return false;
    if((startup_result=MH_EnableHook(facts))!=MH_OK)return false;
    snapshot_callback=callback;installed=true;admission.store(0,std::memory_order_release);
    startup_stage="ready";return true;
}
void stop() noexcept {admission.fetch_or(closed,std::memory_order_acq_rel);}
void report_startup(std::ostream& out){out<<"Sonar projection startup: stage="<<startup_stage<<" hook_status="<<MH_StatusToString(startup_result)<<'\n';}
void poll(std::ostream& out){
    if(!installed)return;
    static uint64_t last{},old_calls{},old_matched{},old_rejected{},old_unwinds{},old_heartbeats{},old_published{};
    const auto now=GetTickCount64();
    if(last && now-last<10000)return;last=now;
    const auto c=calls.load(),m=matched.load(),r=rejected.load(),u=unwinds.load(),h=heartbeats.load(),p=published.load();
    if(c==old_calls&&m==old_matched&&r==old_rejected&&u==old_unwinds&&h==old_heartbeats&&p==old_published)return;
    old_calls=c;old_matched=m;old_rejected=r;old_unwinds=u;old_heartbeats=h;old_published=p;
    out<<"Capability sonar_projection: {\"schema\":1,\"calls\":"<<calls.load()
       <<",\"matched\":"<<matched.load()<<",\"rejected\":"<<rejected.load()
       <<",\"unwinds\":"<<unwinds.load()<<",\"heartbeats\":"<<h
       <<",\"published\":"<<p<<",\"math_mismatch\":"
       <<(math_mismatch.load()?"true":"false")<<"}\n";
}
}
