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
struct SimulationContext { uintptr_t world{}, owner{}; };
// The ECS dispatcher's first argument is a temporary {world, scheduler} view.
// Cross-check its world against the PhysicsScene passed through descriptor+0x58.
// All returned addresses expire with this dispatcher invocation.
bool read_simulation_context(uintptr_t view, uintptr_t descriptor, uintptr_t expected_dispatcher,
                             SimulationContext& result) noexcept;
LinkRead read_body_entity(uintptr_t world, uintptr_t owner, const BodySnapshot& body,
                          BodyEntitySnapshot& result) noexcept;
enum class TargetRead : uint8_t { ok, arguments, world, entity, bounds, scene,
    instance, body, identity, association, changed, memory };
struct EntityBodySnapshot {
    uintptr_t owner{}; // Callback-local; never retain this or body.actor across callbacks.
    BodySnapshot body{};
    BodyEntitySnapshot link{};
};
// Resolve a deliberately selected entity/local-body pair from the current world.
// expected_handle is the complete native body handle from selection, including generation.
// UINT64_MAX discovers the current handle; a retained selection must supply its exact handle.
// Caller still owns world-incarnation/phase checks; handles alone do not identify a world.
TargetRead read_entity_body(uintptr_t world, uint64_t entity, uint32_t local_index,
                            uint64_t expected_handle, uintptr_t dynamic_vtable,
                            EntityBodySnapshot& result) noexcept;
using DampingGetter = float(*)(uintptr_t);
struct DampingAccessors { uintptr_t vtable{}; DampingGetter linear{}, angular{}; };
enum class AccessRead : uint8_t { ok, arguments, snapshot, slot, scalar, changed, mismatch, memory, count };
struct DampingReadback { float linear{}, angular{}; bool alternate{}; };
// Internal read-only probe: exact fingerprinted getter targets, never guest function pointers.
// Output is present for ok/mismatch only. Neither result authorizes mutation.
AccessRead read_damping_accessors(uintptr_t owner, const BodySnapshot& expected,
                                 const DampingAccessors& accessors, DampingReadback& result) noexcept;
using DampingSetter = void(*)(uintptr_t, float);
enum class WriteStatus : uint8_t { ok, arguments, context, target, changed, slot,
    scene_busy, getter, readback, memory };
struct DampingWrite { WriteStatus status{WriteStatus::arguments}; bool attempted{}; };
// Trusted native experiment only. Call from the simulation dispatcher's owning
// interval with a freshly resolved target and a valid lifecycle scope. This
// comparison is NOT atomic and neither the scene flag nor SEH acquires ownership.
// attempted remains true on uncertain failures after entering the setter.
DampingWrite write_linear_damping(const SimulationContext&, const EntityBodySnapshot&,
                                  const DampingAccessors&, DampingSetter,
                                  float expected, float value) noexcept;
}
