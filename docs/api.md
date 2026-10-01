---
title: Wasm mod API and package format
description: Reference for CONTROL Resonant Wasm mod manifests, lifecycle functions, host imports, capabilities, and runtime limits.
---

# Package format and API

## Manifest

`mod.ini` is a strict UTF-8/ASCII-compatible `key=value` file without sections. Blank lines and lines beginning with `#` are ignored. Duplicate and unknown fields are errors.

```ini
id=hello
abi=1
module=hello.wasm
capabilities=log
```

| Field | Contract |
| --- | --- |
| `id` | Required, 1–64 lowercase letters, digits, `_` or `-`; unique within the host |
| `abi` | Required, exactly `1` |
| `module` | Required, local `.wasm` filename; no directories or absolute paths |
| `capabilities` | Empty/omitted, or a comma-separated list of `log`, `input.actions`, `input.buttons`, `input.motion`, `player.noclip`, `player.visibility`, `player.motion`, `player.read`, `camera.read`, `physics.damping`, `ui.read`, `ui.activate`, `ui.presentation`, `media.read`, and `media.skip`; duplicates and unknown requests are rejected |
| `action.0` through `action.15` | Optional named key bindings for `input.actions`; omitted slots and `None` are unbound. Requires Alpha 4.2 or later. |

Logging is available when requested. Engine operations also require native build support, executable compatibility checks (and an installation opt-in on older releases); declaring a capability cannot bypass those gates. Existing ABI 1 packages remain supported. New imports and action fields require Alpha 4.2 or later and are rejected by older runtimes.

From Alpha 4.2, normal startup prepares movement, visibility, physics and read-only player/camera services from installed mods' valid manifests. They can run together with independent ownership; no gameplay-mode marker is needed. Diagnostic observer/trial modes still suspend mods. A service that fails its native compatibility checks reports unavailable without disabling other services.

## Guest exports

| Export | Wasm signature | Required |
| --- | --- | --- |
| `crml_abi_version` | `() -> i32`, returns `1` | Yes |
| `crml_init` | `() -> ()` | Yes |
| `crml_tick` | `(f32 elapsed_seconds) -> ()` | No |
| `crml_shutdown` | `() -> ()` | No |
| `memory` | wasm32 linear memory | For logging and imports that copy output |

All callbacks run serially on a runtime worker. In-game ticks are approximately 100 ms apart, or 10 ms once action input is requested or the isolated physics/movement service is active. Elapsed time is capped at one second. These are not timing guarantees, render callbacks or game-thread scheduling guarantees.

## Host imports

### Capability availability and cleanup

`crml_v1.capabilities() -> i32` returns the intersection of this mod's **declared permissions** and currently installed services. It requires no additional permission and cannot grant access to imports. Constants are defined in `sdk/include/crml_abi.h`:

| Bit | SDK constant | Permission |
| --- | --- | --- |
| 0 | `CRML_CAP_LOG` | `log` |
| 1 | `CRML_CAP_INPUT_BUTTONS` | `input.buttons` |
| 2 | `CRML_CAP_PLAYER_NOCLIP` | `player.noclip` (legacy helper) |
| 3 | `CRML_CAP_PLAYER_VISIBILITY` | `player.visibility` |
| 4 | `CRML_CAP_PHYSICS_DAMPING` | `physics.damping` |
| 5 | `CRML_CAP_INPUT_MOTION` | `input.motion` (fixed controls) |
| 6 | `CRML_CAP_PLAYER_MOTION` | `player.motion` (noncolliding movement) |
| 7 | `CRML_CAP_INPUT_ACTIONS` | `input.actions` |
| 8 | `CRML_CAP_PLAYER_READ` | `player.read` |
| 9 | `CRML_CAP_CAMERA_READ` | `camera.read` |
| 10 | `CRML_CAP_UI_READ` | `ui.read` |
| 11 | `CRML_CAP_UI_ACTIVATE` | `ui.activate` |
| 12 | `CRML_CAP_MEDIA_READ` | `media.read` |
| 13 | `CRML_CAP_MEDIA_SKIP` | `media.skip` |
| 14 | `CRML_CAP_UI_PRESENTATION` | `ui.presentation` |

