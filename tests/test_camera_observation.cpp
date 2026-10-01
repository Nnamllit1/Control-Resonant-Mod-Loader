#include "diagnostics/camera_observation.h"
#include "camera_service.h"
#include "camera_update_hook.h"
#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace crml::camera;
template<class B,class T> void put(B& b,size_t at,T value) {std::memcpy(b.data()+at,&value,sizeof(value));}
void require(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
int main() {
    try {
        std::vector<unsigned char> world(0x60000),chunk(0x400);
        std::array<unsigned char,80> global{};
        std::array<uint32_t,1> env_hashes{0xfe90f7f8};
        std::array<uintptr_t,1> env_values{reinterpret_cast<uintptr_t>(global.data())};
        std::array<uint64_t,2> generations{0,7},locations{UINT64_MAX,0},metadata{0,1};
        std::array<uint32_t,2> hashes{0x01a3288e,0x46967561},offsets{0x200,0x100};
        const uint64_t handle=(uint64_t(7)<<32)|1;
        const auto w=reinterpret_cast<uintptr_t>(world.data()),g=reinterpret_cast<uintptr_t>(global.data());
        put(world,0x585b0,reinterpret_cast<uintptr_t>(env_hashes.data()));put(world,0x585b8,uint32_t(1));
        put(world,0x585c0,reinterpret_cast<uintptr_t>(env_values.data()));
        put(world,0x58510,uint64_t(2));put(world,0x584e8,reinterpret_cast<uintptr_t>(generations.data()));
        put(world,0x58530,reinterpret_cast<uintptr_t>(locations.data()));put(world,0x58478,reinterpret_cast<uintptr_t>(metadata.data()));
        put(world,0x50,reinterpret_cast<uintptr_t>(chunk.data()));put(world,0x10448,uint32_t(1));
        put(chunk,0x10,handle);
        put(world,0xc22*32+0x24,uint32_t(2));put(world,0xc22*32+0x10,reinterpret_cast<uintptr_t>(hashes.data()));
        put(world,0x18458,reinterpret_cast<uintptr_t>(offsets.data()));
        put(chunk,0x100,1.f);put(chunk,0x110,1.f);put(chunk,0x120,1.f);
        put(chunk,0x130,1.f);put(chunk,0x134,1.777f);
        put(global,0,int32_t(1));for(unsigned i=0;i<4;++i) put(global,4+i*8,handle);
        const auto saved_world=world,saved_chunk=chunk;const auto saved_global=global;
        std::array<uintptr_t,6> context{};context[0]=w;context[5]=g;
        Selected selected_view{};
        require(selected(context.data(),selected_view)==Read::ok,"selected camera read");
        require(selected_view.world==w && selected_view.global==g && selected_view.entity==handle && selected_view.selector==1,"selected identity authenticated");
        require(selected_view.basis[0]==1 && selected_view.basis[4]==1 && selected_view.basis[8]==1 && selected_view.lens[0]==1,"selected copied basis and lens");
        context[5]=0;require(selected(context.data(),selected_view)==Read::environment && !selected_view.world,"missing context environment never falls back");context[5]=g;
        require(selected(reinterpret_cast<void*>(1),selected_view)==Read::memory && !selected_view.entity,"invalid callback context guarded");
        SnapshotCache cache;crml_camera_state state{};
        require(cache.read(state,100)==0 && !state.version,"empty cache");
        const auto sample=[&](uint64_t at) {cache.begin();cache.finish(context.data(),at);};
        sample(100);
        require(cache.read(state,100)==1 && state.version==1 && state.age_ms==0 && state.mode==1,"fresh snapshot");
        require(state.flags==(CRML_CAMERA_STATE_POSE|CRML_CAMERA_STATE_LENS) && state.horizontal_fov_radians==1 && state.aspect_ratio==1.777f,"bounded horizontal-radian lens snapshot");
        const auto generation=state.generation;
        require(generation && generation!=handle && generation!=w && generation!=g,"opaque generation not engine identity");
        put(chunk,0x124,4.f);sample(101);
        require(cache.read(state,101)==1 && state.generation==generation && state.position[0]==4,"pose change preserves identity generation");
        require(cache.read(state,601)==1 && state.age_ms==500,"maximum snapshot age");
        state.version=99;require(cache.read(state,602)==0 && !state.version && !state.generation && !state.flags,"stale cache clears output");
        require(cache.read(state,100)==0 && !state.version,"clock inversion rejected");
        cache.begin();require(cache.read(state,101)==0 && !state.version,"in-flight update invalidates previous sample");cache.finish(context.data(),102);
        cache.begin();cache.begin();cache.finish(context.data(),103);
        require(cache.read(state,103)==0 && !state.flags,"overlapping update cannot publish first return");
        cache.finish(context.data(),104);require(cache.read(state,104)==1,"last overlapping return can recover");
        context[5]=0;sample(105);
        require(cache.read(state,105)==0 && !state.generation,"invalid callback clears previous sample");context[5]=g;sample(106);
        require(cache.read(state,106)==1 && state.generation!=generation,"readable recovery has new generation");
        auto recovered=state.generation;put(global,0,int32_t(0));sample(107);
        require(cache.read(state,107)==1 && state.mode==0 && state.generation!=recovered,"selector transition changes generation even for same entity");
        put(chunk,0x130,std::numeric_limits<float>::infinity());sample(108);
        require(cache.read(state,108)==1 && state.flags==CRML_CAMERA_STATE_POSE && state.horizontal_fov_radians==0 && state.aspect_ratio==0,"bad lens omitted without fabricating pose failure");
        put(chunk,0x130,4.f);sample(109);
        require(cache.read(state,109)==1 && !(state.flags&CRML_CAMERA_STATE_LENS),"out-of-range FOV omitted");
        chunk=saved_chunk;global=saved_global;sample(110);world.assign(world.size(),0);
        require(cache.read(state,111)==1 && state.age_ms==1,"worker reads copied data without following engine memory");world=saved_world;
        generations[1]=9;sample(112);
        require(cache.read(state,112)==0 && !state.flags,"removed or recycled selected entity invalidates cache");generations[1]=7;
        Service unavailable;state.version=99;
        require(unavailable.camera_read(state)==-1 && !state.version && !unavailable.capabilities(),"unstarted service clears output and grants no capability");
#ifdef CRML_CAMERA_SERVICE_TESTING
        SnapshotCache exhausted;exhausted.test_generation_limit();exhausted.begin();exhausted.finish(context.data(),100);
        require(exhausted.read(state,100)==0 && !state.generation,"generation exhaustion fails closed");
        exhausted.begin();exhausted.finish(context.data(),101);
        require(exhausted.read(state,101)==0,"exhausted generations do not recycle");
        require(crml::camera_update::test_dispatch(),"shared camera hook forwards once and propagates unwind");
#endif
        Snapshot s{};require(inspect(w,g,s)==Read::ok,"valid camera snapshot");
        require(s.selector==1 && s.world==w && s.global==g,"copied authenticated context");
        require(s.slots[0].flags==(present|live|view|free_transform),"generation and both components verified");
        require(s.slots[0].view_digest && s.slots[0].free_digest,"pose change digests");
        require(world==saved_world && chunk==saved_chunk && global==saved_global,"read-only snapshot");
        const auto first=s.slots[0];put(chunk,0x124,3.f);
        require(inspect(w,g,s)==Read::ok && s.slots[0].view_digest!=first.view_digest && s.slots[0].free_digest==first.free_digest,"view/source changes distinguished");
        chunk=saved_chunk;put(chunk,0x250,std::numeric_limits<float>::infinity());
        require(inspect(w,g,s)==Read::ok && (s.slots[0].flags&invalid_pose) && !(s.slots[0].flags&free_transform),"nonfinite free pose rejected");
        chunk=saved_chunk;put(chunk,0x100,0.f);
        require(inspect(w,g,s)==Read::ok && !(s.slots[0].flags&view) && (s.slots[0].flags&invalid_pose),"degenerate view basis rejected");
        chunk=saved_chunk;put(global,4,UINT64_MAX);
        require(inspect(w,g,s)==Read::ok && s.slots[0].flags==0 && s.slots[0].entity==0,"absent slot distinguished");
        put(global,4,uint64_t(0));
        require(inspect(w,g,s)==Read::ok && !s.slots[0].flags && !s.slots[0].entity,"uninitialized slot is absent");
        global=saved_global;generations[1]=9;
        require(inspect(w,g,s)==Read::ok && s.slots[0].flags==present && !s.slots[0].view_digest,"recycled generation not followed");
        generations[1]=7;locations[1]=UINT64_MAX;
        require(inspect(w,g,s)==Read::ok && s.slots[0].flags==present,"removed entity location rejected");locations[1]=0;
        put(chunk,0x10,handle+1);
        require(inspect(w,g,s)==Read::ok && s.slots[0].flags==present,"chunk row identity mismatch");chunk=saved_chunk;
        hashes[0]=0x1234;
        require(inspect(w,g,s)==Read::ok && (s.slots[0].flags&view) && !(s.slots[0].flags&free_transform),"ordinary camera without free transform");hashes[0]=0x01a3288e;
        for(int32_t selector:{-1,4,INT32_MAX}) {
            put(global,0,selector);require(inspect(w,g,s)==Read::selector && !s.global && s.selector==-1,"invalid selector clears partial data");
        }
        global=saved_global;
        require(inspect(w,g+8,s)==Read::environment && !s.world,"foreign callback global rejected");
        env_hashes[0]=0;require(inspect(w,0,s)==Read::environment,"missing camera environment");env_hashes[0]=0xfe90f7f8;
        std::array<uint32_t,2> duplicate_hashes{0xfe90f7f8,0xfe90f7f8};
        std::array<uintptr_t,2> duplicate_values{0,g};
        put(world,0x585b0,reinterpret_cast<uintptr_t>(duplicate_hashes.data()));put(world,0x585b8,uint32_t(2));
        put(world,0x585c0,reinterpret_cast<uintptr_t>(duplicate_values.data()));
        require(inspect(w,0,s)==Read::environment && selected(context.data(),selected_view)==Read::environment,"duplicate environment rejected even when first value is null");world=saved_world;
        put(world,0x585b8,uint32_t(16385));require(inspect(w,0,s)==Read::environment,"bounded environment enumeration");world=saved_world;
        require(inspect(0,0,s)==Read::arguments && !s.global,"null world");
        require(inspect(1,0,s)==Read::memory && !s.global,"invalid world guarded");
        SYSTEM_INFO info{};GetSystemInfo(&info);
        auto pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,info.dwPageSize*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        require(pages!=nullptr,"guard fixture");DWORD old{};
        require(VirtualProtect(pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&old)!=0,"guard page");
        env_values[0]=reinterpret_cast<uintptr_t>(pages+info.dwPageSize-16);
        const auto status=inspect(w,0,s);VirtualFree(pages,0,MEM_RELEASE);
        require(status==Read::memory && !s.global && !s.slots[0].flags,"partial header read discarded");
        std::cout<<"camera observation tests passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
