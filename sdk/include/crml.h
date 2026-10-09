#pragma once
#include <stdint.h>
#include "crml_abi.h"
#include "crml_results.h"
#include "crml_state.h"
#include "crml_ui.h"
#include "crml_media.h"
#include "crml_settings.h"
#include "crml_feedback.h"
#include "crml_tutorial.h"
#include "crml_drawing.h"
#include "crml_lists.h"
#include "crml_action_rules.h"

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
// Alpha 4.2+. Development builds: counts toward eight observations per callback.
// Released runtimes through Alpha 4.3 instead share eight observations/commands.
// Set min_runtime=0.1.0-alpha.4.3.dev.0 when relying on separate allowances.
__attribute__((import_module("crml_v1"), import_name("capabilities")))
uint32_t crml_capabilities(void);
// Requires player.action_rules; min_runtime=0.1.0-alpha.4.4.dev.11.
// Renews an owner-local 500ms exception for the current player. Actions and
// restriction classes are explicit bitmasks; (0,0) releases this owner's lease.
// 1 accepted (not proof of action execution),0 released,-1 unavailable,
// -2 owner capacity,-3 invalid arguments,-5 no current player context.
// Does not unlock abilities, trigger input, clear quests, or waive other rules.
__attribute__((import_module("crml_v1"), import_name("action_rule_set")))
int32_t crml_action_rule_set(uint32_t actions,uint32_t restrictions);
// One observation; exact sizeof(crml_action_rule_state). 1 copied,-1 unavailable.
// LEASED reports a request only. Native rules may still reject the action.
__attribute__((import_module("crml_v1"), import_name("action_rule_read")))
int32_t crml_action_rule_read(crml_action_rule_state* state,uint32_t size);
// Requires lists; min_runtime=0.1.0-alpha.4.4.dev.1. Owner-local copied page in
// Options > Mods. Publish/hide share the eight-command allowance; next consumes
// one observation. Publish: positive structural revision, -1 unavailable,
// -2 capacity reserved, -3 invalid, -4 rate limited (100ms), -5 exhaustion.
// Identical content retains its revision. Search/pagination are guest policy.
__attribute__((import_module("crml_v1"), import_name("list_publish")))
int64_t crml_list_publish(const crml_list_page* page,uint32_t size);
// 1 consumes one accepted action,0 none,-1 unavailable. Non-success zeroes out.
// Up to16 queued actions; UI refuses further activations when full. Events keep
// their original page revision after replacement; guests decide whether to act.
__attribute__((import_module("crml_v1"), import_name("list_next")))
int32_t crml_list_next(crml_list_event* output,uint32_t size);
// 1 cleared,0 already hidden,-1 unknown owner. Clears pending actions as well.
// Renderer pixels update asynchronously through the normal Options refresh.
__attribute__((import_module("crml_v1"), import_name("list_hide")))
int32_t crml_list_hide(void);
// Requires drawing; min_runtime=0.1.0-alpha.4.4.dev.1. Both use one command from
// the shared eight-command invocation budget. Frames are copied atomically.
// publish: 1 admitted, -1 unavailable, -2 all four surfaces occupied, -3 invalid,
// -4 published less than100ms ago, -5 resource/revision exhaustion. Not proof of
// presentation. Renew within lifetime_ms (100..1000); no more than10Hz/owner.
// hide: 1 host state cleared,0 already hidden,-1 unavailable/unknown owner.
// Pixels clear on the next native UI poll or expiry, not synchronously. Automatic
// release/trap/unload cleanup also clears host state. No guest HTML or input.
__attribute__((import_module("crml_v1"), import_name("drawing_publish")))
int32_t crml_drawing_publish(const crml_drawing_frame* frame, uint32_t size);
__attribute__((import_module("crml_v1"), import_name("drawing_hide")))
int32_t crml_drawing_hide(void);
// Requires drawing; alpha.4.4.dev.2+. World positions use navigation_read's
// axes/units. Read a fresh context before publishing. Map requests expire on
// projection change or inactivity and never persist across a game session.
// Read: 1 ready, -1 unavailable. Publish: drawing results, -6 stale context.
// Legacy read/hide select CRML_MAP_FULL; publish selects the descriptor target.
__attribute__((import_module("crml_v1"), import_name("map_read")))
int32_t crml_map_read(crml_map_state* state, uint32_t size);
// dev.7: fresh copied affine projection; 1 success, -1 unavailable (zero output).
__attribute__((import_module("crml_v1"), import_name("map_projection_read")))
int32_t crml_map_projection_read(crml_map_projection* projection, uint32_t size);
__attribute__((import_module("crml_v1"), import_name("map_publish")))
int32_t crml_map_publish(const crml_map_frame* frame, uint32_t size);
__attribute__((import_module("crml_v1"), import_name("map_hide")))
int32_t crml_map_hide(void);
// Requires drawing; alpha.4.4.dev.4+. Targets FULL or SONAR; unknown target -3.
// Read: 1 ready, -1 unavailable (cleared output). Hide: 1 changed,0 hidden,-1 unknown owner.
// Native destinations share drawing capacity; legacy read/hide retain full-map semantics.
__attribute__((import_module("crml_v1"), import_name("map_read_target")))
int32_t crml_map_read_target(uint32_t target, crml_map_state* state, uint32_t size);
__attribute__((import_module("crml_v1"), import_name("map_hide_target")))
int32_t crml_map_hide_target(uint32_t target);
// drawing capability; dev.5+. Publish renews a 600ms full-map interaction lease.
// Same map context and validation rules; 24 items, 16 queued edits per owner.
// next: 1 event, 0 none, -1 unavailable; failed reads clear output. One observation.
// Publish/hide each consume one command. Guest must check event revision before
// applying edits. Host never writes guest state or changes an existing point.
__attribute__((import_module("crml_v1"), import_name("map_annotations_publish")))
int32_t crml_map_annotations_publish(const crml_map_annotations* frame,uint32_t size);
__attribute__((import_module("crml_v1"), import_name("map_annotations_publish_v2")))
int32_t crml_map_annotations_publish_v2(const crml_map_annotations_v2* frame,uint32_t size);
// dev.10: separate world annotations and six stock-marker attachments. The
// combined count is at most 128. Existing publish imports and ABI 1 are unchanged.
__attribute__((import_module("crml_v1"), import_name("map_annotations_publish_v3")))
int32_t crml_map_annotations_publish_v3(const crml_map_annotations_v3* frame,uint32_t size);
__attribute__((import_module("crml_v1"), import_name("map_annotations_next")))
int32_t crml_map_annotations_next(crml_map_annotation_event* event,uint32_t size);
// dev.10: read v3-owner events. A legacy reader facing a v3 queue, or this
// reader facing a legacy owner, returns -3 without consuming the queued event.
__attribute__((import_module("crml_v1"), import_name("map_annotations_next_v2")))
int32_t crml_map_annotations_next_v2(crml_map_annotation_event_v2* event,uint32_t size);
// dev.10: 1 for a fresh owned annotation lease, 0 for no own lease (zero
// output), -1 unavailable. requested/granted contain only NATIVE_MARKERS and
// PLACE_ACTION; CREATE remains the publishing guest's policy.
__attribute__((import_module("crml_v1"), import_name("map_annotations_status")))
int32_t crml_map_annotations_status(crml_map_annotation_status* status,uint32_t size);
__attribute__((import_module("crml_v1"), import_name("map_annotations_hide")))
int32_t crml_map_annotations_hide(void);
// Requires navigation.read; min_runtime=0.1.0-alpha.4.4.dev.1. One observation.
// 1: copied coherent position/optional movement-plane up, 0: stale/no foreground
// sample, -1: unavailable adapter. Failed observations zero the output; invalid
// memory/size traps. Current native adapter is reviewed-hotfix-only. No persistent
// coordinate/zone/campaign identity, complete teleport history or grounded flag.
__attribute__((import_module("crml_v1"), import_name("navigation_read")))
int32_t crml_navigation_read(crml_navigation_state* output, uint32_t size);
// Requires navigation.read; min_runtime=0.1.0-alpha.4.4.dev.3. Same fresh-only
// read and failure behavior as navigation_read. The 64-byte v2 state adds an
// observed continuity token; elapsed time alone does not change that token.
// It cannot prove that an unobserved world/entity was never reused.
__attribute__((import_module("crml_v1"), import_name("navigation_read_v2")))
int32_t crml_navigation_read_v2(crml_navigation_state_v2* output, uint32_t size);
// Development API: monotonic milliseconds since this Runtime was constructed.
// Sampled at the start of each guest invocation, including start/version/init/
// tick/shutdown; repeated reads in one invocation return the same value. It
// advances across host stalls, independently of clamped tick dt. Not game time,
// a wall-clock date, a save identity or a lease deadline. Compare differences
// within this Runtime only. No capability required; one observation per call.
// Requires min_runtime=0.1.0-alpha.4.3.dev.0. Never persist as an absolute time.
__attribute__((import_module("crml_v1"), import_name("clock_ms")))
uint64_t crml_clock_ms(void);
// Requests cleanup of this mod's native leases without unloading the mod.
// No ownership argument or extra capability. Physics restoration can be deferred
// to an engine callback. Automatic trap/unload cleanup remains in force.
// Alpha 4.2+; counts toward the eight-command allowance, including cleanup.
__attribute__((import_module("crml_v1"), import_name("release")))
void crml_release(void);
// Requires input.actions. Held-state bits correspond to manifest action.0..15.
// Configurable, read-only, foreground-only; no key consumption or injected input.
// Returns zero on focus loss/Escape or unavailable/stale input. Alpha 4.2+.
__attribute__((import_module("crml_v1"), import_name("input_actions")))
uint32_t crml_input_actions(void);
#include "crml_input.h"
// Development builds after Alpha 4.3; require input.actions. Rebind a slot using
// a manifest key token (or None), not a native key code. 1 changed, 0 unchanged,
// -1 unavailable, -3 invalid slot/name, -5 revision exhausted. Separate 16-call
// configuration allowance per invocation; no gameplay command cost.
// Names are 1..11 bytes without a terminator; invalid memory/length traps.
// No file persistence, input injection or suppression. Applies immediately.
__attribute__((import_module("crml_v1"), import_name("input_bind")))
int32_t crml_input_bind(uint32_t slot, const char* name, uint32_t length);
// Copied bindings, held-state bits and conflict/context information. sizeof(*out)
// required; 1 copied even without keyboard support, -1 unavailable (zero output).
// Counts as one observation. revision is not a key event or world/save identity.
__attribute__((import_module("crml_v1"), import_name("input_read")))
int32_t crml_input_read(crml_input_state* out, uint32_t size);
// Requires capabilities=log. UTF-8 bytes in guest memory, at most 4096 per call.
__attribute__((import_module("crml_v1"), import_name("log")))
void crml_log(int32_t level, const char* text, uint32_t length);
// Development API: storage capability; minimum runtime 0.1.0-alpha.4.3.dev.0.
// Installation/mod-ID scoped, NOT save/campaign scoped. One opaque record, 64KiB.
// Guest owns encoding/schema. Reads return committed byte count (including zero),
// -1 unavailable, -2 absent, -3 buffer too small, -5 I/O/corrupt file. On failure
// output is unchanged. A read during a write returns the previous committed data.
__attribute__((import_module("crml_v1"), import_name("storage_read")))
int32_t crml_storage_read(void* output, uint32_t capacity);
// Copies bytes immediately, then queues atomic replacement on a storage worker.
// 0 accepted, -1 unavailable, -4 pending/rate limited (one write per second),
// -5 host/resource failure at submission.
// Acceptance is NOT durability; poll storage_status. Zero bytes stores an empty
// record. Guest traps/unload do not cancel an already accepted write.
__attribute__((import_module("crml_v1"), import_name("storage_write")))
int32_t crml_storage_write(const void* bytes, uint32_t length);
// 0 no request, 1 pending, 2 committed, -1 unavailable, -5 write failed.
// Terminal status persists until the next accepted write; no read consumes it.
// All storage imports share a separate 16-call budget per guest callback.
// Invalid memory or length >65536 traps before touching storage.
__attribute__((import_module("crml_v1"), import_name("storage_status")))
int32_t crml_storage_status(void);
// Development API, requires settings. Register up to 32 definitions per loaded
// mod. Returns owner-local handle 1..32; -1 unavailable, -3 invalid definition,
// -4 duplicate key, -5 full/host failure. Invalid memory/size traps. Register during init.
__attribute__((import_module("crml_v1"), import_name("settings_register")))
int32_t crml_settings_register(const crml_setting_definition* definition, uint32_t size);
// Atomic snapshot of ALL settings, in handle order. Capacity is bytes, <=768 and
// a multiple of sizeof(crml_setting_value). Returns count, -1 unavailable or -3
// short buffer, -5 host failure; failures leave output unchanged.
// Separate 64-call settings budget.
__attribute__((import_module("crml_v1"), import_name("settings_read")))
int32_t crml_settings_read(crml_setting_value* output, uint32_t capacity);
// 1 changed, 0 unchanged, -1 unavailable, -2 unknown handle, -3 invalid value,
// -4 stale revision, -5 revision exhausted/host failure. Revision 0 means unconditional.
// Settings are live values; persist explicitly with storage when desired.
__attribute__((import_module("crml_v1"), import_name("settings_set")))
int32_t crml_settings_set(uint32_t handle, double value, uint64_t expected_revision);

