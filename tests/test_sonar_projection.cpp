#include "sonar_projection.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace crml::sonar_projection;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
bool near(float a,float b){return std::abs(a-b)<.0001f;}
int main(){try {
    Snapshot s{};
    s.current.reference=s.current.plane={0,0,0,1};
    s.current.origin={100,20,200};
    s.current.near_radius=20;s.current.far_radius=80;
    s.previous=s.current;s.interpolation=1;
    std::array<float,2> uv{};float height{};
    require(valid(s),"identity snapshot valid");
    require(project(s,{110,23,200},uv,height)&&near(uv[0],.725f)&&near(uv[1],.5f)&&near(height,3),
            "identity camera keeps X and plane-local height");
    require(project(s,{100,20,200},uv,height)&&near(uv[0],.5f)&&near(uv[1],.5f),
            "origin remains centered");
    Frame frame{};frame.reference=s.current.reference;frame.plane=s.current.plane;
    frame.origin={100,20,200,0};frame.target={110,23,200,0};
    frame.near_radius=20;frame.far_radius=80;
    NativeResult native{};native.origin={110,20,200,0};native.distance=10;
    native.scale=.225f;native.uv={.725f,.5f};
    float error{};
    require(compare_native(frame,native,error)&&error<.0001f,"copied geometry matches flattened native origin");
    native.uv={.75f,.5f};
    require(!compare_native(frame,native,error),"wrong native heading is rejected");
    native.uv={.725f,.5f};native.origin[1]=23;
    require(!compare_native(frame,native,error),"unflattened native output rejected");
    constexpr float half=0.7071067811865475f;
    s.current.reference={0,half,0,half};s.previous=s.current;
    require(project(s,{110,20,200},uv,height)&&near(uv[0],.5f)&&near(uv[1],.275f),
            "quarter-turn camera rotates X ray into sonar vertical axis");
    s.previous.reference={0,0,0,1};s.interpolation=.25f;
    require(project(s,{110,20,200},uv,height)&&near(uv[0],.66875f)&&near(uv[1],.44375f),
            "facts interpolation blends previous and current native UVs");
    s.current.reference={0,0,0,1};s.current.plane={half,0,0,half};
    s.previous=s.current;s.interpolation=1;
    require(project(s,{110,20,203},uv,height)&&near(height,3)&&near(uv[0],.725f),
            "tilted movement plane keeps vertical offset out of radial distance");
    std::array<float,3> a{50,20,200},b{150,20,200};
    require(clip_segment(s,a,b)&&near(a[0],80)&&near(b[0],120),"near circle clips crossing segment");
    a={110,20,200};b=a;
    require(clip_segment(s,a,b),"zero-length in-range label remains visible");
    a={130,20,200};b=a;
    require(!clip_segment(s,a,b),"out-of-range label hidden");
    s.current.near_radius=0;
    require(!valid(s)&&!project(s,{110,20,200},uv,height),"invalid radius fails closed");
    std::cout<<"Sonar projection checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