Availability describes a service, not a live player, a selected prop, successful execution or ownership of a lease. Continue checking operation results. The standalone host reports only declared logging support; it has no game or keyboard service. These bits do not imply colliding flight, free-camera ownership or general entity spawning.

`crml_v1.release() -> ()` requests cleanup of **this mod's** native leases without unloading the mod. It takes no owner ID and cannot release another mod's leases. Physics restoration can remain pending until an engine callback can safely perform it. Automatic cleanup on traps, shutdown and destruction remains in force, even if the guest never calls this function.

Both imports are available from Alpha 4.2 and count toward the shared limit of eight input and gameplay calls per lifecycle invocation. These are one combined budget, not eight calls per subsystem. Logging has its own limits.

### Media observation and skipping

The experimental `media.read` and `media.skip` permissions expose a copied playback snapshot and an owner-scoped skip request. The current native adapter covers the reviewed startup video only; it does not enumerate all videos or control in-game cinematics. Engine compatibility and the original startup readiness checks remain required.

`crml_v1.media_read(i32 output, i32 size) -> i32` requires `media.read` and exactly 288 bytes. The layout in `crml_media.h` is `u32 size/version/flags/elapsed_ms` at offsets 0/4/8/12, `u64 generation` at 16, `u32 name_length/reserved` at 24/28, and a 256-byte NUL-terminated name at 32. `1` means copied; non-positive means unavailable and clears output. Samples expire after 250 ms. Invalid size or memory traps before native access.

Flags are `CRML_MEDIA_ACTIVE` (1), `CRML_MEDIA_SKIPPABLE` (2), `CRML_MEDIA_ENGINE_NAME` (4) and `CRML_MEDIA_MAPPED_NAME` (8). The last two distinguish a captured engine asset name from a name associated by the reviewed adapter. This adapter reads the original logical boot-resource name from engine metadata and marks it **mapped**; it does not expose installation paths or claim to have captured a start call that happened before the runtime loaded. Names are identifiers, not localized display labels.

`crml_v1.media_skip(i64 generation) -> i32` requires `media.skip`. Zero generation traps; `0` means queued and a negative result means rejected/unavailable. The current adapter accepts one pending request, expires it after one second, and rechecks identity and readiness on the engine thread. It consumes a skip only at the original initialization-gated loop exit with playback time strictly greater than 2,000 ms. Original destruction and renderer cleanup run normally. Neither copied pointers nor raw timer writes are available to guests.

Both imports share the eight-call budget. `release()` and automatic trap/unload cleanup cancel pending requests; a completed skip cannot be undone. The startup-skip example chooses the boot asset name and retry policy in Wasm. Native observation can miss this one-time video if the runtime starts too late; unavailable support leaves playback unchanged. See the [startup control-flow map](research/startup-media-map.json).

### Startup UI state and actions

These Alpha 4.2 imports provide a bounded interface to recognized startup screens. They do not grant arbitrary HTML, JavaScript, engine events or settings-page creation. Include `crml.h`; `crml_ui.h` defines the layout and constants. The native adapter requires the reviewed executable, UI middleware and original UI document. In-game startup behavior on this adapter is experimental.

| Import in `crml_v1` | Wasm signature | Permission | Result |
| --- | --- | --- | --- |
| `ui_read` | `(i32 output, i32 size) -> i32` | `ui.read` | `1` copied; non-positive unavailable |
| `ui_activate` | `(i64 generation, i32 action) -> i32` | `ui.activate` | `0` queued; negative rejected/unavailable |
| `ui_present` | `(i64 generation, i32 kind, i32 name, i32 name_length, i32 hidden, i32 duration_ms) -> i32` | `ui.presentation` | `0` queued; `-1` unavailable/stale; `-2` occupied/full |

`ui_read` requires exactly 32 output bytes. The snapshot has `u32 size`, `u32 version`, `u32 screen`, `u32 actions`, `u64 generation`, `u32 age_ms` and `u32 reserved`, at offsets 0, 4, 8, 12, 16, 24 and 28. Successful samples have version 1 and age at most 1,000 ms. Non-success clears the output; invalid memory or layout size traps before native access. Generation values are opaque 64-bit identities, not addresses.

