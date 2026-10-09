#include "navigation_view.h"
#include "compatibility.h"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace crml::navigation;
void require(bool ok,const char* text) {if(!ok) throw std::runtime_error(text);}
template<class T> void put(std::vector<unsigned char>& data,size_t at,T value) {std::memcpy(data.data()+at,&value,sizeof(value));}
uintptr_t address(std::vector<unsigned char>& data) {return reinterpret_cast<uintptr_t>(data.data());}
struct Fixture {
    std::vector<unsigned char> world=std::vector<unsigned char>(0x58600),meta=std::vector<unsigned char>(16),
        locations=std::vector<unsigned char>(24),generations=std::vector<unsigned char>(24),
        hashes=std::vector<unsigned char>(4),offsets=std::vector<unsigned char>(4),chunk=std::vector<unsigned char>(512);
    std::array<uintptr_t,19> view{};
    static constexpr uint64_t player=(uint64_t{7}<<32)|2;
    Fixture() {
        put(world,0x58510,uint64_t{3});put(world,0x584e8,address(generations));put(generations,16,uint32_t{7});
        put(world,0x58530,address(locations));put(locations,16,(uint64_t{1}<<32)|1);
        put(world,0x58478,address(meta));put(meta,8,uint64_t{2});
        put(world,0x58,address(chunk));put(world,0x1044c,uint32_t{2});put(chunk,0x18,player);
        put(world,(0xc22+1)*32+0x10,address(hashes));put(world,(0xc22+1)*32+0x24,uint32_t{1});
        put(world,0x18478,address(offsets));put(hashes,0,uint32_t{0xc0853848});put(offsets,0,uint32_t{0x100});
        put(chunk,0x11c,1.f);
        view[10]=address(chunk)+0x100;view[17]=address(chunk);view[18]=1;
    }
    GroundStatus read(GroundObservation& out) {return inspect_ground(view.data(),address(world),player,out);}
};
void expected(const float (&q)[4],float x,float y,float z) {
    float up[3]{};require(ground_up(q,up),"valid rotation rejected");
    require(std::abs(up[0]-x)<0.00001f && std::abs(up[1]-y)<0.00001f && std::abs(up[2]-z)<0.00001f,"incorrect rotated ground direction");
}
int main() {
    try {
        const float half=std::sqrt(0.5f);
        expected({0,0,0,1},0,1,0);expected({half,0,0,half},0,0,1);
        expected({0,0,half,half},-1,0,0);expected({1,0,0,0},0,-1,0);
        expected({0,half,0,half},0,1,0);expected({0,0,-half,-half},-1,0,0);
        expected({0,0,0,1.001f},0,1,0);
        float out[3]{1,2,3};
        for(const auto& bad:std::array<std::array<float,4>,4>{{{0,0,0,0},{0,0,0,2},{NAN,0,0,1},{0,INFINITY,0,1}}}) {
            float q[4];std::memcpy(q,bad.data(),sizeof(q));require(!ground_up(q,out),"malformed rotation accepted");
            require(out[0]==0 && out[1]==0 && out[2]==0,"failed decode leaked previous up");
        }
        Fixture f;GroundObservation observed{};
        using namespace crml::compatibility;
        reviewed_build=true;engine_profile=EngineProfile::previous;
        require(f.read(observed)==GroundStatus::unavailable,"previous build accepted without review");
        engine_profile=EngineProfile::october_update;
        require(f.read(observed)==GroundStatus::unavailable,"earlier update accepted without review");
        engine_profile=EngineProfile::october_patch;reviewed_build=false;
        require(f.read(observed)==GroundStatus::unavailable,"unreviewed executable accepted");
        reviewed_build=true;
        require(f.read(observed)==GroundStatus::valid && observed.up[1]==1,"row stride/component decode failed");
        put(f.chunk,0x118,half);put(f.chunk,0x11c,half);
        require(f.read(observed)==GroundStatus::valid && std::abs(observed.up[0]+1)<0.00001f,"changing ground did not update");
        ++f.view[10];require(f.read(observed)==GroundStatus::unavailable,"unrelated view accepted");--f.view[10];
        ++f.view[18];require(f.read(observed)==GroundStatus::unavailable,"wrong view row accepted");--f.view[18];
        ++f.view[17];require(f.read(observed)==GroundStatus::unavailable,"wrong view chunk accepted");--f.view[17];
        put(f.generations,16,uint32_t{8});require(f.read(observed)==GroundStatus::unavailable,"stale entity accepted");
        require(observed.up[0]==0 && observed.rotation[3]==0,"failed identity leaked previous observation");
        put(f.generations,16,uint32_t{7});put(f.hashes,0,uint32_t{1});require(f.read(observed)==GroundStatus::unavailable,"missing plane accepted");
        put(f.hashes,0,uint32_t{0xc0853848});put(f.chunk,0x110,NAN);require(f.read(observed)==GroundStatus::quaternion,"invalid plane accepted");
        put(f.chunk,0x110,0.f);
        require(inspect_ground(reinterpret_cast<void*>(1),address(f.world),Fixture::player,observed)==GroundStatus::memory,"unreadable view accepted");
        require(observed.up[0]==0 && observed.rotation[3]==0,"memory fault leaked copied data");
        require(inspect_ground(f.view.data(),1,Fixture::player,observed)==GroundStatus::unavailable,"unreadable world accepted");
        std::cout<<"Navigation ground observation checks passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
