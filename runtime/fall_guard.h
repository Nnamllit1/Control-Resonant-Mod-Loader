#pragma once
#include <cstdint>
#include <array>

namespace crml::probe::fall {
struct Player { uintptr_t world{}; uint64_t entity{}; };
using Active = Player(*)() noexcept;
bool start(uintptr_t image, Active active) noexcept;
bool available(Player player) noexcept;
bool matches_inactive(const void* view,Player player) noexcept;
bool exclude_target(void* result,const void* query,uint64_t entity,Player player) noexcept;
// The independent player fall monitor drives script-visible distance/effects.
bool suppress_monitor(const void* view,Player player) noexcept;
bool matches_fall_action(const void* view,Player player) noexcept;
bool matches_active_recovery(const void* view,Player player) noexcept;
// Present an inactive recovery to the native camera updater, which performs its
// own fade-out and camera-override cleanup. Actual recovery state is untouched.
struct CameraOverride {
    std::array<uintptr_t,5> view{};
    std::array<uint8_t,8> padding{};
    alignas(16) std::array<uint8_t,240> recovery{};
    bool prepare(const void* original,Player player) noexcept;
};
uint64_t skipped_checks() noexcept;
uint64_t skipped_triggers() noexcept;
uint64_t skipped_monitors() noexcept;
uint64_t skipped_fall_actions() noexcept;
uint64_t skipped_active_recoveries() noexcept;
uint64_t camera_overrides() noexcept;
uint64_t camera_clear_requests() noexcept;
}