| Screen value | SDK suffix after `CRML_UI_SCREEN_` | Continue supported |
| --- | --- | --- |
| 0 | `NONE` | No |
| 1 | `PHOTOSENSITIVITY` | Yes |
| 2 | `LEGAL` | No |
| 3 | `EULA` | No |
| 4 | `PRIVACY` | No |
| 5 | `SAVE_WARNING` | Yes |
| 6 | `DISPLAY_CALIBRATION` | No |
| 7 | `FIRST_TIME_SETUP` | No |
| 8 | `USER_INTERACTION` | Yes |
| 9 | `MAIN_MENU` | No |

Action `CRML_UI_ACTION_CONTINUE` is 1; its availability bit is `CRML_UI_ACTION_MASK_CONTINUE` (also 1). Only request advertised actions using the current generation. A generation changes when the observed screen or allowed actions change, or a new UI document publishes its first sample. There is one pending action across mods, with a two-second expiry. Calls share the eight-call lifecycle budget. Zero generation and unsupported action values trap.

Acceptance means queued, not that the game advanced. The trusted UI bridge rechecks readiness and screen identity before sending the game's ordinary Continue event, at most once per observed screen visit. A loading/readiness change does not rearm that event. Read state again to observe the result. `release()`, trap cleanup and unload cancel this mod's pending action; an event already delivered to the engine cannot be recalled. The bridge does not automatically accept consent screens or interrupt saving.

`ui_present` temporarily hides a uniquely matched native UI element while preserving layout, bindings and engine work. `kind` is `CRML_UI_TARGET_ID` (1) or `CRML_UI_TARGET_CLASS` (2). Supply an exact engine DOM ID or single class token as 1–64 ASCII letters, digits, underscores or hyphens; `name_length` excludes a terminator. This is not a CSS selector, script, resource path or screen enumeration API. The copied screen values above remain the adapter's fixed mapping.

With `hidden=1`, choose a lease duration of 1–1,000 ms and renew while needed. `hidden=0` requires duration 0 and removes that mod's lease. A nonzero current snapshot generation is required. Invalid arguments or guest memory trap before native access. Up to eight named leases are shared across mods; another owner cannot replace a lease for the same kind/name. All three imports share the eight-call budget. A successful queue result does not confirm that the element exists or was hidden.

The UI bridge applies fresh requests only to one attached matching element. Missing or ambiguous targets, two names resolving to the same element, and inline `visibility: … !important` overrides are left unchanged. Lease expiry, screen/page changes and owner cleanup remove the override. Readiness-only generation changes can retain a lease on the same screen. Engine style changes are preserved rather than overwritten. Cleanup reaches the UI through polling; deadlines are checked on its 250 ms poll, including while a request is pending. No synchronous render-thread deadline is promised.

The startup example targets the engine's `splash` class during selected notices. Suppressing that presentation does not declare preferences loaded, bypass save-header reads, dismiss consent or remove UI nodes. Startup may remain blank while required engine initialization finishes; the boot-video gate and native loading time still apply.

Presentation and Continue requests are handled independently. A renderer error applying a visibility request does not prevent an otherwise valid Continue action from being dispatched.

`examples/startup-skip` implements screen selection, stability delay and bounded retries in Wasm, with matching C/WAT sources. It is distributed separately from the runtime; installing the runtime alone does not skip screens.

### Configurable input actions

`crml_v1.input_actions() -> i32` requires `input.actions`. Bits 0–15 are held states for that mod's `action.0`–`action.15` bindings. Other bits are zero. New mods can use this instead of the fixed `input_buttons` and `input_motion` layouts, which remain available for existing packages.

```ini
id=my-mod
abi=1
module=my-mod.wasm
capabilities=log,input.actions
action.0=F10
action.1=Insert
action.2=None
```

