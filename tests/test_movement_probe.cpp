#include "movement_view.h"
#include "noclip.h"
#include <cmath>
#include <Windows.h>
#include <MinHook.h>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using crml::probe::Observation;
using crml::probe::Sample;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> void put(std::vector<unsigned char>& b, size_t at, T value) { std::memcpy(b.data()+at, &value, sizeof(value)); }
uintptr_t address(std::vector<unsigned char>& b) { return reinterpret_cast<uintptr_t>(b.data()); }
struct Fixture {
    std::vector<unsigned char> world = std::vector<unsigned char>(0x58600);
    std::vector<unsigned char> registry = std::vector<unsigned char>(0x80);
    std::vector<unsigned char> tags = std::vector<unsigned char>(8);
    std::vector<unsigned char> bits = std::vector<unsigned char>(1024);
    std::vector<unsigned char> meta = std::vector<unsigned char>(16);
    std::vector<unsigned char> locations = std::vector<unsigned char>(32);
    std::vector<unsigned char> generations = std::vector<unsigned char>(32);
    std::vector<unsigned char> hashes = std::vector<unsigned char>(16);
    std::vector<unsigned char> offsets = std::vector<unsigned char>(16);
    std::vector<unsigned char> chunk = std::vector<unsigned char>(1024);
    std::vector<unsigned char> physics = std::vector<unsigned char>(64);
    std::vector<unsigned char> movement = std::vector<unsigned char>(64);
    std::array<uintptr_t,19> view{};
    uintptr_t world_pointer{};
    uint32_t row;
    explicit Fixture(uint32_t r): row(r) {
        world_pointer = address(world);
        put(world,0,address(registry)); put(registry,0x48,address(tags)); put(registry,0x50,uint32_t{4});
        put(tags,6,uint16_t{0}); bits[0]=2;
        put(world,0x58480,address(bits)); put(world,0x58478,address(meta)); put(meta,8,uint64_t{2});
        put(world,0x58510,uint64_t{4}); put(world,0x584e8,address(generations)); put(generations,16,uint32_t{7});
        put(world,0x58530,address(locations)); put(locations,16,(uint64_t{row}<<32)|1);
        put(world,0x58,address(chunk)); put(world,0x1044c,row+1);
        put(chunk,0x10+row*8,(uint64_t{7}<<32)|2);
        const auto m=(1+0xc22)*32;
        put(world,m+0x10,address(hashes)); put(world,m+0x24,uint32_t{4}); put(world,0x18478,address(offsets));
        const std::array<uint32_t,4> h={0x6cfbb2a9,0x9b382c56,0x5077c6e3,0x6da4a5ae};
        const std::array<uint32_t,4> off={0x100,0x200,0x300,0x340};
        for(int i=0;i<4;++i) { put(hashes,i*4,h[i]); put(offsets,i*4,off[i]); }
        put(chunk,0x100+row*32+16,1.25f); put(chunk,0x100+row*32+20,2.5f); put(chunk,0x100+row*32+24,-3.0f);
        put(physics,16,1.25f); put(physics,20,2.5f); put(physics,24,-3.0f);
        put(chunk,0x200+row*32,address(physics)); put(chunk,0x208+row*32,address(movement)); put(movement,0x10,address(physics));
        view[0]=address(chunk)+0x200; view[7]=address(chunk)+0x300; view[11]=address(chunk)+0x100;
        // Observed GlobalID differs from the runtime handle; it may also be absent.
        put(chunk,0x380,uint64_t{0x3b8ca7fcb370409c});
        view[13]=address(chunk)+0x380; view[17]=address(chunk); view[18]=row;
    }
    Observation inspect(Sample& sample) { return crml::probe::inspect(view.data(), &world_pointer, 3, sample); }
};