// Text settings: minimum runtime 0.1.0-alpha.4.4.dev.1, same settings capability
// and 64-call budget. Shared 32-control/key/handle namespace with numeric settings.
// Version 1 definition; max_bytes 1..255. Single-line UTF-8, empty values allowed;
// NUL/control characters and invalid encodings rejected. Register returns handle.
__attribute__((import_module("crml_v1"), import_name("settings_text_register")))
int32_t crml_settings_text_register(const crml_text_setting_definition* definition, uint32_t size);
// 1 copied; -1 unavailable, -2 unknown handle, -3 wrong kind, -5 host failure.
// Output size must be sizeof(*output); failures leave output unchanged.
// settings_read also lists text handles/revisions with kind=TEXT and value=0.
__attribute__((import_module("crml_v1"), import_name("settings_text_read")))
int32_t crml_settings_text_read(uint32_t handle, crml_text_setting_value* output, uint32_t size);
// length is UTF-8 bytes, excludes NUL; same set results/revision policy as numeric.
// Values are copied; no automatic persistence or engine interpretation.
__attribute__((import_module("crml_v1"), import_name("settings_text_set")))
int32_t crml_settings_text_set(uint32_t handle, const char* value, uint32_t length, uint64_t expected_revision);

// Requires feedback. Copies 1..240 UTF-8 bytes; no ASCII control characters.
// severity: CRML_FEEDBACK_INFO..ERROR, duration 1000..10000 ms from acceptance.
// Returns a positive owner-scoped receipt, -1 renderer/service unavailable,
// -2 active message/capacity, -3 invalid content/arguments, -4 rate limited,
// -5 internal/resource failure. Acceptance does not prove presentation.
// One active message per mod, four globally; at most one publish per second.
__attribute__((import_module("crml_v1"), import_name("feedback_show")))
int64_t crml_feedback_show(const char* text, uint32_t length, uint32_t severity, uint32_t duration_ms);
// CRML_FEEDBACK_* receipt status; -1 owner unavailable, -2 stale/foreign ticket,
// -5 internal failure.
// Only the latest successful receipt is retained until unload. Presentation
// means the renderer acknowledged inserting text, not that the player saw it.
__attribute__((import_module("crml_v1"), import_name("feedback_status")))
int32_t crml_feedback_status(uint64_t ticket);
// 1 dismissed, 0 already terminal, negative as status. Fault/unload removes
// owned messages. All feedback calls share a separate 16-call callback budget.
// Invalid memory or length >240 traps before touching the service.
__attribute__((import_module("crml_v1"), import_name("feedback_dismiss")))
int32_t crml_feedback_dismiss(uint64_t ticket);

