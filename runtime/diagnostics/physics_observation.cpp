#include "diagnostics/physics_observation.h"
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
MotionRead read_body_motion(uintptr_t owner,const BodySnapshot& expected,uintptr_t vtable,MotionSnapshot& out) noexcept {
    out={};
    if(!owner || !vtable || !expected.actor) return MotionRead::arguments;
    __try {
        BodySnapshot before{},after{};
        if(snapshot(owner,uint32_t(expected.handle),vtable,before)!=BodyRead::ok || before.handle!=expected.handle
           || before.actor!=expected.actor || before.actor_identity!=expected.actor_identity) return MotionRead::target;
        const auto scene=read<uintptr_t>(before.actor+0x18);
        // Getters 0x22370 / 0x21880 reject reads when scene+0x3e9 is set
        // (except an engine-specific allowance). Conservatively reject all busy reads.
        if(!scene || read<uint8_t>(scene+0x3e9)) return MotionRead::phase;
        float linear[3]{},angular[3]{};
        for(unsigned i=0;i<3;++i) {
            linear[i]=read<float>(before.actor+0xa0+i*4);
            angular[i]=read<float>(before.actor+0xb0+i*4);
            if(!std::isfinite(linear[i]) || !std::isfinite(angular[i])) return MotionRead::scalar;
        }
        if(snapshot(owner,uint32_t(expected.handle),vtable,after)!=BodyRead::ok || before.handle!=after.handle
           || before.actor!=after.actor || before.actor_identity!=after.actor_identity
           || before.alternate!=after.alternate || read<uintptr_t>(after.actor+0x18)!=scene
           || read<uint8_t>(scene+0x3e9)) return MotionRead::changed;
        const auto l=std::hypot(linear[0],linear[1],linear[2]),a=std::hypot(angular[0],angular[1],angular[2]);
        if(!std::isfinite(l) || !std::isfinite(a)) return MotionRead::scalar;
        out={l,a};return MotionRead::ok;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return MotionRead::memory;}
}
bool read_simulation_context(uintptr_t view,uintptr_t descriptor,uintptr_t dispatcher,SimulationContext& out) noexcept {
    out={};
    if(!view || !descriptor || !dispatcher) return false;
    __try {
        if(read<uintptr_t>(descriptor+0x140)!=dispatcher) return false;
        const auto world=read<uintptr_t>(view);
        const auto environments=read<uintptr_t>(descriptor+0x58);
        if(!world || !environments) return false;
        const auto environment=read<uintptr_t>(environments);
        if(!environment) return false;
        const auto owner=read<uintptr_t>(environment);
        if(!owner || scene_inner(world)!=owner || read<uintptr_t>(owner+0x2e8)!=owner) return false;
        if(read<uintptr_t>(view)!=world || read<uintptr_t>(descriptor+0x58)!=environments
           || read<uintptr_t>(environments)!=environment || read<uintptr_t>(environment)!=owner
           || read<uintptr_t>(descriptor+0x140)!=dispatcher) return false;
        out={world,owner}; return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
LinkRead read_body_entity(uintptr_t world,uintptr_t owner,const BodySnapshot& body,BodyEntitySnapshot& out) noexcept {
    out={}; BodyEntitySnapshot copy{}; LinkRead result{};
    __try { result=entity_inner(world,owner,body,copy); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return LinkRead::memory; }
    if(result==LinkRead::ok) out=copy;
    return result;
}
namespace {
TargetRead target_inner(uintptr_t world,uint64_t entity,uint32_t local,uint64_t expected,
                        uintptr_t vtable,EntityBodySnapshot& out) noexcept {
    if(!world || !entity || entity==UINT64_MAX || !vtable || local>=65536) return TargetRead::arguments;
    const auto owner=scene_inner(world);
    if(!owner) return TargetRead::world;
    uintptr_t chunk{}; uint32_t row{};
    const auto physics=probe::entity_component(world,entity,0x6ebfd07c,16,chunk,row);
    if(!physics) return TargetRead::entity;
    const auto slot=read<uint32_t>(physics+8);
    uintptr_t records{}; uint32_t count{};
    if(!table(owner,0xc8,records,count) || slot>=count) return TargetRead::bounds;
    const auto record=records+uint64_t(slot)*0x70;
    const auto instance=read<uintptr_t>(record+0x20);
    if(!instance || !read<uint8_t>(record+0x60) || read<uint32_t>(record+0x50)!=slot
       || read<uint64_t>(record+0x30)!=entity) return TargetRead::scene;
    const auto handles=read<uintptr_t>(instance+0x90);
    const auto handle_count=read<uint16_t>(instance+0xa8);
    if(!handles || local>=handle_count) return TargetRead::instance;
    const auto handle=read<uint64_t>(handles+uint64_t(local)*8);
    if(handle==UINT64_MAX || (expected!=UINT64_MAX && handle!=expected)) return TargetRead::identity;
    BodySnapshot body{};
    if(snapshot(owner,uint32_t(handle),vtable,body)!=BodyRead::ok) return TargetRead::body;
    if(body.handle!=handle) return TargetRead::identity;
    BodyEntitySnapshot link{};
    if(entity_inner(world,owner,body,link)!=LinkRead::ok || link.entity!=entity
       || link.scene_slot!=slot || link.local_index!=local) return TargetRead::association;
    uintptr_t chunk_after{}; uint32_t row_after{};
    if(scene_inner(world)!=owner || read<uintptr_t>(owner+0xc8)!=records || read<uint32_t>(owner+0xd0)!=count
       || probe::entity_component(world,entity,0x6ebfd07c,16,chunk_after,row_after)!=physics
       || chunk_after!=chunk || row_after!=row || read<uint32_t>(physics+8)!=slot
       || read<uintptr_t>(record+0x20)!=instance || read<uint64_t>(record+0x30)!=entity
       || read<uint32_t>(record+0x50)!=slot || !read<uint8_t>(record+0x60)
       || read<uintptr_t>(instance+0x90)!=handles || read<uint16_t>(instance+0xa8)!=handle_count
       || read<uint64_t>(handles+uint64_t(local)*8)!=handle) return TargetRead::changed;
    out={owner,body,link}; return TargetRead::ok;
}
}
TargetRead read_entity_body(uintptr_t world,uint64_t entity,uint32_t local,uint64_t expected,
                           uintptr_t vtable,EntityBodySnapshot& out) noexcept {
    out={}; EntityBodySnapshot copy{}; TargetRead result{};
    __try { result=target_inner(world,entity,local,expected,vtable,copy); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return TargetRead::memory; }
    if(result==TargetRead::ok) out=copy;
    return result;
}
namespace {
bool same_body(const BodySnapshot& a,const BodySnapshot& b) noexcept {
    return a.handle==b.handle && a.actor==b.actor && a.actor_identity==b.actor_identity
        && a.alternate==b.alternate && a.linear_damping==b.linear_damping && a.angular_damping==b.angular_damping;
}
AccessRead access_inner(uintptr_t owner,const BodySnapshot& expected,const DampingAccessors& accessors,DampingReadback& out) noexcept {
    if(!owner || !expected.actor || !accessors.vtable || !accessors.linear || !accessors.angular) return AccessRead::arguments;
    BodySnapshot before{},middle{},after{};
    const auto index=uint32_t(expected.handle);
    if(snapshot(owner,index,accessors.vtable,before)!=BodyRead::ok) return AccessRead::snapshot;
    if(!same_body(expected,before)) return AccessRead::changed;
    if(read<uintptr_t>(accessors.vtable+0x130)!=reinterpret_cast<uintptr_t>(accessors.linear)
       || read<uintptr_t>(accessors.vtable+0x140)!=reinterpret_cast<uintptr_t>(accessors.angular)) return AccessRead::slot;
    const auto linear=accessors.linear(before.actor);
    // Do not call the second getter after a detected retirement or representation change.
    if(snapshot(owner,index,accessors.vtable,middle)!=BodyRead::ok || !same_body(before,middle)) return AccessRead::changed;
    const auto angular=accessors.angular(before.actor);
    if(snapshot(owner,index,accessors.vtable,after)!=BodyRead::ok || !same_body(before,after)) return AccessRead::changed;
    if(!std::isfinite(linear) || !std::isfinite(angular) || linear<0 || angular<0) return AccessRead::scalar;
    out={linear,angular,before.alternate};
    return linear==before.linear_damping && angular==before.angular_damping?AccessRead::ok:AccessRead::mismatch;
}
}
AccessRead read_damping_accessors(uintptr_t owner,const BodySnapshot& expected,const DampingAccessors& accessors,DampingReadback& out) noexcept {
    out={}; DampingReadback copy{}; AccessRead result{};
    __try { result=access_inner(owner,expected,accessors,copy); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return AccessRead::memory; }
    if(result==AccessRead::ok || result==AccessRead::mismatch) out=copy;
    return result;
}
namespace {
WriteStatus write_inner(const SimulationContext& context,const EntityBodySnapshot& selected,
                        const DampingAccessors& accessors,DampingSetter setter,
                        float expected,float value,bool& attempted) noexcept {
    if(!context.world || !context.owner || !selected.body.actor || !accessors.vtable || !setter
       || !std::isfinite(expected) || expected<0 || !std::isfinite(value) || value<0) return WriteStatus::arguments;
    if(selected.owner!=context.owner || scene_inner(context.world)!=context.owner) return WriteStatus::context;
    EntityBodySnapshot fresh{};
    if(target_inner(context.world,selected.link.entity,selected.link.local_index,
                    selected.body.handle,accessors.vtable,fresh)!=TargetRead::ok) return WriteStatus::target;
    if(fresh.owner!=context.owner || fresh.link.scene_slot!=selected.link.scene_slot
       || !same_body(selected.body,fresh.body) || fresh.body.linear_damping!=expected) return WriteStatus::changed;
    if(read<uintptr_t>(accessors.vtable+0x128)!=reinterpret_cast<uintptr_t>(setter)) return WriteStatus::slot;
    DampingReadback before{};
    if(access_inner(context.owner,fresh.body,accessors,before)!=AccessRead::ok) return WriteStatus::getter;
    const auto scene=read<uintptr_t>(fresh.body.actor+0x18);
    // Native setter 0x24c30 checks the same busy byte. Require an attached scene
    // as well, because detached objects are outside this experiment's scope.
    if(!scene || read<uint8_t>(scene+0x3ea)) return WriteStatus::scene_busy;
    EntityBodySnapshot immediate{};
    if(target_inner(context.world,selected.link.entity,selected.link.local_index,
                    selected.body.handle,accessors.vtable,immediate)!=TargetRead::ok
       || immediate.owner!=context.owner || !same_body(fresh.body,immediate.body)
       || read<uintptr_t>(immediate.body.actor+0x18)!=scene
       || read<uint8_t>(scene+0x3ea)) return WriteStatus::changed;
    attempted=true;
    setter(immediate.body.actor,value);
    EntityBodySnapshot after{};
    if(target_inner(context.world,selected.link.entity,selected.link.local_index,
                    selected.body.handle,accessors.vtable,after)!=TargetRead::ok
       || after.owner!=context.owner || after.body.actor!=immediate.body.actor
       || after.body.actor_identity!=immediate.body.actor_identity) return WriteStatus::readback;
    DampingReadback values{};
    if(access_inner(context.owner,after.body,accessors,values)!=AccessRead::ok
       || values.linear!=value || values.angular!=before.angular) return WriteStatus::readback;
    return WriteStatus::ok;
}
}
DampingWrite write_linear_damping(const SimulationContext& context,const EntityBodySnapshot& selected,
                                  const DampingAccessors& accessors,DampingSetter setter,
                                  float expected,float value) noexcept {
    DampingWrite result{};
    __try { result.status=write_inner(context,selected,accessors,setter,expected,value,result.attempted); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { result.status=WriteStatus::memory; }
    return result;
}
}
