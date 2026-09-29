#pragma once
#include <cstdint>
struct IDXGISwapChain;
namespace crml::probe {
// All graphics work runs on intercepted render calls. The worker publishes state only.
// verified_image is accepted only after the caller verifies the supported executable
// fingerprint. Zero prepares an overlay for explicit attachment by a native host.
void* overlay_create(uintptr_t verified_image, bool physics_trial=false,bool guest_physics=false,bool guest_movement=false) noexcept;
// Called with a borrowed, live swapchain on its owning render thread. Never creates
// a device, window or swapchain. Safe both before and after the first frame.
bool overlay_attach(IDXGISwapChain* swap) noexcept;
// toggle_down is the raw Insert key state, including while focus is lost.
// Hiding the panel never changes a gameplay lease or mod input.
void overlay_update(void* window, bool focused, int status, bool camera_valid=true, bool toggle_down=false) noexcept;
void overlay_destroy(void* window) noexcept;
struct OverlayDiagnostics { unsigned long long presents, frames, queue_matches; const char* status; };
OverlayDiagnostics overlay_diagnostics() noexcept;
#ifdef CRML_OVERLAY_TESTING
bool overlay_test_bind_present(void* target) noexcept;
#endif
}