// Requires tutorials. Native hint, prompt and panel availability is independent.
// Copies UTF-8 title/body; no raw HTML. Hints and prompts last1000..30000ms;
// panels use duration0 and stock native Continue. A positive ticket means queued.
__attribute__((import_module("crml_v1"), import_name("tutorial_available")))
int32_t crml_tutorial_available(uint32_t kind);
__attribute__((import_module("crml_v1"), import_name("tutorial_show")))
int64_t crml_tutorial_show(uint32_t kind,const char* title,uint32_t title_length,
                          const char* body,uint32_t body_length,uint32_t duration_ms);
// Copies a versioned page with optional game image and bounded layout.
// Image loading is asynchronous; PRESENTED does not confirm image decoding.
__attribute__((import_module("crml_v1"), import_name("tutorial_present")))
int64_t crml_tutorial_present(const crml_tutorial_page* page,uint32_t page_size);
__attribute__((import_module("crml_v1"), import_name("tutorial_status")))
int32_t crml_tutorial_status(uint64_t ticket);
// Cancellation may remain CANCELLING until native presentation retires.
__attribute__((import_module("crml_v1"), import_name("tutorial_dismiss")))
int32_t crml_tutorial_dismiss(uint64_t ticket);
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
// Requires player.visibility and runtime >=0.1.0-alpha.4.4.dev.0. One observation.
// Copies this owner's current lease and evidence flags, no acknowledgement or renewal.
// Returns 1 snapshot (including IDLE), -1 unsupported with the output zeroed.
// SUBMITTED records command publication, not completed rendering. Evidence is
// retained only for the current lease, not across release/reacquisition.
__attribute__((import_module("crml_v1"), import_name("visibility_read")))
int32_t crml_visibility_read(crml_visibility_state* out,uint32_t out_len);
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
// Named CRML_PHYSICS_STATUS_* values are defined in crml_results.h.
__attribute__((import_module("crml_v1"), import_name("physics_status")))
int32_t crml_physics_status(void);
// Requests restoration/cancellation and releases ownership after cleanup.
__attribute__((import_module("crml_v1"), import_name("physics_restore")))
int32_t crml_physics_restore(void);
// Copied callback snapshots, never live engine memory. Pass sizeof(*out).
// 1 copied, 0 not ready/stale observation, -1 unavailable. Failure zeros output.
// Each call consumes one of eight observations per callback. Alpha 4.2+.
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
// negative status; all failures zero output. Counts toward the eight-observation
// allowance. Generation identifies this observed screen instance, not an address.
__attribute__((import_module("crml_v1"), import_name("ui_read")))
int32_t crml_ui_read(crml_ui_state* out, uint32_t size);
// Requires ui.activate. Use a nonzero generation from ui_read and a supported
// CRML_UI_ACTION_* value. Unsupported arguments trap; stale/unavailable requests
// return a negative status. Zero means queued, never confirmed completion.
// Requires the action's bit in the current screen's advertised actions mask.
// Counts toward the eight-command allowance; unload cancels this mod's requests.
__attribute__((import_module("crml_v1"), import_name("ui_activate")))
int32_t crml_ui_activate(uint64_t generation, uint32_t action);

