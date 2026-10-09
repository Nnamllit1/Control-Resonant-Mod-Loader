#include "map_projection.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace crml::map_projection;
void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
bool near(float a,float b){return std::abs(a-b)<0.0001f;}
int main(){try {
    District d{};d.rotation={0,0,0,1};d.minimum={10,20,-30};d.maximum={110,220,70};d.scale={1,1};
    std::array<float,2> p{};
    require(normalized(d,{60,240,900},p)&&near(p[0],.5f)&&near(p[1],.5f),"world Y halved, V inverted, third axis omitted after rotation");
    require(normalized(d,{210,-360,0},p)&&near(p[0],2)&&near(p[1],2),"outside points are not clamped to create border trails");
    d.translation={5,7,9};d.offset={3,4};d.scale={2,3};
    require(normalized(d,{10,40,0},p)&&near(p[0],.21f)&&near(p[1],.67f),"offset and scale precede bounds normalization");
    std::array<float,3> world{};
    require(on_plane(d,p,{0,0,77},{0,0,1},world)&&near(world[0],10)&&near(world[1],40)&&near(world[2],77),"inverse map click preserves explicit plane depth through scaled projection");
    require(!on_plane(d,p,{0,0,0},{0,1,0},world),"edge-on plane cannot invent a floor pick");
    d={};d.rotation={0,0,float(std::sqrt(.5)),float(std::sqrt(.5))};d.minimum={0,0,0};d.maximum={10,20,30};d.scale={1,1};
    require(normalized(d,{5,20,99},p)&&near(p[0],.5f)&&near(p[1],.5f),"rotated district uses rotated authored bounds including negative extent");
    require(on_plane(d,p,{0,0,12},{0,0,1},world)&&near(world[0],5)&&near(world[1],20)&&near(world[2],12),"rotated projection round trips on explicit plane");
    d.maximum=d.minimum;require(!normalized(d,{0,0,0},p)&&p==std::array<float,2>{},"degenerate bounds unavailable");
    d.maximum={10,20,30};d.rotation={0,0,0,0};require(!normalized(d,{0,0,0},p),"invalid quaternion rejected");
    d.rotation={0,0,0,1};d.scale[0]=std::numeric_limits<float>::infinity();require(!normalized(d,{0,0,0},p),"nonfinite data rejected");
    std::cout<<"Map projection checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
