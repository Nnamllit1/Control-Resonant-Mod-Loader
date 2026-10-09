#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace crml::sonar_projection {
// Value copies of native geometry inputs. XYZW quaternions, XYZ world vectors.
struct Frame {
    std::array<float,4> reference{},plane{},origin{},target{};
    float near_radius{},far_radius{};
    uint8_t flag{};
};
struct NativeResult {
    uint32_t radial_class{},vertical_class{};
    std::array<float,4> origin{};
    float distance{},scale{};
    std::array<float,2> uv{};
};
struct Model {
    std::array<float,4> reference{},plane{};
    std::array<float,3> origin{};
    float near_radius{},far_radius{};
};
struct Snapshot {
    Model current{},previous{};
    float interpolation{}; // previous + (current - previous)*t
    float comparison_error{};
};
inline bool finite(const auto& values) noexcept {
    for(float v:values)if(!std::isfinite(v))return false;
    return true;
}
inline std::array<float,4> conjugate(const std::array<float,4>& q) noexcept {
    return {-q[0],-q[1],-q[2],q[3]};
}
inline std::array<float,4> multiply(const std::array<float,4>& a,const std::array<float,4>& b) noexcept {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
            a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
            a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
            a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
inline std::array<float,3> rotate(const std::array<float,4>& q,const std::array<float,3>& p) noexcept {
    const std::array<float,3> t{2*(q[1]*p[2]-q[2]*p[1]),2*(q[2]*p[0]-q[0]*p[2]),2*(q[0]*p[1]-q[1]*p[0])};
    return {p[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],
            p[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],
            p[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0]};
}
inline float norm2(const std::array<float,4>& q) noexcept {
    return q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3];
}
inline bool valid(const Model& m) noexcept {
    return finite(m.reference)&&finite(m.plane)&&finite(m.origin)&&
        std::isfinite(m.near_radius)&&std::isfinite(m.far_radius)&&
        m.near_radius>0&&m.far_radius>m.near_radius&&
        norm2(m.reference)>0.98f&&norm2(m.reference)<1.02f&&
        norm2(m.plane)>0.98f&&norm2(m.plane)<1.02f;
}
inline bool valid(const Snapshot& s) noexcept {
    return valid(s.current)&&valid(s.previous)&&std::isfinite(s.interpolation)&&
        s.interpolation>=0&&s.interpolation<=1&&std::isfinite(s.comparison_error);
}
inline std::array<float,3> local_displacement(const std::array<float,4>& plane,
                                               const std::array<float,3>& origin,
                                               const std::array<float,3>& target) noexcept {
    return rotate(conjugate(plane),{target[0]-origin[0],target[1]-origin[1],target[2]-origin[2]});
}
inline float distance_scale(float d,float near_radius,float far_radius,uint8_t flag=0) noexcept {
    if(d<=near_radius)return .45f*d/near_radius;
    if(d<=far_radius)return .4375f+.05f*(flag?.6f:d/far_radius);
    return flag?std::min(.55f+.05f*2.2f,1.55f):.55f;
}
inline uint32_t distance_class(float d,float near_radius,float far_radius) noexcept {
    return d<=near_radius?0u:d<=far_radius?1u:2u;
}
// Mirrors geometry 0x1e78280: plane-local height is removed before radial
// distance, then the native camera/plane yaw rotates the planar ray.
inline bool project_model(const Model& m,const std::array<float,3>& world,
                          std::array<float,2>& uv,float& height,float* radial=nullptr,
                          float* scale_out=nullptr) noexcept {
    uv={};height=0;
    if(!valid(m)||!finite(world))return false;
    auto local=local_displacement(m.plane,m.origin,world);
    height=local[1];local[1]=0;
    const auto planar=rotate(m.plane,local);
    const float distance=std::sqrt(planar[0]*planar[0]+planar[1]*planar[1]+planar[2]*planar[2]);
    if(!std::isfinite(distance)||!std::isfinite(height))return false;
    const float scale=distance_scale(distance,m.near_radius,m.far_radius);
    if(radial)*radial=distance;
    if(scale_out)*scale_out=scale;
    if(distance<1e-5f){uv={.5f,.5f};return true;}
    const auto relative=multiply(conjugate(m.plane),m.reference);
    const float yaw=std::atan2(2*(relative[0]*relative[2]+relative[1]*relative[3]),
                               1-2*(relative[0]*relative[0]+relative[1]*relative[1]));
    const std::array<float,4> yaw_quat{0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)};
    const auto orientation=multiply(m.plane,yaw_quat);
    const auto oriented=rotate(conjugate(orientation),
        {planar[0]/distance,planar[1]/distance,planar[2]/distance});
    uv={.5f+scale*oriented[0],.5f-scale*oriented[2]};
    return finite(uv);
}
inline bool model_from_frame(const Frame& f,Model& m) noexcept {
    if(f.flag!=0||!finite(f.target)||!finite(f.origin))return false;
    m={f.reference,f.plane,{f.origin[0],f.origin[1],f.origin[2]},f.near_radius,f.far_radius};
    return valid(m);
}
inline bool compare_native(const Frame& frame,const NativeResult& native,float& error) noexcept {
    Model m{};std::array<float,2> uv{};float height{},radial{},scale{};
    if(!model_from_frame(frame,m)||!project_model(m,{frame.target[0],frame.target[1],frame.target[2]},
                                           uv,height,&radial,&scale)||
       !finite(native.origin)||!finite(native.uv)||!std::isfinite(native.distance)||
       !std::isfinite(native.scale))return false;
    auto local=local_displacement(m.plane,m.origin,{frame.target[0],frame.target[1],frame.target[2]});
    local[1]=0;
    const auto planar=rotate(m.plane,local);
    const float origin_error=std::max({std::abs(m.origin[0]+planar[0]-native.origin[0]),
                                       std::abs(m.origin[1]+planar[1]-native.origin[1]),
                                       std::abs(m.origin[2]+planar[2]-native.origin[2])});
    const float uv_error=std::max(std::abs(uv[0]-native.uv[0]),std::abs(uv[1]-native.uv[1]));
    error=std::max({uv_error,std::abs(scale-native.scale),
                    std::abs(radial-native.distance)/(1+radial),origin_error/(1+radial)});
    return uv_error<=.002f&&std::abs(scale-native.scale)<=.002f&&
        std::abs(radial-native.distance)<=.002f*(1+radial)&&
        origin_error<=.002f*(1+radial)&&
        native.radial_class==distance_class(radial,m.near_radius,m.far_radius)&&
        native.vertical_class==distance_class(std::abs(height),m.near_radius,m.far_radius);
}
inline bool project(const Snapshot& s,const std::array<float,3>& world,
                    std::array<float,2>& uv,float& relative_height) noexcept {
    uv={};relative_height=0;
    if(!valid(s))return false;
    std::array<float,2> current{},previous{};float old_height{};
    if(!project_model(s.current,world,current,relative_height)||
       !project_model(s.previous,world,previous,old_height))return false;
    uv={previous[0]+(current[0]-previous[0])*s.interpolation,
        previous[1]+(current[1]-previous[1])*s.interpolation};
    return finite(uv);
}
inline bool clip_single(const Model& m,std::array<float,3>& a,std::array<float,3>& b) noexcept {
    if(!valid(m)||!finite(a)||!finite(b))return false;
    const auto la=local_displacement(m.plane,m.origin,a),lb=local_displacement(m.plane,m.origin,b);
    const double dx=double(lb[0])-la[0],dz=double(lb[2])-la[2];
    const double aa=dx*dx+dz*dz,bb=2*(double(la[0])*dx+double(la[2])*dz);
    const double cc=double(la[0])*la[0]+double(la[2])*la[2]-double(m.near_radius)*m.near_radius;
    double enter=0,leave=1;
    if(aa<1e-12){if(cc>0)return false;}
    else {
        const double discriminant=bb*bb-4*aa*cc;
        if(discriminant<0||!std::isfinite(discriminant))return false;
        const double root=std::sqrt(discriminant);
        enter=std::max(enter,(-bb-root)/(2*aa));
        leave=std::min(leave,(-bb+root)/(2*aa));
        if(enter>leave)return false;
    }
    const auto first=a,last=b;
    for(size_t i=0;i<3;++i){
        a[i]=static_cast<float>(first[i]+enter*(double(last[i])-first[i]));
        b[i]=static_cast<float>(first[i]+leave*(double(last[i])-first[i]));
    }
    return finite(a)&&finite(b);
}
inline bool clip_world_xz(const Model& m,std::array<float,3>& a,std::array<float,3>& b) noexcept {
    if(!valid(m)||!finite(a)||!finite(b))return false;
    const double x=double(a[0])-m.origin[0],z=double(a[2])-m.origin[2];
    const double dx=double(b[0])-a[0],dz=double(b[2])-a[2];
    const double aa=dx*dx+dz*dz,bb=2*(x*dx+z*dz);
    const double cc=x*x+z*z-double(m.far_radius)*m.far_radius;
    double enter=0,leave=1;
    if(aa<1e-12){if(cc>0)return false;}
    else {
        const double disc=bb*bb-4*aa*cc;
        if(disc<0||!std::isfinite(disc))return false;
        const double root=std::sqrt(disc);
        enter=std::max(enter,(-bb-root)/(2*aa));
        leave=std::min(leave,(-bb+root)/(2*aa));
        if(enter>leave)return false;
    }
    const auto first=a,last=b;
    for(size_t i=0;i<3;++i){
        a[i]=static_cast<float>(first[i]+enter*(double(last[i])-first[i]));
        b[i]=static_cast<float>(first[i]+leave*(double(last[i])-first[i]));
    }
    return finite(a)&&finite(b);
}
inline bool clip_segment(const Snapshot& s,std::array<float,3>& a,std::array<float,3>& b) noexcept {
    return valid(s)&&clip_single(s.current,a,b)&&clip_single(s.previous,a,b)&&
           clip_world_xz(s.current,a,b)&&clip_world_xz(s.previous,a,b);
}
}