Supported names are case-sensitive: `F1`–`F12`, `Insert`, `Home`, `End`, `PageUp`, `PageDown`, `W`, `A`, `S`, `D`, `Q`, `E`, `R`, `Space`, `Ctrl`, `Shift`, `Up`, `Down`, `Left`, `Right`, and `None`. Numeric key codes and other names are rejected. Bindings are read when the mod loads; restart after editing them. Duplicate fields and indices outside 0–15 are rejected; assigning the same key to multiple actions is allowed.

The release runtime supplies this read-only service without a gameplay-mode marker. It does not enable movement, visibility or physics. It returns zero while the game lacks focus, Escape is held, the keyboard snapshot is stale, or the service is unavailable. Reading actions does not consume or suppress another mod's input. Shared bindings can still trigger both mods. This API provides no text input, system-wide keyboard access or input injection.

This is a snapshot, not an event queue: very brief presses can be missed. Derive press edges in guest code with `current & ~previous`; choose an explicit policy for keys held on startup or when returning to the game. Calls share the eight-call budget above. The SDK's `examples/input-actions` includes C and WAT implementations with no gameplay dependency.

### Player, camera and physics snapshots

These Alpha 4.2 imports copy a bounded snapshot into exported guest memory. Include `crml.h` and pass `sizeof(*out)` from C; `sdk/include/crml_state.h` defines the fixed-width layouts. Offsets below are bytes, and scalar values use Wasm's little-endian representation.

| Import in `crml_v1` | Wasm signature | Capability | Output |
| --- | --- | --- | --- |
| `player_read` | `(i32 output, i32 size) -> i32` | `player.read` | `crml_player_state`, 32 bytes |
| `camera_read` | `(i32 output, i32 size) -> i32` | `camera.read` | `crml_camera_state`, 80 bytes |
| `physics_read` | `(i64 token, i32 output, i32 size) -> i32` | `physics.damping` | `crml_physics_state`, 32 bytes |

Each call consumes one of the shared **eight input and gameplay calls per lifecycle invocation**. A size other than the exact layout size, missing or invalid exported memory, or an output range outside guest memory traps before native access. Unaligned output is supported. For a valid call, `1` means copied, `0` means no fresh readable snapshot, and `-1` means unavailable. Every non-success result clears the entire output structure; argument-validation traps do not promise an output write. `physics_read` can also return `-2` for contention or another owner, and `-3` for an invalid or stale selection token.

Successful snapshots have `version = 1` and `age_ms <= 500`. Reads do not renew a movement or physics lease. Player, camera and physics data come from separate engine callbacks and **do not represent a synchronized frame**, even when read in the same Wasm callback. Handle temporary unavailability and check flags before using optional fields.

#### Player state

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | `version` | `u32` | `1` on success |
| 4 | `age_ms` | `u32` | Age of the copied observation |
| 8 | `generation` | `u64` | Opaque local observation identity |
| 16 | `position[3]` | `f32[3]` | World-space X, Y, Z before the controller update |
| 28 | `reserved` | `u32` | Zero |

`player.read` runs independently of movement, visibility, input suppression and overlays. It returns `0` while the game lacks focus or its sample is unavailable, invalid or stale. Position values are finite. Generation changes when the observed player/world changes, after invalidation, or after an observation gap over 500 ms. It is not an engine entity ID, pointer, persistent identifier or permission to modify the player. Use generation changes to reset mod-owned position histories.

#### Camera state

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | `version` | `u32` | `1` on success |
| 4 | `age_ms` | `u32` | Age of the copied observation |
| 8 | `generation` | `u64` | Opaque local selected-camera identity |
| 16 | `mode` | `i32` | Engine camera selector: `0` player, `1` or `2` free-camera slot, `3` special slot |
| 20 | `flags` | `u32` | Valid field groups described below |
| 24 | `position[3]` | `f32[3]` | World-space position |
| 36 | `basis[9]` | `f32[9]` | Three consecutive, approximately orthonormal vectors in `CameraView` order |
| 72 | `horizontal_fov_radians` | `f32` | Selected view's horizontal field of view, in radians |
| 76 | `aspect_ratio` | `f32` | Selected view's width/height ratio |

