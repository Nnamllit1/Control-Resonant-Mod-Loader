#pragma once
#include <cstdint>

namespace crml::observer {
enum class BodyRead : uint8_t { ok, arguments, bounds, generation, missing_actor,
    actor_type, actor_identity, representation, scalar, changed, memory, count };
struct BodySnapshot {
    uint64_t handle{}, actor_identity{};
    uintptr_t actor{}; // Callback-local only; hash before enqueueing a record.
    float linear_damping{}, angular_damping{};
    bool alternate{};
};
// Guarded observations, not ownership acquisition or callable mod handles.
// Caller must gate both executable and PhysX fingerprints and use an engine callback.
uint32_t body_slot_count(uintptr_t owner) noexcept;
BodyRead read_body(uintptr_t owner, uint32_t index, uintptr_t dynamic_vtable,
                   BodySnapshot& result) noexcept;
enum class LinkRead : uint8_t { ok, arguments, world, bounds, association, scene,
    instance, entity, changed, memory, count };
struct BodyEntitySnapshot { uint64_t entity{}; uint32_t scene_slot{}, local_index{}; };
// Use only a world supplied by the current callback, never one retained from a prior frame.
uintptr_t world_scene(uintptr_t world) noexcept;
LinkRead read_body_entity(uintptr_t world, uintptr_t owner, const BodySnapshot& body,
                          BodyEntitySnapshot& result) noexcept;
}
