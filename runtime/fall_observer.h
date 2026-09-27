#pragma once
#include "fall_guard.h"
#include <array>
#include <iosfwd>

namespace crml::probe::fall_trace {
enum class Stage : unsigned { controller, inactive, trigger, recovery, camera, count };
struct State {
    uint64_t entity{}, source{};
    uint8_t blocked{}, flags{}, safe_valid{}, camera_active{}, fade_latched{}, teleported{};
    uint8_t camera_present{}, fade_present{}, fade_pending{}, fade_aux{};
    uint32_t fade_level{};
    float elapsed{}, transition_delay{}, effect_delay{}, fade_duration{};
    std::array<float,3> position{}, safe_position{};
};
// Fresh generation-checked reads only; no component pointers escape this call.
bool snapshot(fall::Player player,State& out,const void* fade=nullptr) noexcept;
bool changed(const State& before,const State& after) noexcept;
// Caller has already checked the executable fingerprint. Mutually exclusive
// with the legacy fall guard. Hook failure leaves every installed hook passive.
bool start(uintptr_t image,fall::Active player) noexcept;
void controller(fall::Player player) noexcept;
void write(std::ostream& out);
void stop() noexcept;
#ifdef CRML_FALL_TRACE_TESTING
namespace testing {
void configure(fall::Active,void*,void*,void*,void*) noexcept;
void invoke(Stage,const std::array<void*,8>&);
void hold_lock(bool) noexcept;
}
#endif
}