// Development API, min_runtime=0.1.0-alpha.4.3.dev.0; ui.activate permission.
// Same bounded actions as ui_activate, with a positive owner-scoped receipt.
// -1 unavailable/stale observation/lock busy, -2 pending command, -3 stale
// generation or action unavailable, -5 exhausted IDs. Invalid arguments trap.
// Counts as a command. No new engine action or transition guarantee is added.
__attribute__((import_module("crml_v1"), import_name("ui_action_submit")))
int64_t crml_ui_action_submit(uint64_t generation, uint32_t action);
// One observation call. CRML_UI_ACTION_* status, -1 provider/lock unavailable,
// -2 unknown, evicted or foreign receipt. Most recent 64 tracked commands are
// retained globally, including after release. No pointers/save IDs in receipts.
// DISPATCHED means the renderer trigger returned, not that the engine advanced.
// OUTCOME_UNKNOWN may resolve on a late same-page renderer report. Never retry
// a delivered command merely because its outcome is unknown.
__attribute__((import_module("crml_v1"), import_name("ui_action_status")))
int32_t crml_ui_action_status(uint64_t receipt);

// Requires ui.presentation. A temporary visibility override for one uniquely
// matched native UI element by ID or CLASS, never a selector or script. Name is
// 1..64 ASCII letters/digits/underscore/hyphen, without a NUL terminator in length.
// hidden=1 leases visibility:hidden for 1..1000 ms; hidden=0 requires duration=0
// and removes this owner's override. Generation must identify the current view.
// Eight named leases are shared globally. 0 means queued, -1 unavailable/stale,
// -2 busy/full. Requests share the eight-command allowance. Expiry, screen/page changes
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
// A zero generation traps. Native readiness checks remain in force. Skip counts
// as a command; media_read is an observation. Unload cancels owned requests.
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
// Requires player.motion, minimum runtime 0.1.0-alpha.4.4.dev.0.
// Owner-local lease/cancellation snapshot; one observation, no acknowledgement.
// Returns 1 snapshot (including IDLE), -1 unsupported (all bytes zeroed).
// Read after a failed renewal and before explicit release for its stop reason.
// Does not expose another mod's state or establish applied engine movement.
__attribute__((import_module("crml_v1"), import_name("motion_read")))
int32_t crml_motion_read(crml_motion_state* out, uint32_t out_len);
CRML_EXPORT("crml_abi_version") uint32_t crml_abi_version(void);
CRML_EXPORT("crml_init") void crml_init(void);
// Optional; worker heartbeat, NOT a game frame or game-thread callback.
CRML_EXPORT("crml_tick") void crml_tick(float elapsed_seconds);
CRML_EXPORT("crml_shutdown") void crml_shutdown(void);
#ifdef __cplusplus
}
#endif

#include "crml_helpers.h"
