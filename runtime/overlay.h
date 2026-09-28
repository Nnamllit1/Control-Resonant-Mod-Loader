#pragma once
namespace crml::probe {
// All graphics work runs on intercepted render calls. The worker publishes state only.
void* overlay_create(bool physics_trial=false,bool guest_physics=false,bool guest_movement=false) noexcept;
// toggle_down is the raw Insert key state, including while focus is lost.
// Hiding the panel never changes a gameplay lease or mod input.
void overlay_update(void* window, bool focused, int status, bool camera_valid=true, bool toggle_down=false) noexcept;
void overlay_destroy(void* window) noexcept;
struct OverlayDiagnostics { unsigned long long presents, frames, queue_matches; const char* status; };
OverlayDiagnostics overlay_diagnostics() noexcept;
}
