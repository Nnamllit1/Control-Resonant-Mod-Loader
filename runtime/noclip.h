#pragma once
#include "movement_view.h"
#include <array>

namespace crml::probe {
struct Direction { float x{}, y{}, z{}; bool fast{}; };
// Pure state machine, called under the bridge lock. No game memory is written.
struct Flight {
    uint64_t owner{}, entity{}, world{}, lease{}, last_step{};
    float speed{5};
    bool enabled{};
    void reset() noexcept { *this = Flight{}; }
    bool step(const Sample& sample, uint64_t current_world, uint64_t now, bool focused,
              Direction input, std::array<float, 3>& target) noexcept;
};
// Per-call argument storage. Engine row indexing resolves onto these private values.
struct Override {
    alignas(16) std::array<float, 8> transform{};
    std::array<uintptr_t, 19> view{};
    std::array<uint8_t, 2> keyframed{1, 0};
    std::array<uint8_t, 6> padding{};
    bool prepare(const void* original, const Sample& sample, const std::array<float, 3>& target) noexcept;
};
}
