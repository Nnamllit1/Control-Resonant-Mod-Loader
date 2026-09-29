#include "camera_observation.h"
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
