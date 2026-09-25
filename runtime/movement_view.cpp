#include "movement_view.h"
#include <Windows.h>
#include <cmath>
#include <cstring>

namespace crml::probe {
namespace {
template<class T> T read(uintptr_t address) noexcept {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}
uintptr_t component(uintptr_t world, uint16_t archetype, uintptr_t chunk, uint32_t row, uint32_t hash, uint32_t stride) noexcept {
    const auto meta = world + (archetype + 0xc22ull) * 32;
    const auto count = read<uint32_t>(meta + 0x24);
    if (!count || count > 2048) return 0;
    const auto hashes = read<uintptr_t>(meta + 0x10);
    const auto offsets = read<uintptr_t>(world + 0x18458 + archetype * 32ull);
    for (uint32_t i = 0; i < count; ++i) {
        if (read<uint32_t>(hashes + i * 4ull) != hash) continue;
        const auto offset = read<uint32_t>(offsets + i * 4ull);
        if (!offset || offset > 64 * 1024 * 1024) return 0;
        return chunk + offset + row * static_cast<uintptr_t>(stride);
    }
    return 0;
}
Observation inspect_inner(const void* view, const void* world_view, uint16_t player_tag, Sample& out) noexcept {
    out.rejection=Rejection::arguments;
    if (!view || !world_view || player_tag >= 16384) return Observation::invalid;
    out.rejection=Rejection::world;
    const auto ctx = reinterpret_cast<uintptr_t>(view);
    const auto world = read<uintptr_t>(reinterpret_cast<uintptr_t>(world_view));
    const auto row = read<uint64_t>(ctx + 0x90);
    const auto chunk = read<uintptr_t>(ctx + 0x88);
    if (!world || !chunk || row >= 16384) return Observation::invalid;
    out.rejection=Rejection::entity;
    // ctx+0x68 is GlobalID (persistent content identity), NOT an ECS handle.
    // Runtime handles live in the chunk header and include the entity generation.
    const auto entity = read<uint64_t>(chunk + 0x10 + row * 8);
    if (!entity || entity == UINT64_MAX) return Observation::invalid;
    out.rejection=Rejection::generation;
    const auto index = static_cast<uint32_t>(entity);
    const auto capacity = read<uint64_t>(world + 0x58510);
    if (capacity > 16 * 1024 * 1024 || index >= capacity) return Observation::invalid;
    const auto generations = read<uintptr_t>(world + 0x584e8);
    if (read<uint32_t>(generations + index * 8ull) != entity >> 32) return Observation::invalid;
    out.rejection=Rejection::location;
    const auto locations = read<uintptr_t>(world + 0x58530);
    const auto location = read<uint64_t>(locations + index * 8ull);
    const auto archetype = static_cast<uint16_t>(location);
    const auto meta = read<uintptr_t>(world + 0x58478);
    const auto count = read<uint64_t>(meta + 8);
    if (!count || count > 8192 || archetype >= count || location >> 32 != row) return Observation::invalid;
    if (read<uintptr_t>(world + 0x50 + archetype * 8ull) != chunk || row >= read<uint32_t>(world + 0x10448 + archetype * 4ull)) return Observation::invalid;
    out.rejection=Rejection::tag;
    const auto registry = read<uintptr_t>(world);
    const auto table = read<uintptr_t>(registry + 0x48);
    if (player_tag >= read<uint32_t>(registry + 0x50)) return Observation::invalid;
    const auto slot = read<uint16_t>(table + player_tag * 2ull);
    if (slot >= 16384) return Observation::invalid;
    const auto bits = read<uintptr_t>(world + 0x58480) + slot * 1024ull;
    if (!(read<uint8_t>(bits + archetype / 8) & (1u << (archetype % 8)))) return Observation::other_entity;
    out.rejection=Rejection::components;
    const auto transform = component(world, archetype, chunk, static_cast<uint32_t>(row), 0x6cfbb2a9, 32);
    const auto controller = component(world, archetype, chunk, static_cast<uint32_t>(row), 0x9b382c56, 32);
    const auto keyframed = component(world, archetype, chunk, static_cast<uint32_t>(row), 0x5077c6e3, 2);
    const auto disabled = component(world, archetype, chunk, static_cast<uint32_t>(row), 0x6da4a5ae, 1);
    const auto pushability = component(world, archetype, chunk, static_cast<uint32_t>(row), 0x2a16db09, 8);
    const auto teleported = component(world, archetype, chunk, static_cast<uint32_t>(row), 0xc55ae319, 1);
    if (!transform || !controller || !keyframed || !disabled || !pushability || !teleported) return Observation::invalid;
    out.rejection=Rejection::layout;
    if (transform != read<uintptr_t>(ctx + 0x58) + row * 32 || controller != read<uintptr_t>(ctx) + row * 32 || keyframed != read<uintptr_t>(ctx + 0x38) + row * 2) return Observation::invalid;
    if (pushability != read<uintptr_t>(ctx + 0x40) + row * 8 || teleported != read<uintptr_t>(ctx + 0x70) + row) return Observation::invalid;
    out.rejection=Rejection::controller;
    const auto physics = read<uintptr_t>(controller);
    const auto movement = read<uintptr_t>(controller + 8);
    if (!physics || !movement || read<uintptr_t>(movement + 0x10) != physics) return Observation::invalid;
    out.entity = entity;
    out.world = world;
    out.row = static_cast<uint32_t>(row);
    std::memcpy(out.position, reinterpret_cast<void*>(transform + 16), 12);
    std::memcpy(out.controller_position, reinterpret_cast<void*>(physics + 16), 12);
    std::memcpy(out.keyframed, reinterpret_cast<void*>(keyframed), 2);
    out.disabled = read<uint8_t>(disabled);
    out.teleported = read<uint8_t>(teleported);
    out.rejection=Rejection::coordinates;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(out.position[i]) || !std::isfinite(out.controller_position[i])) return Observation::invalid;
    }
    out.rejection=Rejection::none;
    return Observation::player;
}

