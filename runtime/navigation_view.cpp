#include "navigation_view.h"
#include "movement_view.h"
#include "compatibility.h"
#include <Windows.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace crml::navigation {
bool ground_up(const float (&rotation)[4], float (&up)[3]) noexcept {
    up[0]=up[1]=up[2]=0;
    double norm{};
    for(float value:rotation) {
        if(!std::isfinite(value)) return false;
        norm+=static_cast<double>(value)*value;
    }
    if(norm<0.98 || norm>1.02) return false;
    const double x=rotation[0],y=rotation[1],z=rotation[2],w=rotation[3];
    // Native sonar consumer 0x1e67d20..0x1e67d77: MovementPlane * local +Y.
    // Divide by quaternion norm to tolerate normal floating-point drift.
    up[0]=static_cast<float>(2*(x*y-w*z)/norm);
    up[1]=static_cast<float>(1-2*(x*x+z*z)/norm);
    up[2]=static_cast<float>(2*(y*z+w*x)/norm);
    return true;
}
namespace {
GroundStatus inspect_inner(const void* movement_view,uintptr_t world,uint64_t player,GroundObservation& out) noexcept {
    constexpr uint32_t hash=0xc0853848, stride=16;
    uintptr_t chunk{};uint32_t row{};
    const auto component=probe::entity_component(world,player,hash,stride,chunk,row);
    if(!component || component>(std::numeric_limits<uintptr_t>::max)()-stride) return GroundStatus::unavailable;
    const auto* view=static_cast<const uintptr_t*>(movement_view);
    const auto plane_base=view[0x50/8];
    if(view[0x88/8]!=chunk || view[0x90/8]!=row || !plane_base ||
       plane_base>(std::numeric_limits<uintptr_t>::max)()-static_cast<uintptr_t>(row)*stride ||
       plane_base+static_cast<uintptr_t>(row)*stride!=component) return GroundStatus::unavailable;
    GroundObservation candidate{};
    std::memcpy(candidate.rotation,reinterpret_cast<const void*>(component),stride);
    if(!ground_up(candidate.rotation,candidate.up)) return GroundStatus::quaternion;
    uintptr_t final_chunk{};uint32_t final_row{};
    const auto final_component=probe::entity_component(world,player,hash,stride,final_chunk,final_row);
    if(final_component!=component || final_chunk!=chunk || final_row!=row ||
       view[0x50/8]!=plane_base || view[0x88/8]!=chunk || view[0x90/8]!=row ||
       std::memcmp(candidate.rotation,reinterpret_cast<const void*>(component),stride)) return GroundStatus::changed;
    out=candidate;
    return GroundStatus::valid;
}
}
GroundStatus inspect_ground(const void* movement_view,uintptr_t world,uint64_t player,GroundObservation& out) noexcept {
    out={};
    if(!compatibility::reviewed_build || compatibility::engine_profile!=compatibility::EngineProfile::october_patch ||
       !movement_view || !world || !player || player==UINT64_MAX) return GroundStatus::unavailable;
    __try {return inspect_inner(movement_view,world,player,out);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        out={};return GroundStatus::memory;
    }
}
}
