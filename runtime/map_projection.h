#pragma once
#include <array>
#include <cmath>

namespace crml::map_projection {
// Internal copied data, not a guest ABI. The native district transform supplies
// a quaternion and translation; the reviewed projector halves world Y before
// rotation. Bounds are already in the district's authored coordinate convention.
struct District {
    std::array<float,4> rotation{};
    std::array<float,3> translation{},minimum{},maximum{};
    std::array<float,2> offset{},scale{};
};
inline std::array<float,3> rotate(const std::array<float,4>& q,const std::array<float,3>& p) noexcept {
    std::array<float,3> t{2*(q[1]*p[2]-q[2]*p[1]),2*(q[2]*p[0]-q[0]*p[2]),2*(q[0]*p[1]-q[1]*p[0])};
    return {p[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],
            p[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],
            p[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0]};
}
inline bool normalized(const District& d,const std::array<float,3>& world,std::array<float,2>& out) noexcept {
    out={};
    auto finite=[](const auto& a) {for(float f:a) if(!std::isfinite(f)) return false;return true;};
    if(!finite(d.rotation)||!finite(d.translation)||!finite(d.minimum)||!finite(d.maximum)||
       !finite(d.offset)||!finite(d.scale)||!finite(world)) return false;
    double norm{};for(float f:d.rotation)norm+=double(f)*f;
    if(norm<0.98 || norm>1.02) return false;
    auto p=rotate(d.rotation,{world[0],world[1]*0.5f,world[2]});
    const auto lo=rotate(d.rotation,d.minimum),hi=rotate(d.rotation,d.maximum);
    std::array<float,2> result{};
    for(size_t i=0;i<2;++i) {
        const float a=lo[i]+d.translation[i],b=hi[i]+d.translation[i];
        const float extent=b-a;
        // Native collapses zero extents. Do not advertise a usable projection
        // when an axis has no extent; callers must report unavailable instead.
        if(!std::isfinite(extent)||std::abs(extent)<1e-6f) return false;
        result[i]=((p[i]+d.translation[i]+d.offset[i])*d.scale[i]-a)/extent;
    }
    result[1]=1-result[1];
    if(!finite(result)) return false;
    out=result;return true; // Deliberately unclamped: a line needs geometric clipping.
}
// Invert the copied affine projection on an explicit guest-supplied plane.
// No depth picking or inferred floor: singular/edge-on planes are rejected.
inline bool on_plane(const District& d,const std::array<float,2>& uv,const std::array<float,3>& origin,const std::array<float,3>& up,std::array<float,3>& out) noexcept {
    out={};std::array<float,2> base{};
    if(!std::isfinite(uv[0])||!std::isfinite(uv[1])||!normalized(d,origin,base))return false;
    double a[3][4]{};double norm{};
    for(size_t i=0;i<3;++i){
        if(!std::isfinite(up[i]))return false;norm+=double(up[i])*up[i];
        auto p=origin;p[i]+=32;std::array<float,2> v{};if(!normalized(d,p,v))return false;
        a[0][i]=(double(v[0])-base[0])/32;a[1][i]=(double(v[1])-base[1])/32;a[2][i]=up[i];
    }
    if(norm<.98||norm>1.02)return false;
    a[0][3]=double(uv[0])-base[0];a[1][3]=double(uv[1])-base[1];
    for(size_t i=0;i<3;++i){
        size_t pivot=i;for(size_t j=i+1;j<3;++j)if(std::abs(a[j][i])>std::abs(a[pivot][i]))pivot=j;
        if(std::abs(a[pivot][i])<1e-10)return false;
        for(size_t k=0;k<4;++k)std::swap(a[i][k],a[pivot][k]);
        const double divisor=a[i][i];for(size_t k=i;k<4;++k)a[i][k]/=divisor;
        for(size_t j=0;j<3;++j)if(j!=i){const double factor=a[j][i];for(size_t k=i;k<4;++k)a[j][k]-=factor*a[i][k];}
    }
    for(size_t i=0;i<3;++i){out[i]=float(origin[i]+a[i][3]);if(!std::isfinite(out[i])||std::abs(out[i])>1e8)return false;}
    std::array<float,2> check{};return normalized(d,out,check)&&std::abs(check[0]-uv[0])<.0001f&&std::abs(check[1]-uv[1])<.0001f;
}
}