`CRML_CAMERA_STATE_POSE` (`1`) marks valid position and basis. `CRML_CAMERA_STATE_LENS` (`2`) marks valid FOV and aspect ratio; absent lens fields are zero. The basis is not an Euler-angle triple. Generation tracks observed world, camera environment, selected entity and mode changes; it can also change when a previously invalid observation becomes readable. It is local to the runtime and exposes no engine handle.

This service copies the selected `CameraView` after the engine's camera-update call. It is not focus-gated, but an interrupted or stale update produces no snapshot. Special rendering paths, including `CameraManView`, can override the selected view before presentation, so these values do not necessarily describe the final rendered image. Reading a free-camera slot does not activate it. This API grants no camera ownership, position/rotation/FOV writes or free-camera controls.

#### Physics state

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | `version` | `u32` | `1` on success |
| 4 | `age_ms` | `u32` | Age of the copied observation |
| 8 | `flags` | `u32` | Valid field groups described below |
| 12 | `reserved` | `u32` | Zero |
| 16 | `linear_damping` | `f32` | Observed linear damping coefficient |
| 20 | `angular_damping` | `f32` | Observed angular damping coefficient |
| 24 | `linear_speed` | `f32` | Magnitude of the body's linear velocity |
| 28 | `angular_speed` | `f32` | Magnitude of the body's angular velocity |

`CRML_PHYSICS_STATE_DAMPING` (`1`) marks both damping fields; `CRML_PHYSICS_STATE_SPEED` (`2`) marks both speed fields. Unavailable groups contain zero. Neither speed is a velocity vector. These are sampled physics values, not a guarantee of visible motion during the guest callback.

Pass the token returned by `physics_target` for **this mod's** selected body. Retain it while an operation is active; `physics_target` only returns an idle selected target. The host supplies the caller's ownership identity. A token borrowed from another mod grants no access. Focus loss or Escape returns `-1`; cleanup, pending commands, retirement, scene changes and stale samples can also prevent a read. Reading angular damping or speed does not grant write access to those properties. The existing write API still supports only temporary linear damping on one selected body.

### Logging

`crml_v1.log(level: i32, offset: i32, length: i32) -> ()`

Requires `log`. `offset` and `length` describe guest memory, never a native address. Level must be 0–3 (debug/info/warning/error); the current plain-text sink uses the same format for each level. Text should be UTF-8. Control characters are replaced with spaces to keep log entries on one line. Invalid byte sequences are not transcoded.

One call accepts at most 4,096 bytes. Each lifecycle invocation accepts at most 32 calls and 16,384 bytes. Invalid ranges and exhausted logging budgets trap the guest.

### Experimental noclip

`crml_v1.noclip_poll(speed: f32) -> i32`

Requires `player.noclip`. Call on each worker heartbeat to keep the mod's native movement lease alive and service the fixed F6 keyboard toggle. Finite speeds from 0.25 through 20 world units per second are accepted; Shift multiplies movement speed by three. More than eight calls per lifecycle invocation, nonfinite values, and out-of-range speeds trap the guest.

Returns `1` when enabled, `0` when off, `-1` when unavailable, or `-2` when another mod owns the override. The standalone host returns `-1`. Native cleanup releases ownership on load failure, traps, shutdown, and runtime destruction, independently of a guest shutdown export. Guests cannot provide addresses, native callbacks, arbitrary key codes, or entity IDs.

The host handles movement keys and the status panel; this is not a general UI or keyboard API. While the lease is active, it consumes the fixed noclip keyboard controls, suppresses player fall/boundary recovery and fall monitoring, and neutralizes script-visible fall values. Once a flight position is acquired, same-player/world teleports restore that target instead of cancelling flight. This also affects intentional travel; disable noclip first. These overrides end when the lease is released or expires. See the [test guide and limitations](gameplay.md).

## Bounded button input

`crml_v1.input_buttons() -> i32` requires `input.buttons`. Bit 0 is F7, bit 1 is F8; other bits are zero. Returns zero while the game lacks focus, Escape is held, or no input service exists. No arbitrary key codes or text input are exposed. Polling runs on the worker heartbeat, so a press shorter than that interval can be missed.

