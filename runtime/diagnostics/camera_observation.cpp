#include "diagnostics/camera_observation.h"
#include "movement_view.h"
#include <Windows.h>
#include <cmath>
#include <cstring>

namespace crml::camera {
namespace {
template<class T> T read(uintptr_t address) noexcept {
    T value;std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));return value;
}
uintptr_t environment(uintptr_t world) noexcept {
    const auto count=read<uint32_t>(world+0x585b8);
    if(!count || count>16384) return 0;
    const auto hashes=read<uintptr_t>(world+0x585b0),values=read<uintptr_t>(world+0x585c0);
    if(!hashes || !values) return 0;
    uintptr_t result{};bool found=false;
    for(uint32_t i=0;i<count;++i) if(read<uint32_t>(hashes+i*4ull)==0xfe90f7f8) {
        if(found) return 0; // An ambiguous global table cannot authenticate a view.
        found=true;
        result=read<uintptr_t>(values+i*8ull);
    }
    return result;
}
bool alive(uintptr_t world,uint64_t entity) noexcept {
    const auto index=static_cast<uint32_t>(entity);
    const auto capacity=read<uint64_t>(world+0x58510);
    if(index==UINT32_MAX || !capacity || capacity>16*1024*1024 || index>=capacity) return false;
    const auto generations=read<uintptr_t>(world+0x584e8),locations=read<uintptr_t>(world+0x58530);
    if(!generations || !locations || read<uint32_t>(generations+index*8ull)!=entity>>32) return false;
    const auto location=read<uint64_t>(locations+index*8ull);
    const auto archetype=static_cast<uint16_t>(location);
    const auto row=static_cast<uint32_t>(location>>32);
    const auto metadata=read<uintptr_t>(world+0x58478);
    if(!metadata) return false;
    const auto count=read<uint64_t>(metadata+8);
    if(count>8192 || archetype>=count || row>=16384 || row>=read<uint32_t>(world+0x10448+archetype*4ull)) return false;
    const auto chunk=read<uintptr_t>(world+0x50+archetype*8ull);
    return chunk && read<uint64_t>(chunk+0x10+row*8ull)==entity;
}
uint64_t digest(const void* data,size_t size) noexcept {
    auto bytes=static_cast<const unsigned char*>(data);uint64_t hash=14695981039346656037ull;
    for(size_t i=0;i<size;++i) {hash^=bytes[i];hash*=1099511628211ull;}
    return hash?hash:1;
}
bool finite(const float* data,size_t count) noexcept {
    for(size_t i=0;i<count;++i) if(!std::isfinite(data[i])) return false;
    return true;
}
void slot(uintptr_t world,uint64_t entity,Slot& result) noexcept {
    if(!entity || static_cast<uint32_t>(entity)==UINT32_MAX) return;
    result.entity=entity;result.flags=present;
    if(!alive(world,entity)) return;
    result.flags|=live;
    uintptr_t chunk{};uint32_t row{};
    const auto camera=probe::entity_component(world,entity,0x46967561,64,chunk,row);
    if(camera) {
        float values[14];std::memcpy(values,reinterpret_cast<void*>(camera),sizeof(values));
        bool valid=finite(values,14);
        for(unsigned a=0;a<3 && valid;++a) for(unsigned b=a;b<3;++b) {
            float dot{};for(unsigned i=0;i<3;++i) dot+=values[a*3+i]*values[b*3+i];
            if(std::abs(dot-(a==b?1.f:0.f))>.1f) valid=false;
        }
        if(valid) {result.flags|=view;result.view_digest=digest(values,sizeof(values));}
        else result.flags|=invalid_pose;
    }
    const auto free=probe::entity_component(world,entity,0x01a3288e,112,chunk,row);
    if(free) {
        float values[6];std::memcpy(values,reinterpret_cast<void*>(free+0x30),12);
        std::memcpy(values+3,reinterpret_cast<void*>(free+0x50),12);
        if(finite(values,6)) {result.flags|=free_transform;result.free_digest=digest(values,sizeof(values));}
        else result.flags|=invalid_pose;
    }
    if(!alive(world,entity) ||
       probe::entity_component(world,entity,0x46967561,64,chunk,row)!=camera ||
       probe::entity_component(world,entity,0x01a3288e,112,chunk,row)!=free) {
        result.flags=present|changed;result.view_digest=0;result.free_digest=0;
    }
}
Read inspect_inner(uintptr_t world,uintptr_t expected,Snapshot& out) noexcept {
    if(!world) return Read::arguments;
    const auto global=environment(world);
    if(!global || (expected && expected!=global)) return Read::environment;
    unsigned char before[36];std::memcpy(before,reinterpret_cast<void*>(global),sizeof(before));
    std::memcpy(&out.selector,before,4);
    if(out.selector<0 || out.selector>3) return Read::selector;
    out.world=world;out.global=global;
    for(unsigned i=0;i<4;++i) {uint64_t handle;std::memcpy(&handle,before+4+i*8,8);slot(world,handle,out.slots[i]);}
    if(environment(world)!=global || std::memcmp(before,reinterpret_cast<void*>(global),sizeof(before))) return Read::changed;
    return Read::ok;
}
Read selected_inner(void* context,Selected& out) noexcept {
    const auto at=reinterpret_cast<uintptr_t>(context);
    if(!at) return Read::arguments;
    const auto world=read<uintptr_t>(at),global=read<uintptr_t>(at+0x28);
    if(!world || !global || environment(world)!=global) return Read::environment;
    unsigned char before[36];std::memcpy(before,reinterpret_cast<void*>(global),sizeof(before));
    int32_t mode;std::memcpy(&mode,before,4);
    if(mode<0 || mode>3) return Read::selector;
    uint64_t entity;std::memcpy(&entity,before+4+mode*8,8);
    if(!entity || !alive(world,entity)) return Read::changed;
    uintptr_t chunk{};uint32_t row{};
    const auto view=probe::entity_component(world,entity,0x46967561,64,chunk,row);
    if(!view) return Read::changed;
    float values[14];std::memcpy(values,reinterpret_cast<void*>(view),sizeof(values));
    if(!finite(values,12)) return Read::changed;
    for(unsigned a=0;a<3;++a) for(unsigned b=a;b<3;++b) {
        float dot{};for(unsigned i=0;i<3;++i) dot+=values[a*3+i]*values[b*3+i];
        if(std::abs(dot-(a==b?1.f:0.f))>.1f) return Read::changed;
    }
    // Identity and value rechecks detect observed churn. They do not establish
    // a lock or authority to mutate the camera or its entity.
    if(read<uintptr_t>(at)!=world || read<uintptr_t>(at+0x28)!=global || environment(world)!=global ||
       std::memcmp(before,reinterpret_cast<void*>(global),sizeof(before)) || !alive(world,entity) ||
       probe::entity_component(world,entity,0x46967561,64,chunk,row)!=view ||
       std::memcmp(values,reinterpret_cast<void*>(view),sizeof(values))) return Read::changed;
    out.world=world;out.global=global;out.entity=entity;out.selector=mode;
    std::memcpy(out.basis,values,sizeof(out.basis));
    std::memcpy(out.position,values+9,sizeof(out.position));
    std::memcpy(out.lens,values+12,sizeof(out.lens));
    return Read::ok;
}
}
Read inspect(uintptr_t world,uintptr_t expected,Snapshot& result) noexcept {
    result={};Read status{};
    __try {status=inspect_inner(world,expected,result);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {status=Read::memory;}
    if(status!=Read::ok) result={};
    return status;
}
Read selected(void* context,Selected& result) noexcept {
    result={};Read status{};
    __try {status=selected_inner(context,result);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {status=Read::memory;}
    if(status!=Read::ok) result={};
    return status;
}
}