using Six = void(*)(void*,void*,void*,void*,void*,void*);
Six trampoline{};
volatile uintptr_t forwarded{};
unsigned intercepted{};
__declspec(noinline) void target(void* a,void* b,void* c,void* d,void* e,void* f) {
    forwarded = reinterpret_cast<uintptr_t>(a) + 2*reinterpret_cast<uintptr_t>(b) + 3*reinterpret_cast<uintptr_t>(c)
              + 4*reinterpret_cast<uintptr_t>(d) + 5*reinterpret_cast<uintptr_t>(e) + 6*reinterpret_cast<uintptr_t>(f);
}
void detour(void* a,void* b,void* c,void* d,void* e,void* f) { ++intercepted; trampoline(a,b,c,d,e,f); }
int main() {
    try {
        for (uint32_t row : {0u, 3u}) {
            Fixture f(row); Sample s{};
            const auto before=f.chunk;
            require(f.inspect(s)==Observation::player && s.row==row && s.position[1]==2.5f, "Valid player view rejected");
            f.view[13]=0;
            require(f.inspect(s)==Observation::player,"Optional GlobalID used as runtime identity");
            require(f.chunk==before,"Observer modified game data");
            crml::probe::Override replacement;
            const auto view_before=f.view;
            require(replacement.prepare(f.view.data(),s,{10,20,30}),"Override rejected valid view");
            float coordinates[3];
            std::memcpy(coordinates,reinterpret_cast<void*>(replacement.view[11]+row*32ull+16),12);
            require(coordinates[0]==10 && coordinates[2]==30,"Engine row indexing did not reach private transform");
            require(*reinterpret_cast<uint8_t*>(replacement.view[7]+row*2ull)==1,"Private keyframed mode missing");
            require(f.chunk==before && f.view==view_before,"Override mutated original components or arguments");
            for(size_t i=0;i<f.view.size();++i) if(i!=7 && i!=11) require(replacement.view[i]==f.view[i],"Override changed unrelated argument");
            f.bits[0]=0; require(f.inspect(s)==Observation::other_entity,"Non-player accepted"); f.bits[0]=2;
            put(f.generations,16,uint32_t{8}); require(f.inspect(s)==Observation::invalid && s.rejection==crml::probe::Rejection::generation,"Stale entity rejection reason missing"); put(f.generations,16,uint32_t{7});
            f.view[18]=16384; require(f.inspect(s)==Observation::invalid,"Out-of-range row accepted"); f.view[18]=row;
            ++f.view[11]; require(f.inspect(s)==Observation::invalid,"Mismatched transform accepted"); --f.view[11];
            put(f.chunk,0x100+row*32+16,std::numeric_limits<float>::quiet_NaN()); require(f.inspect(s)==Observation::invalid,"NaN accepted");
            put(f.chunk,0x100+row*32+16,1.25f);
            put(f.movement,0x10,uintptr_t{0}); require(f.inspect(s)==Observation::invalid,"Mismatched controller accepted");
        }
        Sample s{};
        s.entity=42; s.world=100;
        crml::probe::Flight flight;
        const auto arm=[&] { flight.reset(); flight.enabled=true; flight.owner=1; flight.entity=42; flight.world=100; flight.lease=1000; flight.last_step=1000; };
        std::array<float,3> next{};
        arm(); require(flight.step(s,100,1050,true,{1,1,1,false},next),"Valid flight rejected");
        require(std::abs(std::sqrt(next[0]*next[0]+next[1]*next[1]+next[2]*next[2])-.25f)<.001f,"Diagonal speed is not normalized");
        arm(); require(!flight.step(s,100,1501,true,{},next) && !flight.enabled,"Expired lease retained flight");
        arm(); require(!flight.step(s,100,1050,false,{},next) && !flight.enabled,"Focus loss retained flight");
        arm(); require(!flight.step(s,101,1050,true,{},next),"World replacement retained flight");
        arm(); s.entity=43; require(!flight.step(s,100,1050,true,{},next),"Player replacement retained flight"); s.entity=42;
        arm(); s.keyframed[0]=1; require(!flight.step(s,100,1050,true,{},next),"Engine keyframing retained flight"); s.keyframed[0]=0;
        arm(); require(!flight.step(s,100,1300,true,{},next),"Long frame gap retained flight");
        require(crml::probe::inspect(nullptr,nullptr,3,s)==Observation::invalid,"Null view accepted");
        auto page=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);
        require(page!=nullptr,"Guard page allocation failed");
        const auto fault=crml::probe::inspect(page,page,3,s);
        VirtualFree(page,0,MEM_RELEASE);
        require(fault==Observation::invalid && s.rejection==crml::probe::Rejection::memory,"Unreadable memory rejection reason missing");
        require(MH_Initialize()==MH_OK,"MinHook initialization failed");
        require(MH_CreateHook(reinterpret_cast<void*>(&target),reinterpret_cast<void*>(&detour),reinterpret_cast<void**>(&trampoline))==MH_OK,"Hook creation failed");
        require(MH_EnableHook(reinterpret_cast<void*>(&target))==MH_OK,"Hook activation failed");
        Six volatile invoke=&target;
        invoke(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),reinterpret_cast<void*>(3),reinterpret_cast<void*>(4),reinterpret_cast<void*>(5),reinterpret_cast<void*>(6));
        require(intercepted==1 && forwarded==91,"Hook lost register or stack arguments");
        require(MH_DisableHook(reinterpret_cast<void*>(&target))==MH_OK,"Hook disable failed");
        require(MH_Uninitialize()==MH_OK,"Hook cleanup failed");
        std::cout << "Movement guards, private overrides, flight cleanup, and six-argument trampoline passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
