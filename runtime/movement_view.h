#pragma once
#include <cstdint>

namespace crml::probe {
enum class Rejection { none, arguments, world, entity, generation, location, tag, components, layout, controller, coordinates, memory, count };
inline constexpr const char* rejection_names[]{"none","arguments","world","entity","generation","location","tag","components","layout","controller","coordinates","memory"};
// Internal diagnostic ABI for the fingerprinted game build. Never exposed to guests.
struct Sample {
    Rejection rejection{};
    uint64_t entity{};
    uintptr_t world{};
    uint32_t row{};
    float position[3]{};
    float controller_position[3]{};
    uint8_t keyframed[2]{};
    uint8_t disabled{};
};
enum class Observation { invalid, other_entity, player };
Observation inspect(const void* movement_view, const void* world_view, uint16_t player_tag, Sample& result) noexcept;
}
