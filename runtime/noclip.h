#pragma once
#include "movement_view.h"
#include <array>

namespace crml::probe {
struct Direction { float x{}, y{}, z{}; bool fast{}; };
Direction camera_relative(Direction input, const CameraBasis& camera) noexcept;
enum class StopReason { none, toggle, focus, escape, stale_sample, lease, entity, world, disabled,
    teleport, keyframed, tick_gap, speed, displacement, direction, coordinates, view, mod_release, shutdown };
inline constexpr const char* stop_names[]{"none","toggle","focus_loss","escape","stale_sample","lease_expired","entity_changed",
    "world_changed","controller_disabled","engine_teleport","engine_keyframing","tick_gap","invalid_speed","displacement",
    "invalid_direction","invalid_coordinates","invalid_view","mod_release","shutdown"};
struct FlightStop {
    StopReason reason{};
    uint64_t count{},tick{},entity{};
    std::array<float,3> requested{},observed{};
};
struct TeleportRestore {
    uint64_t count{},tick{};
    std::array<float,3> requested{},observed{};
};
// Pure state machine, called under the bridge lock. No game memory is written.
struct Flight {
    uint64_t owner{}, entity{}, world{}, lease{}, last_step{};
    float speed{5};
    bool enabled{};
    bool positioned{};
    std::array<float,3> position{};
    FlightStop stopped{};
    TeleportRestore restored{};
    bool teleport_blocks(uint8_t flag) const noexcept { return flag && !(enabled && positioned); }
    void reset(StopReason reason=StopReason::toggle,uint64_t now=0,const Sample* sample=nullptr) noexcept;
    bool step(const Sample& sample, uint64_t current_world, uint64_t now, bool focused,
              Direction input, std::array<float, 3>& target) noexcept;
};
// Per-call argument storage. Engine row indexing resolves onto these private values.
struct Override {
    alignas(16) std::array<float, 8> transform{};
    std::array<uintptr_t, 19> view{};
    std::array<uint8_t, 2> keyframed{1, 0};
    std::array<uint8_t, 6> padding{};
    std::array<uint8_t, 8> pushability{};
    std::array<uint8_t, 8> teleported{};
    bool prepare(const void* original, const Sample& sample, const std::array<float, 3>& target) noexcept;
};
}
