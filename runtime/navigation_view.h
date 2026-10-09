#pragma once
#include <cstdint>

namespace crml::navigation {
// Private copied observation. The quaternion is the player's MovementPlane,
// not a camera transform or a collision surface normal. No persistent identity
// is inferred from the world address or entity generation.
struct GroundObservation {
    float rotation[4]{}; // x,y,z,w
    float up[3]{};
};
enum class GroundStatus { unavailable, valid, quaternion, changed, memory };

// Reject malformed/non-unit input; output is cleared on every failure.
bool ground_up(const float (&rotation)[4], float (&up)[3]) noexcept;

// Read only, bounded, no engine calls. Caller must be in the authenticated player
// movement callback for the reviewed executable. Repeated identity/data checks
// detect observed relocation but do not establish cross-thread synchronization.
GroundStatus inspect_ground(const void* movement_view, uintptr_t world, uint64_t player, GroundObservation& out) noexcept;
}
