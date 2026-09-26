#include "engine_observer.h"
#include "physics_observation.h"
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>
#include <limits>
using namespace crml::observer;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class T,size_t N> void put(std::array<unsigned char,N>& bytes,size_t offset,T value) {
    std::memcpy(bytes.data()+offset,&value,sizeof(value));
}
unsigned getter_mode{},linear_calls{},angular_calls{};
float fixture_damping(uintptr_t actor,size_t offset) {
    const auto sim=*reinterpret_cast<uintptr_t*>(actor+0x50);
    if(sim && (*reinterpret_cast<uint8_t*>(actor+0x7c)&1))
        return *reinterpret_cast<float*>(*reinterpret_cast<uintptr_t*>(sim+0xc0)+offset);
    return *reinterpret_cast<float*>(actor+0xc8+offset-0x30);
}
float fixture_linear(uintptr_t actor) {
    ++linear_calls;
    if(getter_mode==1) return 20.0f;
    if(getter_mode==2) return std::numeric_limits<float>::quiet_NaN();
    if(getter_mode==3) *reinterpret_cast<uint64_t*>(actor+0x10)=UINT64_MAX;
    if(getter_mode==4) RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    return fixture_damping(actor,0x30);
}
float fixture_angular(uintptr_t actor) { ++angular_calls; return fixture_damping(actor,0x34); }
void test_body_observation() {
    std::array<unsigned char,0x320> owner{};
    std::array<unsigned char,0x100> actor{},sim{};
    std::array<unsigned char,0x40> state{};
    std::array<uint64_t,2> handles{UINT64_MAX,(uint64_t(7)<<32)|1};
    std::array<uintptr_t,2> actors{0,reinterpret_cast<uintptr_t>(actor.data())};
    std::array<unsigned char,0x148> vtable_bytes{};
    const auto vtable=reinterpret_cast<uintptr_t>(vtable_bytes.data());
    put(vtable_bytes,0x130,reinterpret_cast<uintptr_t>(&fixture_linear));
    put(vtable_bytes,0x140,reinterpret_cast<uintptr_t>(&fixture_angular));
    const DampingAccessors accessors{vtable,&fixture_linear,&fixture_angular};
    put(owner,0x2d0,reinterpret_cast<uintptr_t>(handles.data())); put(owner,0x2d8,2u); put(owner,0x2dc,2u);
    put(owner,0x1d0,reinterpret_cast<uintptr_t>(actors.data())); put(owner,0x1d8,2u); put(owner,0x1dc,2u);
    put(actor,0,vtable); put(actor,8,uint16_t(7)); put(actor,0x10,(uint64_t(7)<<32)|3);
    put(actor,0xc8,0.25f); put(actor,0xcc,0.5f);
    const auto owner_at=reinterpret_cast<uintptr_t>(owner.data());
    BodySnapshot result{};
    auto expect=[&](BodyRead reason) { require(read_body(owner_at,1,vtable,result)==reason,"body read rejection");
        if(reason!=BodyRead::ok) require(!result.actor && !result.handle,"failed body reads clear output"); };
    const auto before=actor; expect(BodyRead::ok);
    require(result.handle==handles[1] && result.linear_damping==.25f && result.angular_damping==.5f && !result.alternate,"normal damping snapshot");
    require(actor==before && body_slot_count(owner_at)==2,"read-only body snapshot and bounded count");
    DampingReadback values{};
    auto access=[&](AccessRead expected) {
        require(read_damping_accessors(owner_at,result,accessors,values)==expected,"getter comparison result");
        if(expected!=AccessRead::ok && expected!=AccessRead::mismatch)
            require(!values.linear && !values.angular && !values.alternate,"failed getter clears output");
    };
    access(AccessRead::ok);
    require(values.linear==.25f && values.angular==.5f && actor==before,"read-only native accessor agreement");
    getter_mode=1; access(AccessRead::mismatch); require(values.linear==20.0f,"retain mismatching accessor value");
    getter_mode=2; access(AccessRead::scalar);
    getter_mode=3; auto angular_before=angular_calls; access(AccessRead::changed);
    require(angular_before==angular_calls,"skip second getter after actor invalidation");
    put(actor,0x10,(uint64_t(7)<<32)|3);
    getter_mode=4; access(AccessRead::memory); getter_mode=0;
    put(vtable_bytes,0x130,uintptr_t(1)); const auto linear_before=linear_calls; access(AccessRead::slot);
    require(linear_before==linear_calls,"unexpected virtual target is never called");
    put(vtable_bytes,0x130,reinterpret_cast<uintptr_t>(&fixture_linear));
    put(actor,8,uint16_t(6)); access(AccessRead::snapshot); put(actor,8,uint16_t(7));
    put(actor,0xc8,.75f); access(AccessRead::changed); put(actor,0xc8,.25f);
    require(read_damping_accessors(0,result,accessors,values)==AccessRead::arguments,"getter missing owner rejected");
    handles[1]=(uint64_t(9)<<32)|1; expect(BodyRead::actor_identity); // Recycled handle, old actor.
    handles[1]=(uint64_t(7)<<32)|0; expect(BodyRead::generation); // Free-list link, not this index.
    handles[1]=(uint64_t(7)<<32)|1;
    actors[1]=0; expect(BodyRead::missing_actor); actors[1]=reinterpret_cast<uintptr_t>(actor.data());
    put(actor,8,uint16_t(6)); expect(BodyRead::actor_type); put(actor,8,uint16_t(7));
    put(actor,0,vtable+8); expect(BodyRead::actor_type); put(actor,0,vtable);
    put(actor,0x10,UINT64_MAX); expect(BodyRead::actor_identity); put(actor,0x10,(uint64_t(7)<<32)|3);
    put(actor,0xc8,std::numeric_limits<float>::quiet_NaN()); expect(BodyRead::scalar);
    put(actor,0xc8,-1.0f); expect(BodyRead::scalar); put(actor,0xc8,.25f);
    put(owner,0x1d8,1u); expect(BodyRead::bounds); put(owner,0x1d8,2u);
    put(owner,0x2dc,1u); expect(BodyRead::bounds); put(owner,0x2dc,2u);
    put(owner,0x2dc,0xffffffffu); require(!body_slot_count(owner_at),"oversized capacity rejected"); put(owner,0x2dc,2u);
    put(actor,0x50,reinterpret_cast<uintptr_t>(sim.data())); put(actor,0x7c,uint8_t(1));
    expect(BodyRead::representation);
    put(sim,0xc0,reinterpret_cast<uintptr_t>(state.data()));
    put(state,0x30,3.0f); put(state,0x34,4.0f);
    expect(BodyRead::representation); // Tag 0 at this address means forces, not damping.
    put(state,0x1f,uint8_t(1)); expect(BodyRead::ok);
    require(result.alternate && result.linear_damping==3 && result.angular_damping==4,"tagged damping storage");
    access(AccessRead::ok);
    require(values.alternate && values.linear==3 && values.angular==4,"alternate getter agreement");
    require(read_body(1,1,vtable,result)==BodyRead::memory && !result.actor,"invalid owner address");
    actors[1]=1; expect(BodyRead::memory);
    require(read_body(0,1,vtable,result)==BodyRead::arguments,"missing owner rejected");
}
void test_body_entity() {
    auto world=std::make_unique<std::array<unsigned char,0x58600>>();
    std::array<unsigned char,0x320> owner{};
    std::array<unsigned char,0xe0> records{};
    std::array<unsigned char,0xb0> instance{};
    std::array<unsigned char,0x20> actor{},associations{};
    std::array<unsigned char,0x120> chunk{};
    std::array<uint64_t,2> handles{UINT64_MAX,(uint64_t(7)<<32)|1},pairs{UINT64_MAX,1};
    std::array<uintptr_t,2> actors{0,reinterpret_cast<uintptr_t>(actor.data())};
    std::array<uint64_t,1> local_handles{handles[1]};
    const uint64_t entity=(uint64_t(11)<<32)|2,identity=(uint64_t(7)<<32)|3;
    const auto owner_at=reinterpret_cast<uintptr_t>(owner.data()),world_at=reinterpret_cast<uintptr_t>(world->data());
    const auto instance_at=reinterpret_cast<uintptr_t>(instance.data());
    std::array<uint32_t,3> global_hashes{1,0x2eb2d62c,0xffffffff};
    uintptr_t payload=owner_at;
    std::array<uintptr_t,3> global_values{0,reinterpret_cast<uintptr_t>(&payload),0};
    put(*world,0x585b0,reinterpret_cast<uintptr_t>(global_hashes.data())); put(*world,0x585b8,3u);
    put(*world,0x585c0,reinterpret_cast<uintptr_t>(global_values.data()));
    put(owner,0x2e8,owner_at);
    auto setup_table=[&](size_t offset,uintptr_t pointer,uint32_t count) {
        put(owner,offset,pointer); put(owner,offset+8,count); put(owner,offset+12,count);
    };
    setup_table(0x2d0,reinterpret_cast<uintptr_t>(handles.data()),2);
    setup_table(0x1d0,reinterpret_cast<uintptr_t>(actors.data()),2);
    setup_table(0x2f0,reinterpret_cast<uintptr_t>(pairs.data()),2);
    setup_table(0xc8,reinterpret_cast<uintptr_t>(records.data()),2);
    setup_table(0x300,reinterpret_cast<uintptr_t>(associations.data()),2);
    put(records,0x70+0x20,instance_at); put(records,0x70+0x30,entity);
    put(records,0x70+0x50,1u); put(records,0x70+0x60,uint8_t(1));
    put(instance,0x90,reinterpret_cast<uintptr_t>(local_handles.data())); put(instance,0xa8,uint16_t(1));
    put(associations,16,instance_at); put(associations,24,0u); put(actor,0x10,identity);
    std::array<uint64_t,3> generations{0,0,11},locations{0,0,1};
    std::array<uint64_t,2> meta{0,2};
    uint32_t hash=0x6ebfd07c,offset=0x100;
    put(*world,0x58510,uint64_t(3)); put(*world,0x584e8,reinterpret_cast<uintptr_t>(generations.data()));
    put(*world,0x58530,reinterpret_cast<uintptr_t>(locations.data())); put(*world,0x58478,reinterpret_cast<uintptr_t>(meta.data()));
    put(*world,0x58,reinterpret_cast<uintptr_t>(chunk.data())); put(*world,0x1044c,1u);
    put(*world,(1+0xc22)*32+0x10,reinterpret_cast<uintptr_t>(&hash)); put(*world,(1+0xc22)*32+0x24,1u);
    put(*world,0x18478,reinterpret_cast<uintptr_t>(&offset)); put(chunk,0x10,entity); put(chunk,0x108,1u);
    BodySnapshot body{handles[1],identity,actors[1],.25f,.5f,false}; BodyEntitySnapshot out{};
    auto expect=[&](LinkRead reason) {
        require(read_body_entity(world_at,owner_at,body,out)==reason,"body/entity association result");
        if(reason!=LinkRead::ok) require(!out.entity && !out.scene_slot && !out.local_index,"failed association clears output");
    };
    const auto before=*world; const auto records_before=records; const auto chunk_before=chunk;
    require(world_scene(world_at)==owner_at,"world scene exact global lookup"); expect(LinkRead::ok);
    require(out.entity==entity && out.scene_slot==1 && out.local_index==0,"round-trip body ECS association");
    require(*world==before && records==records_before && chunk==chunk_before,"association is read-only");
    payload=owner_at+1; expect(LinkRead::world); payload=owner_at;
    global_hashes[1]++; require(!world_scene(world_at),"missing global rejected"); global_hashes[1]--;
    put(*world,0x585b8,65537u); require(!world_scene(world_at),"unbounded globals rejected"); put(*world,0x585b8,3u);
    put(owner,0x2fc,1u); expect(LinkRead::bounds); put(owner,0x2fc,2u);
    pairs[1]=UINT64_MAX; expect(LinkRead::association); pairs[1]=1;
    put(records,0xd0,uint8_t(0)); expect(LinkRead::scene); put(records,0xd0,uint8_t(1));
    put(associations,24,1u); expect(LinkRead::instance); put(associations,24,0u);
    local_handles[0]+=uint64_t(2)<<32; expect(LinkRead::instance); local_handles[0]=body.handle;
    generations[2]=12; expect(LinkRead::entity); generations[2]=11;
    locations[2]=uint64_t(1)<<32|1; expect(LinkRead::entity); locations[2]=1;
    put(chunk,0x108,0u); expect(LinkRead::entity); put(chunk,0x108,1u);
    handles[1]+=uint64_t(2)<<32; expect(LinkRead::changed); handles[1]=body.handle;
    actors[1]=0; expect(LinkRead::changed); actors[1]=body.actor;
    put(records,0x90,uintptr_t(1)); expect(LinkRead::memory); put(records,0x90,instance_at);
    expect(LinkRead::ok);
    require(!world_scene(0) && !world_scene(1),"invalid world is guarded");
    require(read_body_entity(0,owner_at,body,out)==LinkRead::arguments,"missing world rejected");
}
int main() {
    try {
        require(test_hook_prologues(),"all observed prologues must be relocatable by MinHook");
        test_body_observation();
        test_body_entity();
        auto buffer=std::make_unique<Buffer>(); buffer->open();
        for(size_t i=0;i<Buffer::capacity;++i) { Event e{}; e.value=i; require(buffer->push(e),"initial buffer fill"); }
        require(!buffer->push({}) && buffer->dropped()==1,"full-buffer loss must be explicit");
        std::array<Event,128> out{}; size_t read=0;
        while(const auto size=buffer->drain(out.data(),out.size())) {
            for(size_t i=0;i<size;++i) { require(out[i].sequence==read+1 && out[i].value==read,"FIFO/order across drains"); ++read; }
        }
        require(read==Buffer::capacity,"complete drain"); buffer->close();
        require(!buffer->push({}),"closed buffer accepts no records");
        Event invalid{}; invalid.kind=Kind::count; require(!buffer->push(invalid),"invalid kind rejected");

        buffer->open(); std::atomic<unsigned> finished{}; std::vector<std::thread> writers;
        for(unsigned thread=0;thread<4;++thread) writers.emplace_back([&,thread] {
            for(unsigned i=0;i<10000;++i) { Event e{}; e.kind=Kind::resource; e.value=(uint64_t(thread)<<32)|i; buffer->push(e); }
            ++finished;
        });
        std::unordered_set<uint64_t> values; uint64_t sequence=0;
        auto drain=[&] {
            const auto size=buffer->drain(out.data(),out.size());
            for(size_t i=0;i<size;++i) {
                require(out[i].sequence==++sequence,"concurrent publication order");
                require(values.insert(out[i].value).second,"concurrent duplicate record");
            }
            return size;
        };
        while(finished.load()!=4) { drain(); std::this_thread::yield(); }
        for(auto& thread:writers) thread.join(); while(drain()) {}
        require(sequence+buffer->dropped()==40000,"all producer records accounted for");
        require(buffer->count(Kind::resource)==40000,"attempt count"); buffer->close();

        std::array<unsigned char,128> resource{};
        uint64_t id=0x123456789abcdef0ull; uint32_t refs=17,state=4;
        std::memcpy(resource.data()+8,&id,8); std::memcpy(resource.data()+0x64,&refs,4); std::memcpy(resource.data()+0x68,&state,4);
        const auto copy=resource; const auto sample=read_resource(reinterpret_cast<uintptr_t>(resource.data()));
        require(sample.readable && sample.id==id && sample.refs==17 && sample.state==4,"copied resource fields");
        require(resource==copy,"observation must not write resource memory");
        require(!read_resource(0).readable && !read_resource(1).readable,"invalid resource pointers");
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        auto pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,system.dwPageSize*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        require(pages!=nullptr,"guarded allocation"); DWORD old{};
        require(VirtualProtect(pages+system.dwPageSize,system.dwPageSize,PAGE_NOACCESS,&old)!=0,"guard page");
        const auto partial=read_resource(reinterpret_cast<uintptr_t>(pages+system.dwPageSize-0x60));
        VirtualFree(pages,0,MEM_RELEASE);
        require(!partial.readable && !partial.pointer && !partial.id,"partial read must fail as a whole");
        require(identity(0,55)==0 && identity(1234,55)==identity(1234,55) && identity(1234,55)!=identity(1234,56),"session identities");
        Event e{}; e.kind=Kind::resource; e.object=UINT64_MAX; e.entity=UINT64_MAX;
        std::ostringstream text; write_event(text,e);
        require(text.str().find("\"18446744073709551615\"")!=std::string::npos,"64-bit identities must be JSON strings");
        std::cout<<"Observer buffer concurrency, loss accounting, guarded snapshots and serialization passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