bool camera_inner(uintptr_t world, CameraBasis& out) noexcept {
    if(!world) return false;
    // camera_look_direction resolves the main camera through this world-global entry.
    const auto size=read<uint32_t>(world+0x585b8);
    if(!size || size>16384) return false;
    const auto hashes=read<uintptr_t>(world+0x585b0), values=read<uintptr_t>(world+0x585c0);
    uintptr_t cameras{};
    for(uint32_t i=0;i<size;++i) if(read<uint32_t>(hashes+i*4ull)==0xfe90f7f8) {
        cameras=read<uintptr_t>(values+i*8ull); break;
    }
    if(!cameras) return false;
    const auto entity=read<uint64_t>(cameras+4);
    if(!entity || entity==UINT64_MAX) return false;
    const auto index=static_cast<uint32_t>(entity);
    const auto capacity=read<uint64_t>(world+0x58510);
    if(capacity>16*1024*1024 || index>=capacity) return false;
    if(read<uint32_t>(read<uintptr_t>(world+0x584e8)+index*8ull)!=entity>>32) return false;
    const auto location=read<uint64_t>(read<uintptr_t>(world+0x58530)+index*8ull);
    const auto archetype=static_cast<uint16_t>(location);
    const auto row=static_cast<uint32_t>(location>>32);
    const auto count=read<uint64_t>(read<uintptr_t>(world+0x58478)+8);
    if(count>8192 || archetype>=count || row>=16384) return false;
    const auto chunk=read<uintptr_t>(world+0x50+archetype*8ull);
    if(!chunk || row>=read<uint32_t>(world+0x10448+archetype*4ull) || read<uint64_t>(chunk+0x10+row*8ull)!=entity) return false;
    // nl_camera_ray reads this 64-byte Camera component: 3x4 world basis, then lens.
    const auto camera=component(world,archetype,chunk,row,0x46967561,64);
    if(!camera) return false;
    float basis[9]; std::memcpy(basis,reinterpret_cast<void*>(camera),sizeof(basis));
    for(float value:basis) if(!std::isfinite(value)) return false;
    for(int a=0;a<3;++a) for(int b=a;b<3;++b) {
        float dot{}; for(int i=0;i<3;++i) dot+=basis[a*3+i]*basis[b*3+i];
        if(std::abs(dot-(a==b?1.f:0.f))>.1f) return false;
    }
    out.entity=entity;
    std::memcpy(out.right,basis,12); std::memcpy(out.forward,basis+6,12);
    out.valid=true;
    return true;
}
}
bool inspect_camera(uintptr_t world, CameraBasis& result) noexcept {
    result={};
    __try { return camera_inner(world,result); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { result={}; return false; }
}
Observation inspect(const void* view, const void* world, uint16_t tag, Sample& result) noexcept {
    // Guard only diagnostic reads, never exceptions from the original game routine.
    __try { return inspect_inner(view, world, tag, result); }
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        result.rejection=Rejection::memory;
        return Observation::invalid;
    }
}
}
