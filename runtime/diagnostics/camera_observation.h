#pragma once
#include <array>
#include <cstdint>

namespace crml::camera {
enum class Read : uint8_t { ok, arguments, environment, selector, changed, memory };
enum SlotFlags : uint32_t { present=1, live=2, view=4, free_transform=8, invalid_pose=16, changed=32 };
struct Slot {
    uint64_t entity{}, view_digest{}, free_digest{};
    uint32_t flags{};
};
struct Snapshot {
    uintptr_t world{}, global{};
    int32_t selector{-1};
    std::array<Slot,4> slots{};
};
struct Selected {
    uintptr_t world{}, global{};
    uint64_t entity{};
    int32_t selector{-1};
    float position[3]{}, basis[9]{}, lens[2]{};
};
// Copies only: no engine calls, retained ownership or worker-side pointer use.
// Before/after identity checks detect some races; they are not synchronization.
Read inspect(uintptr_t world, uintptr_t expected_global, Snapshot& result) noexcept;
// Copies only the selected CameraView, not the final renderer override. The
// context is borrowed from camera::update; missing/foreign globals fail closed.
Read selected(void* update_context, Selected& result) noexcept;
}
