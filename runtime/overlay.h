#pragma once
namespace crml::probe {
// All graphics work runs on intercepted render calls. The worker publishes state only.
void* overlay_create() noexcept;
void overlay_update(void* window, bool visible, int status) noexcept;
void overlay_destroy(void* window) noexcept;
struct OverlayDiagnostics { unsigned long long presents, frames, queue_matches; const char* status; };
OverlayDiagnostics overlay_diagnostics() noexcept;
}
