#pragma once
#include "movement_view.h"
#include <array>
#include <ostream>

namespace crml::probe {
struct ComponentIdentity { uint32_t hash{}; uint8_t table{}; };
struct EntitySnapshot {
    bool valid{}, truncated{};
    uint16_t archetype{};
    uint32_t row{}, count{};
    uint64_t entity{};
    std::array<ComponentIdentity, 2048> components{};
    Sample player{};
};
// Called only on the observed movement thread. Copies identities, never follows
// unknown component payloads or retains their addresses for worker-side reads.
bool inspect_entity(const Sample& sample, EntitySnapshot& result) noexcept;
void write_entity_snapshot(std::ostream& out, const EntitySnapshot& snapshot,
                           uint64_t tick, uint32_t thread);
}
