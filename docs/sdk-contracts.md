---
description: Capability scope, coordinates, time, identities, outcomes and evidence for the Wasm SDK.
---

# SDK contracts and evidence

Use this reference to decide whether an API can support a mod's behavior. The
[API reference](api.md) gives import signatures and layouts; the headers shipped
with the SDK are the corresponding C declarations. A compatible ABI preserves
calling conventions. It does not establish that every native operation works on
every executable, or that a successful request has taken effect in the game.

## Availability and scope

`capabilities()` returns the intersection of the package's declared permissions
and installed services. A set bit does not establish a fresh observation, an
eligible target, ownership, menu state, successful execution or restoration.
Installation of one family does not grant permission to another package.

| Permission / imports | Implemented scope | Boundary for mod authors |
| --- | --- | --- |
| `input.actions`, `input.buttons`, `input.motion` | Bounded held-key observations; configurable action slots and copied conflicts in development builds | No text input, injection, exclusive key ownership or general gameplay/menu context |
| `player.read` / `player_read` | Copied position from an authenticated player controller callback | No health, progression, save identity or event stream |
| `navigation.read` / `navigation_read`, `navigation_read_v2` | Position and optional movement-plane up from the same authenticated callback; transient publication sequence and sampled discontinuity flags. V2 adds observed continuity separate from sample freshness | Reviewed hotfix only; no persistent coordinate/save/zone identity, contact normal, semantic simulation time, complete teleport history or proof against unobserved identity reuse |
| `camera.read` / `camera_read` | Copied selected engine camera pose and optional lens | No camera writes, ownership, free-camera activation or guarantee of final rendered view |
| `player.motion` / `motion_set` | Temporary world-velocity request through a noncolliding player controller override | This name does not imply ordinary colliding movement, arbitrary teleportation or general character control |
| `player.action_rules` / `action_rule_set`, `action_rule_read` | Owner-scoped 500ms area-restriction exceptions for explicit action masks | Build-specific adapter; native instruction fixtures, full gameplay pending. Request acceptance is not action execution; other restrictions remain. See [action rules](action-rules.md) |
| `player.noclip` / `noclip_poll` | Legacy native F6 toggle, motion policy and recovery behavior | Kept for ABI compatibility; use `motion_set` for guest-controlled movement policy; the two implementations have different teleport/recovery behavior |
| `player.visibility` / `visibility_set` | Temporary player mesh hide request | No arbitrary entity visibility, renderer access or confirmation that a frame was hidden; `visibility_poll` additionally polls the legacy F7 key |
| `physics.damping` / `physics_*` | One nearby eligible prop, copied damping/speed observations and temporary linear damping | No raycast, target name/position, impulse, collision policy, angular damping write, arbitrary body ID or general physics API |
| `ui.read`, `ui.activate` | Reviewed startup screen mapping and eligible Continue action | No general UI discovery, consent acceptance or guaranteed navigation completion |
| `ui.presentation` / `ui_present` | Temporary hiding of an exact ID or class token resolving to one native element | No CSS selectors, script injection or engine-fact changes; a queued request can fail to find a unique element |
| `media.read`, `media.skip` | Reviewed startup media adapters | No general cutscene, audio or video player API; a mapped asset name is not an observed instance name |
| `storage`, `settings`, `feedback` | Bounded mod-owned records, live typed values, passive messages and receipts | Storage is installation-scoped; settings need explicit persistence; renderer acknowledgement does not prove a person saw a message |
| `drawing` / `drawing_publish`, `drawing_hide` | Bounded whole schematic frames in the native UI document | Admission is not presentation; hide clears host state immediately and pixels on a later renderer update; no world projection, occlusion or map access. See [drawing](mod-drawing.md). |
| `drawing` / `map_annotations_publish_v3`, `map_annotations_next_v2`, `map_annotations_status` | Up to 128 world annotations and six separately typed attachments to existing native X markers, with owner-local edits and interaction grants | Native attachment coordinates are map-local pixels; status reports only the current annotation lease and native-marker/placement grants. Neither a slot nor map geometry supplies campaign identity. See [drawing](mod-drawing.md#separate-world-annotations-and-native-attachments). |
| `lists` / `list_publish`, `list_next`, `list_hide` | Copied pages of up to 32 stable-ID rows, font scale and a 16-event owner-local activation FIFO inside Options | Guest owns search/pagination/content meaning; next destructively consumes an action, not an engine result. Accepted old-revision events survive content replacement. See [lists](mod-lists.md). |
| `tutorials` | Experimental timed native hints and dismissible native panels with private page data | Requires a reviewed executable; no campaign progression writes, custom images or dynamic control glyphs; native image/text appearance has been exercised; complete world-transition and cleanup qualification remains limited. See [tutorials](mod-tutorials.md). |

The native `Gameplay` interface currently routes several families through one
trusted provider type. That internal interface is not a guest extension API.
Adding an import requires an explicit permission, bounded arguments and resources,
owner cleanup, defined outcomes, compatibility handling and tests. A recovered
engine binding alone does not meet those requirements.

## Descriptive C helpers

`crml.h` includes `crml_helpers.h` in the development SDK. These inline helpers
make narrower adapter scope visible in C source; the original ABI 1 imports remain
available. Each helper makes exactly one existing host call, with the same
permission, allowance, result and validation rules. They do not retry, cache state,
choose a target/value/duration, or add a native feature.

| C helper | Underlying ABI operation | Scope |
| --- | --- | --- |
| `crml_player_flight_set_velocity(x, y, z)` | `motion_set(1, x, y, z)` | Noncolliding character flight, not normal colliding movement or camera control |
| `crml_player_flight_release()` | `motion_set(0, 0, 0, 0)` | Release this mod's flight lease |
| `crml_player_flight_read(out)` | `motion_read(out, sizeof(*out))` | Lease/cancellation observation |
| `crml_player_mesh_set_hidden(hidden)` | `visibility_set(hidden)` | Root player mesh only; no AI or collision change |
| `crml_player_mesh_read(out)` | `visibility_read(out, sizeof(*out))` | Lease and native hidden/submission evidence |
| `crml_prop_set_linear_damping(target, value, duration_ms)` | `physics_apply(target, value, duration_ms)` | Temporary linear damping; not friction, weight or angular damping |
| `crml_prop_restore_linear_damping()` | `physics_restore()` | Request restoration/cancel selection; poll status for cleanup |
| `crml_startup_read_screen(out)` | `ui_read(out, sizeof(*out))` | Reviewed startup screens |
| `crml_startup_read_media(out)` | `media_read(out, sizeof(*out))` | Reviewed startup-media observation |
| `crml_startup_skip_media(generation)` | `media_skip(generation)` | Request skipping the observed eligible startup-media instance |
| `crml_ui_element_set_hidden(generation, kind, name, length, hidden, duration_ms)` | `ui_present(...)` | Temporarily hide one existing native element; does not create a panel |

Using an inline helper does not itself raise the runtime requirement. The import
it uses determines compatibility: `motion_read` and `visibility_read` need at
least `0.1.0-alpha.4.4.dev.0`; helpers around older imports retain those imports'
requirements. Declare all needed permissions in the manifest. Unused helpers do
not add imports to a normal optimized guest build.

If this scope does not cover a mod requirement, use the
[native bridge contribution guide](extending-runtime.md). Renaming a call does not
supply the missing engine operation.

## Coordinates and observations

Position arrays use world-space `[X, Y, Z]` in engine world units. The movement
adapter treats **positive Y as up** and the X/Z plane as horizontal. No conversion
to metres, geographic heading, map coordinates or campaign location is supplied.
`motion_set` takes velocity in these units per second, with vector length at most
20. The controller adapter integrates using its own callback clock, caps each
step at 50 ms and cancels a gap over 250 ms; guest tick time is not the integration
step. The first step acquires a position without moving it.

`physics_select_near(x, y, z, radius)` uses a world-aligned offset from the player's
position when the search starts. It does not rotate the offset with the camera or
track the player throughout the search. Both offset length and radius are bounded
by 20 world units; radius must be positive. The compatibility `physics_select()`
uses zero offset and radius two. Selection considers eligible single-body props
and excludes the player and attached objects. Proximity alone is not eligibility.
The search rejects near ties when the two nearest candidates differ by less than
0.1 world units. Moving the player more than 0.25 world units during the search,
changing the selection scope, or reaching its 15-second limit invalidates it.
Guests cannot currently distinguish all those rejection causes through a detailed
result record.

`motion_camera` returns normalized horizontal camera-right components `[X, Z]`.
The maintained movement example derives horizontal forward as `[-right_z,
right_x]`. This helper uses a separate main-camera lookup, so do not assume it
matches a free-camera or special slot returned by `camera_read`.

`camera_read.basis` contains three consecutive engine basis vectors, each stored
as `[X, Y, Z]`: elements `0..2`, `3..5`, and `6..8`. The engine reader uses the first
as right and the third as forward; the SDK preserves that order. They are
approximately orthonormal, not Euler angles. The reader checks finite values,
lengths and perpendicularity; it does not establish a universal determinant sign
or handedness across every camera mode. Do not infer a matrix transpose, graphics
API projection convention or quaternion layout from this array. Lens values are
horizontal FOV in radians and width/height aspect ratio, valid only with
`CRML_CAMERA_STATE_LENS`.

`physics_read` returns damping coefficients and speed **magnitudes**, not velocity
vectors. Check `CRML_PHYSICS_STATE_DAMPING` and `CRML_PHYSICS_STATE_SPEED` separately.
These reads do not grant writes to angular damping or velocity.

The player, camera and physics snapshots come from separate callbacks. Reading
them together does not produce a coherent frame. Player position is sampled
before the controller update; selected camera pose is sampled after camera update.
No interpolation, frame number or common observation timestamp is exposed.

| Observation | Current freshness and readiness rules |
| --- | --- |
| `player_read` | At most 500 ms old; focus required; `0` for stale/invalid/not focused, `-1` if the service is unavailable |
| `camera_read` | At most 500 ms old; rejects an update in flight, overlapping/inconsistent publication and lock contention; no focus gate |
| `physics_read` | At most 500 ms old; caller's valid selection required; focus/Escape, cleanup, pending work, retirement and scene identity checked |
| `motion_camera` | Player-camera sample at most 100 ms old; focus/Escape and usable horizontal right vector required |
| `ui_read` | At most 1000 ms old; current native provider returns `-1` for absent/stale samples or lock contention |
| `media_read` | At most 250 ms old; current native provider returns `-1` for absent/stale samples or lock contention |

The general snapshot convention permits `0` for not ready; the UI/media providers
currently collapse their unavailable/not-ready cases to `-1`. A returned failure
clears these snapshot outputs; a guest-memory or layout validation trap does not
promise an output write. Storage and settings reads have their own unchanged-output
failure contracts. Use the exact `sizeof` from the version-matched header; some
layouts begin with `version`, others with `size, version`.

## Identity and lifetime

All generations, selection tokens and receipts are opaque. Compare them only
within the operation family and runtime lifetime that produced them. They are not
pointers, interchangeable entity IDs, timestamps or persistent save identities.

| Value | What changes or invalidates it | Safe use |
| --- | --- | --- |
| Player generation | Player/world replacement, invalidation, backward sample clock or a publication gap over 500 ms | Reset a position history; never infer a particular load, death or campaign transition |
| Camera generation | Observed world/environment/entity/mode change, invalidation or a gap over 500 ms | Reset camera-dependent cached data; never infer player or save identity |
| UI generation | First observation, new page, changed mapped screen or changed available actions | Submit against the latest observed state; a generation change alone does not prove a new screen visit |
| Media generation | Instance change, elapsed time moving backwards, a gap over 250 ms, or changed name/source | Bind a skip request to that observation; repeated names do not identify a persistent instance |
| Physics selection token | Reselection, retirement, scene changes and cleanup; idle selection expires after 15 seconds | Retain the token obtained before applying damping; `physics_target()` returns zero while the operation is active |
| Input binding revision | This loaded owner's bindings change | Detect rebinding; not an input event sequence or world generation |
| Settings handle/revision | Handles belong to the loaded mod; revisions describe live value changes | Use optimistic revision checks; persist guest keys/values, not runtime handles |
| UI action receipt | Owner-scoped; newest 64 tracked commands retained globally | Poll dispatch outcome; eviction returns unknown/foreign status, and release does not erase retained receipts |
| Feedback receipt | Owner-scoped; only the latest successful receipt retained until unload | Poll acknowledgement/expiry/cancellation; an old receipt is not message history |

Absence of a fresh player sample can mean focus loss, a paused callback, loading
or invalid data. It does not distinguish those causes. The startup UI
mapping likewise does not supply a general in-game menu state. There is currently
no semantic campaign, save-slot, world-loaded, combat or death event API. Mods
requiring those facts need a separately established observation contract.

[Persistence](mod-storage.md) is one 64 KiB record per installation and manifest
ID. The guest owns schema validation, migration and any explicit reset. The host
uses an atomic committed-file replacement, preserves the previous record on failed
writes and drains accepted writes during normal destruction. Accepted writes
survive guest failure/unload; process crashes can still lose pending writes.
Changing or reinstalling the mod with the same ID uses the same namespace. Neither
player generation nor a user-selected filename makes this campaign-safe storage.

`release()` cancels the owner's native leases and pending work and cancels owned
feedback; it does not unload the guest or reset its bindings/settings. Trap/unload
also detach bindings, settings and feedback. The host supplies owner identity;
guests cannot impersonate another owner through an argument. Cleanup requests are
not synchronous engine restoration: physics requires a later eligible callback,
mesh visibility resumes through the engine mesh update, and UI presentation uses
renderer polling. A delivered action cannot be recalled.

## Scheduling and time

`crml_tick(elapsed_seconds)` is a worker heartbeat, not a game frame or game-thread
callback. Guest lifecycle calls are serialized. Packages load in directory sort
order, compilation/instantiation/initialization complete before the game host's
heartbeat loop, and every live ticking guest is called sequentially. Slow
compilation or another callback can delay later work. No callback latency or
startup duration guarantee is made.

The game host currently sleeps for 10 ms when physics, guest movement or requested
keyboard service requires polling, otherwise 100 ms. This is a sleep request, not
a fixed tick rate. The measured steady-clock interval includes time spent in the
previous loop; `Runtime::tick` clamps the argument to one second. A sum of tick
arguments therefore loses time during long stalls. The host does not suspend
heartbeats just because the game is paused or out of focus.

Development builds provide `clock_ms()` (`crml_clock_ms()` in C): monotonic
milliseconds since construction of this runtime. It is sampled once at the start
of each guest invocation, including module start, ABI version, initialization,
tick and shutdown. Repeated reads in one invocation return the same value; two
mods in one heartbeat need not receive the same value. It includes elapsed
startup/compilation time and long host stalls that the tick argument clamps away.
If a host-provided clock moves backwards, the exposed value stays at its previous
maximum until the clock catches up. This is not a clock adjustment event API.

Use unsigned 64-bit differences within the current runtime for guest deadlines.
The clock requires no capability, but each read spends one observation call;
cache one reading per callback when needed. Set
`min_runtime=0.1.0-alpha.4.3.dev.0`. It is not game simulation time, a calendar
date, a persistent identifier or an absolute lease deadline. Do not persist it
as a time that remains meaningful after restart. Existing tick exports and the
one-second clamp retain their ABI behavior.

Native snapshot age, request expiry and leases use the host's native monotonic
millisecond clocks, independently of that clamped argument. Continue checking
results when renewing a lease. Renewal can fail after expiry or a stale-input
cancellation even when the guest's own timer has not elapsed. Do not spin inside
a callback waiting for engine work; observe results on later callbacks.

The [deterministic simulator](mod-testing.md) supplies scripted timing and provider
observations. It tests guest policy and service boundaries, not actual scheduling
cost, engine callback frequency or gameplay pause behavior.

Optional [performance profiles](mod-testing.md#measure-guest-cost) measure
module loading and synchronous callback wall time, fuel use and peak host-call
counts. They preserve the scripted clock and do not measure deferred engine or
renderer work. The game runtime emits a bounded schedule of cumulative profiles;
use those to assess the actual installed mod combination before changing budgets
or making latency claims.

## Results and ownership

Results are operation-specific. **Do not interpret every positive result as
completion or every zero as failure.** In particular, a zero-byte storage record
is a successful read, `0` is successful queue acceptance for several commands,
and snapshot `0` means no readable observation. Negative values are not a shared
cross-service error enum: `-2` can mean owner contention, absent storage, unknown
receipt or unknown setting handle.

Use the operation-specific names in `crml_results.h` (included by `crml.h`), such
as `CRML_PHYSICS_APPLY_ACCEPTED`, `CRML_PHYSICS_STATUS_SELECTED`,
`CRML_STORAGE_READ_ABSENT` and `CRML_SETTINGS_SET_STALE_REVISION`. They preserve
the existing integer values and do not change runtime compatibility. UI/feedback
positive receipt states keep their existing constants. Some existing outcomes
remain collapsed: a settings resource error can mean revision/capacity exhaustion
or a host failure, and legacy UI/media rejection does not identify its cause.
See the [named-results example](api.md#named-results).

| Operation | Successful return establishes | Further evidence |
| --- | --- | --- |
| `motion_set`, `visibility_set` | `1`: lease accepted/renewed; `0`: release | No guest receipt proves controller/render execution |
| `motion_read` | Owner-local accepted lease or retained cancellation reason | Does not acknowledge cancellation or prove controller execution; explicit cleanup clears the record |
| `visibility_read` | Caller-owned lease, hidden-mesh observation and renderer command publication flags | Evidence is limited to the current lease; publication is not completed rendering or restoration |
| `physics_select_near`, `physics_apply`, `physics_restore` | `0`: queued request or permitted no-op cleanup | Poll `physics_status`; copied property data and native diagnostics give additional evidence |
| `ui_activate`, `media_skip`, `ui_present` | `0`: queue accepted | Legacy APIs do not expose an execution receipt; do not blindly replay an accepted media skip |
| `ui_action_submit` | Positive command receipt | `DISPATCHED` means the renderer trigger returned; it does not prove the engine advanced. `OUTCOME_UNKNOWN` may later resolve; delivery uncertainty is not permission to retry |
| `feedback_show` | Positive message receipt | Presented means renderer insertion was acknowledged, not that the player saw it |
| `storage_write` | `0`: bytes copied and replacement queued | `storage_status == 2` establishes committed replacement |

Physics status values are `0` idle, `1` queued, `2` searching, `3` selected, `4`
active, `5` restoring, `6` finished, `7` retired, `8` game conflict and `9` refused.
`finished` alone is not proof of restored momentum or a particular gameplay
effect. Retirement prevents writes into a replacement body; a game conflict
preserves the game's changed value instead of overwriting it. Ownership can remain
held until deferred cleanup completes. The API has no detailed guest target
description or per-request failure ledger.

Argument validation, memory bounds and exhausted invocation allowances can trap
a guest. These are separate from a service returning unavailable or busy. See
[sandbox limits](sandbox.md), [action bindings](mod-input.md),
[settings](mod-settings.md) and [feedback](mod-feedback.md) for their respective
budgets and named state constants.

## Compatibility and evidence

ABI 1 import signatures and published layouts are compatibility commitments.
Engine adapters remain experimental even when their import signatures are stable.
Development additions have per-import minimum versions. Settings, storage,
feedback and configurable actions require `0.1.0-alpha.4.3.dev.0`;
`motion_read`, `visibility_read` and tutorials require `0.1.0-alpha.4.4.dev.0`.
Set `min_runtime` to the newest requirement among the imports your mod uses.
Availability and minimum-version checks serve different purposes. A development runtime is not a claim of new live validation.

The reviewed executable profiles are identified by SHA-256:

| Profile | Executable SHA-256 |
| --- | --- |
| Previous | `2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0` |
| October update | `a2e8e57c86ea60f12de1fa628f8db12014eb296fb259449ea688df9c7ba1497a` |
| October hotfix (0.564.478.0) | `4f6596b08bb5bc7fe4150cf5b9f71d7bae87eea02d66627c03a5e05ffe84ea62` |

The profiles map reviewed addresses and field accesses; hooks also check expected
code and relevant backend fingerprints. This establishes static compatibility
checks, not operation effects. Physics additionally requires the reviewed backend
SHA-256 `ec53e67b700e67a5420a2c951340efd9a8ed9d086da539eed6ee88368fe0ce51`.

The following matrix separates evidence levels. Native tests use controlled
objects or fixtures; scripted Wasm providers and browser tests do not execute the
game. Historical observations qualify only the operation/build described, not all
later runtime changes. “Pending” means no complete qualification is asserted.

| Operation | Offline evidence | Previous profile live evidence | October update/hotfix live evidence / remaining qualification |
| --- | --- | --- | --- |
| Callback clock | Real Wasm injected-clock checks, backward-clamp/full-width checks, scripted long-stall and observation-budget tests | No engine adapter required | Actual heartbeat latency/startup cost is not qualified by deterministic clock tests |
| Player position read | Native copied-sample lifetime tests and Wasm memory/layout tests | Controller observations exist; complete guest lifecycle qualification pending | Static profile port; current guest reload/player-replacement qualification pending |
| Navigation observation | Component-layout, quaternion and copied-snapshot tests; Wasm boundary and scripted observations | Adapter unavailable on earlier profiles | Static movement-plane producer/consumer trace; live gravity-transition, zone and reload qualification pending |
| Text settings | UTF-8/byte bounds, stale revisions, copied Wasm values and browser draft/IME lifecycle fixtures | Native text entry not qualified | Native text focus, IME and input ownership still need a grouped live check |
| Schematic drawing | Bounded host frames, owner/rate/expiry tests and browser geometry/teardown fixtures | Native drawing not qualified | Native appearance and polling/expiry timing still need a grouped live check; admission is not presentation |
| Selected camera read | Native selected-view/cache tests, overlap/invalidation tests, Wasm layout checks | Selected-camera update capture; not final-render or complete lifecycle validation | Static profile port; mode changes, final-view limitations and reload qualification pending |
| Noncolliding movement and scoped boundary guard | Native lease/controller/input tests and scripted guest cancellation | Scoped boundary behavior and movement previously exercised | Hotfix: held-key Alt-Tab cancellation and release/repress recovery exercised; keyboard-age expiry and multi-owner edge cases covered separately by automated tests |
| Player mesh hide/release | Native target/lease tests and scripted guest behavior | Adapter available; no complete per-operation qualification asserted here | Hotfix: Photo Visibility hold/toggle, focus recovery, settings changes and visible replacement player after save reload exercised |
| Prop selection and copied physics state | Native selection/session/snapshot tests | Eligible target, getter samples and body-release observation captured | Static profile port; guest target/read lifecycle qualification pending |
| Linear damping apply/restore | Native state-machine/accessor tests | Native diagnostic and guest-enabled captures show property writes/readback and restoration; visible effect remains separate | Current guest operation, visible effect, conflicting game write and reload qualification pending |
| Startup UI Continue/presentation | Native queue/resource-response tests, headless renderer and scripted guest tests | Adapter behavior covered by fixtures; no blanket gameplay qualification | Hotfix: enabled/disabled Startup Preferences launch workflow exercised; individual screen coverage is not established for every startup variant |
| Startup media skip | Native adapter/queue tests and scripted guest tests | No complete per-operation qualification asserted here | Included in the successful Startup Preferences launch check; no separate per-asset timing capture or all-movie coverage |
| Settings and feedback UI | Native service/resource tests and headless browser interaction/cleanup | Not established for these development additions | Hotfix: both selected mods' settings, Options close/reopen and input ownership, notification expiry and disabled notices exercised; keyboard/controller row navigation remains unsupported |
| Native tutorial hints/panels | Native queue and retirement tests; compiled guest tests; browser pixel, layout and cleanup checks | Not established for these development additions | Image/text presentation and proportions confirmed on the October hotfix; full world-transition, competing tutorial and focus-restoration coverage remains limited |
| Mod record persistence | Real filesystem replacement, failure/isolation and process-restart tests | No game adapter required | Hotfix: Startup Preferences and Photo Visibility settings survived a game restart under their respective mods; campaign identity unsupported |

The selected-mod checks above apply to runtime `0.1.0-alpha.4.4.dev.0` on
`0.564.478.0`. They are gameplay observations, not instrumented frame-time or
per-screen coverage measurements. They do not qualify other executable profiles,
arbitrary third-party mod combinations or every engine lifecycle edge case.

The October hotfix has separately reviewed adapter relocations. Input and menu
diagnostic callbacks have delivered both eligible/ineligible action views and
matching/nonmatching navigation contexts on that executable; see the
[input context map](research/input-context-hotfix-map.json). These observations
do not establish named menu coverage or exclusive input ownership. Qualification
from the earlier October executable does not automatically transfer to the hotfix
or to later runtime changes.

The native property trial's restoration evidence does not establish restored
velocity, complete encounter cleanup or save transactions. The camera capture does
not validate camera control. A successful native Options test does not validate
all later settings/feedback widgets. These distinctions remain necessary even when
one executable is listed as supported by the loader.

Contributors can trace these contracts in `runtime/runtime.cpp`,
`runtime/entry.cpp`, `runtime/player_snapshot.h`, `runtime/camera_service.cpp`,
`runtime/movement_view.cpp`, `runtime/noclip.cpp`, `runtime/physics_session.cpp`,
`runtime/ui_service.cpp`, `runtime/media_service.cpp` and the corresponding tests.
Use [engine validation](engine-validation.md), [movement](movement.md),
[visibility](visibility.md) and [physics trial](physics-trial.md) to collect bounded
operation and restoration evidence. Promote an operation's qualification only
with its executable/runtime version, observed outcome and tested cleanup paths.