## Experimental player visibility

`crml_v1.visibility_set(hidden: i32) -> i32` requires `player.visibility` and native gameplay support (before Alpha 4.2, install with `--experimental-visibility`). Only 0 and 1 are accepted: 1 renews a 500 ms hide lease; 0 releases it. The **guest decides** when to request hiding, using button input, a timer, or other guest logic. It does not need the input capability if it does not read buttons.

The native runtime validates the request, enforces focus/Escape cancellation and lease expiry, then submits the renderer command on the engine's mesh update phase. It releases ownership on mod failure and shutdown. The host accepts at most eight calls per lifecycle invocation shared across input, gameplay and UI imports.

Returns `1` for a renewed lease, `0` for release, `-1` when unavailable, and `-2` when another mod owns the lease. This is request status, not confirmation of a rendered result. The bridge targets only a fresh, generation-checked player entity in the mesh visibility query. Guests cannot supply a pointer, render handle, entity ID, or renderer opcode. Normal engine visibility resumes on the next eligible update after release or cancellation.

The legacy `visibility_poll()` import remains available for older mods and combines native F7 polling with a visibility lease. New mods should use `visibility_set()` instead.

See [the visibility example](visibility.md) for installation and controls.

## Experimental prop damping

Requires `physics.damping` and native gameplay support. Alpha 4.2 starts it from mod manifests alongside other services; earlier versions require isolated `--physics-wasm` mode. This service supports one nearby eligible prop and one mod owner at a time. It reuses the native [physics trial's selection and property checks](physics-trial.md), including player/attachment exclusions, full body generations, executable/backend fingerprints, native accessor checks and readback. Other installed mods can independently request movement or visibility.

| Import in `crml_v1` | Wasm signature | Result |
| --- | --- | --- |
| `physics_select_near` | `(f32 offset_x, f32 offset_y, f32 offset_z, f32 radius) -> i32` | Queue a search in a mod-chosen sphere relative to the player |
| `physics_select` | `() -> i32` | Compatibility helper for `physics_select_near(0, 0, 0, 2)` |
| `physics_target` | `() -> i64` | Current owner-scoped selection token, or zero |
| `physics_read` | `(i64 token, i32 output, i32 size) -> i32` | Copy the selected body's [physics snapshot](#physics-state) |
| `physics_apply` | `(i64 token, f32 damping, i32 duration_ms) -> i32` | Queue temporary linear damping |
| `physics_status` | `() -> i32` | Current operation state |
| `physics_restore` | `() -> i32` | Request restoration/cancellation and release ownership |

`physics_select_near` requires Alpha 4.2. Its center is the player's position at search start plus the supplied **world-aligned** offset; it does not rotate with the camera or follow subsequent movement. Offset length is bounded to 20 world units; radius must be greater than zero and at most 20. All components must be finite. Invalid arguments trap before a native request is queued. The search picks the eligible entity origin nearest the requested center. It rejects near ties within 0.1 world units and excludes the player, controllers, attachments, multi-body instances and unsupported storage. It is not a raycast, mesh intersection or arbitrary entity query. Search work and lifetime checks apply regardless of radius.

The mod owns the search region, input bindings, timing, damping value and duration. For example, `crml_physics_select_near(1, 0, 0, 4)` searches a sphere offset one world unit along X, and `crml_physics_apply(token, 0.25f, 1200)` requests low linear damping for 1.2 seconds. Native code resolves identities, schedules accesses on the physics callback and restores the original value. Queries and status getters take no owner argument because ownership comes from the calling mod. The service currently supports one selected body and linear damping; it does not expose mass, friction, forces or arbitrary physics fields.

Command results are `0` accepted (or already idle for restore), `-1` unavailable, `-2` busy or another owner, and `-3` invalid/stale target or arguments. Acceptance is not execution: the engine callback consumes requests. Requests older than 500 ms are not applied. The guest must wait for selection to complete before obtaining a token and applying a value. A token is neither an engine address nor an entity/body ID; passing another mod's token does not grant access.

`physics_status` returns `0` idle, `1` queued, `2` searching, `3` selected, `4` active, `5` restoring, `6` finished, `7` retired, `8` conflicting game change, or `9` refused. It can return `-1` unavailable or `-2` busy. Finished includes a value already equal to the request; it does not by itself prove a write occurred. Terminal status is transient and returns to idle when the selection is cleared. A failed search also returns to idle, with its reason shown in the panel and diagnostic log.

The import accepts finite damping from **0 through 8**, for **1 through 5,000 ms**. Nonfinite/out-of-range values or exceeding the shared eight-call host budget trap the mod. Native code independently checks these limits. Unused selection expires after 15 seconds; completing a trial consumes its token. A new application requires a fresh selection. Damping affects linear velocity decay, not friction, mass or angular damping.

Focus loss, Escape, F11, a missed worker heartbeat, mod failure, shutdown and runtime destruction request cleanup. Cleanup is serviced on the physics callback, including after the guest has stopped. A game-written conflicting value is preserved; retired bodies are not written through stale handles. Temporary unavailability keeps restoration pending. From Alpha 4.2, Wasm physics continues after ten minutes and after its bounded diagnostic log fills or fails. The five-second operation limit, heartbeat checks and restoration rules remain. Native diagnostic trials retain their finite session limits.

See the [Wasm damping example](physics-trial.md#wasm-damping-example) for setup and expected behavior.

## Experimental movement requests

The `player.motion` service exposes explicit character movement commands. Alpha 4.2 starts it from manifest requests; earlier versions need `--movement-wasm`. The [movement example](movement.md) implements the toggle and camera-relative controls in its own guest source. It can run alongside visibility and physics. When explicit movement and the legacy `player.noclip` helper are both requested, explicit movement takes precedence and the legacy helper reports unavailable; they do not compete for the controller.

`crml_v1.input_motion() -> i32` requires **`input.motion`**. Bits 0–7 represent **F6, W, S, A, D, Space, Ctrl, Shift**, respectively. All other bits are zero. The result is zero without focus, while Escape is held, or when the service is unavailable. No text input or arbitrary key-code API is exposed.

`crml_v1.motion_camera(output: i32) -> i32` requires **`player.motion`**. It copies two `f32` values into eight bytes of exported guest memory: the normalized horizontal right vector's X and Z components. Return `1` means a validated camera sample no older than 100 ms; `-1` means unavailable and zeroes both outputs. Invalid guest-memory ranges trap the mod before native access. The vector is a snapshot, not camera ownership; guest forward can be computed as `(-right_z, 0, right_x)`.

`crml_v1.motion_set(enabled: i32, x: f32, y: f32, z: f32) -> i32` requires **`player.motion`**. With `enabled = 1`, it submits a world-space velocity and renews an owner-bound, 500 ms noncolliding character-motion lease. The vector must be finite and its magnitude at most **20 world units per second**. Zero velocity holds position. With `enabled = 0`, it releases this mod's lease. Invalid enable values, nonfinite velocities, excess speed and exceeding the shared eight-call host budget trap the mod.

Results are `1` accepted, `0` released/already off, `-1` unavailable, or `-2` another owner. Acceptance is not proof of a controller update. The native callback integrates the velocity using a maximum 50 ms step and validates current player/world identity and controller arguments. Normal WASD, Space, Ctrl and Shift keyboard actions are suppressed while the lease is active; mouse look remains under game control.

Escape, focus loss, stale input samples, an expired lease, player/world replacement, disabled/keyframed controllers, large unexplained displacement and engine teleports cancel movement. A controller gap over 250 ms also cancels. A pending guest cancellation is reported on its next renewal so the example can switch off instead of silently restarting. Mod traps and shutdown release ownership independently of guest cleanup. On supported builds, the [boundary guard](fall-recovery.md#flight-boundary-guard) prevents selected recovery entry points while the movement lease is active; pending recovery and unmatched producers continue normally. This is native service policy, not guest access to Lua or arbitrary script hooks. The service does not restore pre-flight position, override engine teleports or provide free-camera control.
