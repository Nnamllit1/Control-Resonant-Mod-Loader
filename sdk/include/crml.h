#pragma once
#include <stdint.h>
#include "crml_abi.h"
#include "crml_state.h"
#include "crml_ui.h"
#include "crml_media.h"

// Guest SDK, compiled to wasm32 (no WASI or native DLL target).
#if !defined(__wasm32__)
#error "CRML mods must target wasm32"
#endif
#define CRML_EXPORT(name) __attribute__((export_name(name)))
#ifdef __cplusplus
extern "C" {
#endif
// Intersection of declared permissions and installed services. No extra permission
// needed. Availability does not promise a ready player, target or lease ownership.
// Alpha 4.2+. Counts toward the shared eight-call host budget per callback.
__attribute__((import_module("crml_v1"), import_name("capabilities")))
uint32_t crml_capabilities(void);
// Requests cleanup of this mod's native leases without unloading the mod.
// No ownership argument or extra capability. Physics restoration can be deferred
// to an engine callback. Automatic trap/unload cleanup remains in force.
// Alpha 4.2+; counts toward the same eight-call budget.
__attribute__((import_module("crml_v1"), import_name("release")))
void crml_release(void);
// Requires input.actions. Held-state bits correspond to manifest action.0..15.
// Configurable, read-only, foreground-only; no key consumption or injected input.
// Returns zero on focus loss/Escape or unavailable/stale input. Alpha 4.2+.
__attribute__((import_module("crml_v1"), import_name("input_actions")))
uint32_t crml_input_actions(void);
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
// Experimental physics.damping capability; manifest-driven from Alpha 4.2.
// Commands return 0 queued, -1 unavailable, -2 busy/another owner, -3 stale handle.
// Queued is not confirmation of execution. One owner/prop at a time.
// Alpha 4.2+: nearest eligible prop to a mod-chosen sphere center. Offset is
// world-aligned relative to the player at search start (not camera-relative).
// Finite offset length <=20; finite radius >0 and <=20 world units.
// Bad arguments trap. Search excludes player/attachments and multi-body props.
__attribute__((import_module("crml_v1"), import_name("physics_select_near")))
int32_t crml_physics_select_near(float offset_x, float offset_y, float offset_z, float radius);
// Compatibility helper: same as physics_select_near(0, 0, 0, 2).
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
// Copied callback snapshots, never live engine memory. Pass sizeof(*out).
// 1 copied, 0 not ready/stale observation, -1 unavailable. Failure zeros output.
// Each call consumes one of the shared eight calls per callback. Alpha 4.2+.
// Requires player.read; generation changes when player/world identity changes.
__attribute__((import_module("crml_v1"), import_name("player_read")))
int32_t crml_player_read(crml_player_state* out, uint32_t size);
// Requires camera.read. Selected engine-camera pose, not a final rendered image.
// Check flags before using pose/lens fields. No camera ownership or writes.
__attribute__((import_module("crml_v1"), import_name("camera_read")))
int32_t crml_camera_read(crml_camera_state* out, uint32_t size);
// Requires physics.damping and caller-owned selection token. Also returns
// -2 busy/another owner, -3 stale token. Retain the token while damping is active.
__attribute__((import_module("crml_v1"), import_name("physics_read")))
int32_t crml_physics_read(uint64_t target, crml_physics_state* out, uint32_t size);
// Requires ui.read. Pass sizeof(*out). Returns 1 copied, 0 not ready, or a
// negative status; all failures zero output. Counts toward the shared eight-call
// budget. Generation identifies this observed screen instance, not an address.
__attribute__((import_module("crml_v1"), import_name("ui_read")))
int32_t crml_ui_read(crml_ui_state* out, uint32_t size);
// Requires ui.activate. Use a nonzero generation from ui_read and a supported
// CRML_UI_ACTION_* value. Unsupported arguments trap; stale/unavailable requests
// return a negative status. Zero means queued, never confirmed completion.
// Requires the action's bit in the current screen's advertised actions mask.
// Counts toward the same eight-call budget; unload cancels this mod's requests.
__attribute__((import_module("crml_v1"), import_name("ui_activate")))
int32_t crml_ui_activate(uint64_t generation, uint32_t action);

// Requires ui.presentation. A temporary visibility override for one uniquely
// matched native UI element by ID or CLASS, never a selector or script. Name is
// 1..64 ASCII letters/digits/underscore/hyphen, without a NUL terminator in length.
// hidden=1 leases visibility:hidden for 1..1000 ms; hidden=0 requires duration=0
// and removes this owner's override. Generation must identify the current view.
// Eight named leases are shared globally. 0 means queued, -1 unavailable/stale,
// -2 busy/full. Requests share the eight-call budget. Expiry, screen/page changes
// and owner cleanup restore presentation; readiness-only changes may retain it.
// Engine facts are untouched. Queuing does not confirm a matching element exists.
__attribute__((import_module("crml_v1"), import_name("ui_present")))
int32_t crml_ui_present(uint64_t generation, uint32_t kind, const char* name,
                        uint32_t name_length, uint32_t hidden, uint32_t duration_ms);

// Requires media.read. Pass sizeof(*out). Returns 1 copied, 0 not ready, or a
// negative status; returned failures zero output. Name length excludes NUL and
// is at most CRML_MEDIA_NAME_CAPACITY-1. Check flags before using the name.
// Current adapters cover reviewed startup media, not arbitrary cutscenes.
__attribute__((import_module("crml_v1"), import_name("media_read")))
int32_t crml_media_read(crml_media_state* out, uint32_t size);
// Requires media.skip. Nonzero generation from an ACTIVE, SKIPPABLE observation.
// Zero means queued, never completed; negative means unavailable/stale/refused.
// A zero generation traps. Native readiness checks remain in force. Both media
// calls share the eight-call callback budget; unload cancels owned requests.
__attribute__((import_module("crml_v1"), import_name("media_skip")))
int32_t crml_media_skip(uint64_t generation);
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
// Requires player.motion (older runtimes also need --movement-wasm). Writes normalized camera right
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
