#pragma once
#include <stdint.h>

// Guest SDK, compiled to wasm32 (no WASI or native DLL target).
#if !defined(__wasm32__)
#error "CRML mods must target wasm32"
#endif
#define CRML_EXPORT(name) __attribute__((export_name(name)))
#ifdef __cplusplus
extern "C" {
#endif
// Requires capabilities=log. UTF-8 bytes in guest memory, at most 4096 per call.
__attribute__((import_module("crml_v1"), import_name("log")))
void crml_log(int32_t level, const char* text, uint32_t length);
// Experimental; requires capabilities=player.noclip. Poll every heartbeat.
// Native F6 toggles, Esc/focus loss cancels. Speed: 0.25..20 units/s (Shift x3).
// Returns 1 on, 0 off, -1 unavailable, -2 owned by another mod.
__attribute__((import_module("crml_v1"), import_name("noclip_poll")))
int32_t crml_noclip_poll(float speed);
// Bounded, focus-gated buttons; requires input.buttons. No text-key input.
#define CRML_BUTTON_F7 1u
#define CRML_BUTTON_F8 2u
__attribute__((import_module("crml_v1"), import_name("input_buttons")))
uint32_t crml_input_buttons(void);
// Requires player.visibility. 1 renews a 500 ms hide lease; 0 releases it.
// The guest chooses when to hide. Native focus/Escape checks can cancel requests.
// Returns 1 renewed, 0 released, -1 unavailable, -2 another owner.
__attribute__((import_module("crml_v1"), import_name("visibility_set")))
int32_t crml_visibility_set(int32_t hidden);
// Legacy native-key helper; new mods should use visibility_set instead.
// Experimental; requires player.visibility and the visibility installation opt-in.
// Poll every heartbeat. Hold F7 to hide the player mesh; release restores engine control.
// Returns 1 lease renewed, 0 released, -1 unavailable, -2 another mod owns it.
// A renewed lease is not confirmation that a render command was applied.
__attribute__((import_module("crml_v1"), import_name("visibility_poll")))
int32_t crml_visibility_poll(void);
// Experimental physics.damping capability; isolated --physics-wasm mode.
// Commands return 0 queued, -1 unavailable, -2 busy/another owner, -3 stale handle.
// Queued is not confirmation of execution. One owner/prop at a time.
__attribute__((import_module("crml_v1"), import_name("physics_select")))
int32_t crml_physics_select(void);
// Opaque owner-scoped token, not an entity ID or pointer. Zero means unavailable.
__attribute__((import_module("crml_v1"), import_name("physics_target")))
uint64_t crml_physics_target(void);
// Finite value 0..8, duration 1..5000 ms. Bad ranges trap the guest.
__attribute__((import_module("crml_v1"), import_name("physics_apply")))
int32_t crml_physics_apply(uint64_t target, float damping, uint32_t duration_ms);
// 0 idle, 1 queued, 2 searching, 3 selected, 4 active, 5 restoring,
// 6 finished, 7 retired, 8 game conflict, 9 refused; negative as above.
__attribute__((import_module("crml_v1"), import_name("physics_status")))
int32_t crml_physics_status(void);
// Requests restoration/cancellation and releases ownership after cleanup.
__attribute__((import_module("crml_v1"), import_name("physics_restore")))
int32_t crml_physics_restore(void);
// Fixed motion buttons; requires input.motion. Zero without focus/service.
#define CRML_MOTION_F6 1u
#define CRML_MOTION_W 2u
#define CRML_MOTION_S 4u
#define CRML_MOTION_A 8u
#define CRML_MOTION_D 16u
#define CRML_MOTION_SPACE 32u
#define CRML_MOTION_CTRL 64u
#define CRML_MOTION_SHIFT 128u
__attribute__((import_module("crml_v1"), import_name("input_motion")))
uint32_t crml_input_motion(void);
// Requires player.motion and --movement-wasm. Writes normalized camera right
// X,Z as two f32s (8 bytes). Returns 1 valid, -1 unavailable (output zeroed).
__attribute__((import_module("crml_v1"), import_name("motion_camera")))
int32_t crml_motion_camera(float right_xz[2]);
// World-space velocity, length <=20 units/s. 1 renews a 500ms noncolliding
// character-motion lease with keyboard suppression; 0 releases. Returns
// 1 accepted, 0 released, -1 unavailable, -2 another owner. Bad args trap.
// Teleports/identity changes cancel; no fall/reset or camera-position override.
__attribute__((import_module("crml_v1"), import_name("motion_set")))
int32_t crml_motion_set(int32_t enabled, float x, float y, float z);
CRML_EXPORT("crml_abi_version") uint32_t crml_abi_version(void);
CRML_EXPORT("crml_init") void crml_init(void);
// Optional; worker heartbeat, NOT a game frame or game-thread callback.
CRML_EXPORT("crml_tick") void crml_tick(float elapsed_seconds);
CRML_EXPORT("crml_shutdown") void crml_shutdown(void);
#ifdef __cplusplus
}
#endif
