#include "physics_observation.h"
#include "movement_view.h"
#include <Windows.h>
#include <cmath>

namespace crml::observer {
namespace {
constexpr uint32_t max_slots=1u<<20; // Diagnostic bound, not an engine capacity claim.
template<class T> T read(uintptr_t at) noexcept { return *reinterpret_cast<const volatile T*>(at); }
bool table(uintptr_t owner, size_t offset, uintptr_t& pointer, uint32_t& count) noexcept {
    pointer=read<uintptr_t>(owner+offset); count=read<uint32_t>(owner+offset+8);
    const auto capacity=read<uint32_t>(owner+offset+12);
    return pointer && count<=capacity && capacity<=max_slots;
}
uintptr_t scene_inner(uintptr_t world) noexcept {
    if(!world) return 0;
    const auto count=read<uint32_t>(world+0x585b8);
    const auto hashes=read<uintptr_t>(world+0x585b0),values=read<uintptr_t>(world+0x585c0);
    if(!hashes || !values || !count || count>65536) return 0;
    uint32_t lo=0,hi=count;
    while(lo<hi) { const auto mid=lo+(hi-lo)/2;
        if(read<uint32_t>(hashes+uint64_t(mid)*4)<0x2eb2d62c) lo=mid+1; else hi=mid; }
    if(lo==count || read<uint32_t>(hashes+uint64_t(lo)*4)!=0x2eb2d62c) return 0;
    const auto payload=read<uintptr_t>(values+uint64_t(lo)*8);
    if(!payload) return 0;
    const auto scene=read<uintptr_t>(payload);
    if(read<uint32_t>(world+0x585b8)!=count || read<uintptr_t>(world+0x585b0)!=hashes
       || read<uintptr_t>(world+0x585c0)!=values || read<uintptr_t>(values+uint64_t(lo)*8)!=payload) return 0;
    return scene;
}
LinkRead entity_inner(uintptr_t world,uintptr_t owner,const BodySnapshot& body,BodyEntitySnapshot& out) noexcept {
    if(!world || !owner || !body.actor) return LinkRead::arguments;
    if(scene_inner(world)!=owner || read<uintptr_t>(owner+0x2e8)!=owner) return LinkRead::world;
    const auto index=uint32_t(body.handle);
    uintptr_t handles{},actors{},pairs{},records{},associations{};
    uint32_t nh{},na{},np{},nr{},ni{};
    if(!table(owner,0x2d0,handles,nh) || !table(owner,0x1d0,actors,na)
       || !table(owner,0x2f0,pairs,np) || !table(owner,0xc8,records,nr)
       || !table(owner,0x300,associations,ni) || index>=nh || index>=na || index>=np || index>=ni) return LinkRead::bounds;
    const auto pair=read<uint64_t>(pairs+uint64_t(index)*8);
    const auto slot=uint32_t(pair),local=uint32_t(pair>>32);
    if(slot>=nr) return LinkRead::association;
    const auto record=records+uint64_t(slot)*0x70;
    const auto instance=read<uintptr_t>(record+0x20);
    const auto entity=read<uint64_t>(record+0x30);
    if(!read<uint8_t>(record+0x60) || read<uint32_t>(record+0x50)!=slot || !instance) return LinkRead::scene;
    const auto assoc=associations+uint64_t(index)*16;
    const auto local_count=read<uint16_t>(instance+0xa8);
    const auto local_handles=read<uintptr_t>(instance+0x90);
    if(read<uintptr_t>(assoc)!=instance || read<uint32_t>(assoc+8)!=local
       || !local_handles || local>=local_count || read<uint64_t>(local_handles+uint64_t(local)*8)!=body.handle) return LinkRead::instance;
    uintptr_t chunk{}; uint32_t row{};
    const auto physics=probe::entity_component(world,entity,0x6ebfd07c,16,chunk,row);
    if(!physics || read<uint32_t>(physics+8)!=slot) return LinkRead::entity;
    uintptr_t chunk_after{}; uint32_t row_after{};
    // Re-resolve both ends; this detects some races but does not exclude ABA or acquire ownership.
    if(scene_inner(world)!=owner || read<uintptr_t>(owner+0x2e8)!=owner
       || read<uintptr_t>(owner+0x2d0)!=handles || read<uint32_t>(owner+0x2d8)!=nh
       || read<uintptr_t>(owner+0x1d0)!=actors || read<uint32_t>(owner+0x1d8)!=na
       || read<uintptr_t>(owner+0x2f0)!=pairs || read<uint32_t>(owner+0x2f8)!=np
       || read<uintptr_t>(owner+0xc8)!=records || read<uint32_t>(owner+0xd0)!=nr
       || read<uintptr_t>(owner+0x300)!=associations || read<uint32_t>(owner+0x308)!=ni
       || read<uint64_t>(handles+uint64_t(index)*8)!=body.handle
       || read<uintptr_t>(actors+uint64_t(index)*8)!=body.actor
       || read<uint64_t>(body.actor+0x10)!=body.actor_identity
       || read<uint64_t>(pairs+uint64_t(index)*8)!=pair
       || read<uintptr_t>(record+0x20)!=instance || read<uint64_t>(record+0x30)!=entity
       || !read<uint8_t>(record+0x60) || read<uint32_t>(record+0x50)!=slot
       || read<uintptr_t>(assoc)!=instance || read<uint32_t>(assoc+8)!=local
       || read<uintptr_t>(instance+0x90)!=local_handles || read<uint16_t>(instance+0xa8)!=local_count
       || read<uint64_t>(local_handles+uint64_t(local)*8)!=body.handle
       || probe::entity_component(world,entity,0x6ebfd07c,16,chunk_after,row_after)!=physics
       || chunk_after!=chunk || row_after!=row || read<uint32_t>(physics+8)!=slot) return LinkRead::changed;
    out={entity,slot,local}; return LinkRead::ok;
}
BodyRead snapshot(uintptr_t owner,uint32_t index,uintptr_t vtable,BodySnapshot& out) noexcept {
    if(!owner || !vtable) return BodyRead::arguments;
    uintptr_t handles{},actors{}; uint32_t handle_count{},actor_count{};
    if(!table(owner,0x2d0,handles,handle_count) || !table(owner,0x1d0,actors,actor_count)
       || index>=handle_count || index>=actor_count) return BodyRead::bounds;
    const auto handle=read<uint64_t>(handles+uint64_t(index)*8);
    if(uint32_t(handle)!=index) return BodyRead::generation;
    const auto actor=read<uintptr_t>(actors+uint64_t(index)*8);
    if(!actor) return BodyRead::missing_actor;
    if(read<uintptr_t>(actor)!=vtable || read<uint16_t>(actor+8)!=7) return BodyRead::actor_type;
    const auto identity=read<uint64_t>(actor+0x10);
    // 0x2d83c60 encodes (index*2)|1 in the low word, preserving the upper word.
    if(uint32_t(identity)==UINT32_MAX || (uint32_t(identity)>>1)!=index
       || uint32_t(identity>>32)!=uint32_t(handle>>32)) return BodyRead::actor_identity;
    const auto sim=read<uintptr_t>(actor+0x50);
    const auto flags=read<uint8_t>(actor+0x7c);
    uintptr_t state{}; const bool alternate=sim && (flags&1);
    uintptr_t values=actor+0xc8;
    if(alternate) {
        state=read<uintptr_t>(sim+0xc0);
        if(!state || read<uint8_t>(state+0x1f)!=1) return BodyRead::representation;
        values=state+0x30;
    }
    const float linear=read<float>(values),angular=read<float>(values+4);
    if(!std::isfinite(linear) || !std::isfinite(angular) || linear<0 || angular<0) return BodyRead::scalar;
    // Detect some races without claiming an atomic or lifetime-protected snapshot.
    if(read<uintptr_t>(owner+0x2d0)!=handles || read<uintptr_t>(owner+0x1d0)!=actors
       || read<uint32_t>(owner+0x2d8)!=handle_count || read<uint32_t>(owner+0x1d8)!=actor_count
       || read<uint64_t>(handles+uint64_t(index)*8)!=handle
       || read<uintptr_t>(actors+uint64_t(index)*8)!=actor
       || read<uintptr_t>(actor)!=vtable || read<uint64_t>(actor+0x10)!=identity
       || read<uintptr_t>(actor+0x50)!=sim || read<uint8_t>(actor+0x7c)!=flags
       || (alternate && (read<uintptr_t>(sim+0xc0)!=state || read<uint8_t>(state+0x1f)!=1))) return BodyRead::changed;
    out={handle,identity,actor,linear,angular,alternate}; return BodyRead::ok;
}
}
uint32_t body_slot_count(uintptr_t owner) noexcept {
    if(!owner) return 0;
    __try {
        uintptr_t pointer{}; uint32_t count{};
        return table(owner,0x2d0,pointer,count)?count:0;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return 0; }
}
BodyRead read_body(uintptr_t owner,uint32_t index,uintptr_t vtable,BodySnapshot& out) noexcept {
    out={}; BodySnapshot copy{}; BodyRead result{};
    __try { result=snapshot(owner,index,vtable,copy); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return BodyRead::memory; }
    if(result==BodyRead::ok) out=copy;
    return result;
}
uintptr_t world_scene(uintptr_t world) noexcept {
    __try { return scene_inner(world); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return 0; }
}
LinkRead read_body_entity(uintptr_t world,uintptr_t owner,const BodySnapshot& body,BodyEntitySnapshot& out) noexcept {
    out={}; BodyEntitySnapshot copy{}; LinkRead result{};
    __try { result=entity_inner(world,owner,body,copy); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return LinkRead::memory; }
    if(result==LinkRead::ok) out=copy;
    return result;
}
}
