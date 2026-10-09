---
title: Test guest behavior with scenarios
description: Run CRML Wasm mods against deterministic input, snapshots, failures and competing owners without starting the game.
---

# Test guest behavior

The development SDK after Alpha 4.3 includes `tools/mod.py simulate`. It runs
compiled mods in the real Wasmtime sandbox with scripted native services. Use it
to test guest decisions, capability handling, retries, contention and cleanup
before testing engine behavior in the game.

## Run the movement scenario

From the SDK root:

```powershell
python tools/mod.py build examples/movement --output simulation-mods/movement
python tools/mod.py simulate simulation-mods sdk/scenarios/movement-recovery.json --report reports/movement.json
```

Keep only the movement package in this scenario's mods directory. The scenario
starts flight, makes keyboard input stale, then restores fresh input. It expects
five command/cancellation records, including shutdown. A different record count,
wrong argument or unexpected guest failure produces a nonzero exit status.
`--report` writes the observed events and logs even when an assertion fails.

The simulator starts a fresh host process for each run. Its clock advances only
by the supplied frame durations; it does not read desktop input or launch the
game. The same packages and scenario produce the same trace and session times
unless optional wall-time profiling is enabled.
The base `clock_ms()` import uses this scripted clock and includes the full
frame duration even when `crml_tick` receives a delta clamped to one second.

## Measure guest cost

To exercise several maintained packages together:

```powershell
python tools/mod.py build examples/movement --output composed-mods/movement
python tools/mod.py build examples/photo-visibility --output composed-mods/photo-visibility
python tools/mod.py build examples/startup-preferences --output composed-mods/startup-preferences
python tools/mod.py simulate composed-mods sdk/scenarios/composed-author-services.json --profile --report reports/composed.json
```

Use a directory containing only these three packages. The scenario covers startup
requests, simultaneous flight and mesh hiding, focus loss, Escape, a missed lease
deadline, settings resets and player replacement. It supplies synthetic renderer
evidence. Storage is unavailable in this provider; persistence is exercised by
the separate filesystem tests. The CLI checks guest failures and records the
trace; the SDK acceptance tests additionally check recovery behavior and service
activity. A successful simulation does not establish native menu input ownership
or in-game frame cost.

Host budgets apply to each guest callback, not to the sum of all installed mods.
An expensive peer can still delay another mod's next callback: lease recovery
must handle that delay even when both guests stay within their own call limits.

Add `--profile` to a simulation to include cumulative performance diagnostics in
the report's `logs` array:

```powershell
python tools/mod.py simulate simulation-mods sdk/scenarios/movement-recovery.json --profile --report reports/movement-profile.json
```

For a standalone run without simulated gameplay providers, use
`tools/crml_host.exe simulation-mods 1000 --profile` from the extracted SDK.
This executes 1,000 callbacks as fast as the host can run them; it does not sleep
for the supplied guest time or measure the game's frame rate.

Each package has a `Metrics` summary with its final state, `load_ns` and
`compile_ns`. Load time includes file reading, compilation, linking, module start,
ABI checking, initialization and load logging. Compilation is a subset of load
time. Rejected manifests have no module profile; a module rejected during loading
retains the measurements collected before rejection.

Separate `start`, `abi`, `init`, `tick` and `shutdown` records report:

- `calls` and `failures`, including trapped calls;
- cumulative `total_ns` and worst observed `max_ns` wall time;
- `fuel_peak`, the highest observed Wasmtime fuel consumption in one invocation;
- peak attempted calls to each host budget (`reads`, `commands`, `bindings`,
  `storage`, `settings`, `feedback`, `logs`). A rejected over-budget call counts.

`start` includes Wasmtime instantiation, whether or not the module has a start
function. Callback timing includes synchronous native host calls and OS
scheduling delays, but excludes fuel-reset and profiling bookkeeping. It does
not include later engine work, rendering or asynchronous storage completion.
`fuel_errors` counts unsuccessful fuel queries; nonzero values mean the fuel
peak may be incomplete. Fuel is an execution budget, not elapsed time.

Profiles use a fixed number of counters for at most 32 packages and do not keep
per-frame records. Ordinary simulation reports remain deterministic. Profiling
uses the host's real steady clock without changing the scripted clock or event
expectations, so profiled timing values vary between runs. Compare representative
mod combinations and repeated runs; do not use a single wall-time threshold as a
portable test assertion.

The game runtime emits cumulative summaries after package loading, then after
approximately 5, 30, 60, 120, 300 and 600 seconds of worker execution. A delayed
worker skips missed reporting slots instead of emitting a burst. These use
protected host logging and stop after the bounded schedule. If the worker exits
its loop it also emits a final summary; closing the game can terminate the worker
without reaching that point. Startup summaries contain no tick measurements.

The same bounded reporting schedule includes `Worker metrics` in the game log:

- `load_ns` covers package discovery, provider preparation and module loading
  after compatibility authorization; it excludes the compatibility dialog.
- `loops` and `interval_total_ns` / `interval_max_ns` describe completed worker
  iterations and the interval between their starts. Intervals include sleep,
  scheduling delays and the preceding iteration's work.
- `dispatch_total_ns` / `dispatch_max_ns` measure the complete `Runtime::tick`
  call, including guest calls and their synchronous host work.
- `native_total_ns` / `native_max_ns` measure subsequent worker-side service
  polls and diagnostic logging. They exclude metric-report formatting/output,
  other threads, engine hooks and renderer work.
- `gaps_over_250ms` and `gaps_over_500ms` count long intervals; they are evidence
  of delayed worker execution, not proof of which subsystem caused it.

These counters help distinguish callback cost from polling and scheduling cost.
They do not measure frame rate or all native execution caused by a mod. Metrics
are cumulative; compare differences between reports when examining a particular
part of a session. If no report was captured, absence is not evidence of low cost.

## Scenario format

A scenario is a UTF-8 JSON object with `schema: 1`, optional `initial` state, and
a `frames` array. Unknown fields and duplicate JSON keys are rejected. For example:

```json
{
  "schema": 1,
  "capabilities": ["input.actions", "player.read"],
  "initial": {"player": {"generation": 12, "position": [0, 0, 0]}},
  "frames": [
    {"dt_ms": 16, "state": {"keys": ["Home"]}},
    {"dt_ms": 16, "state": {"keys": [], "player": {"generation": 12, "position": [6, 0, 0]}}},
    {"dt_ms": 16, "state": {"focused": false}}
  ],
  "expect": [],
  "expect_failures": 0
}
```

Initial state is applied before module start and `crml_init`, except settings
edits, which apply immediately after initialization has registered definitions.
For each frame,
state changes are applied, the clock advances by `dt_ms`, expired leases are
cancelled, then the host calls the guests' ticks. `dt_ms` defaults to 10 and may
be 0–60,000. Shutdown runs after the last frame. Frame 0 denotes initialization;
frame 1 is the first tick. Shutdown events use the last frame number.

Unspecified state categories persist. Supplying a snapshot object replaces that
whole snapshot, with defaults for omitted fields. Snapshot ages are explicit;
they do not advance automatically. This permits deliberate stale-data fixtures.

| State field | Meaning |
| --- | --- |
| `keys` | Held controls using the manifest's named-key vocabulary; `[]` releases all |
| `focused` | Boolean, initially true; false masks input, cancels movement/physics and makes player reads not ready |
| `input_fresh` | Boolean, initially true; false models an unavailable keyboard sample |
| `input_emergency` | Boolean, initially false; true models Escape, masks input and cancels active movement, visibility and physics leases |
| `visibility_observation` | `none` (default), `hidden`, or `submitted`; explicitly publishes synthetic hidden-mesh or hide-command evidence for the current active lease before each tick |
| `heading` | `result` and two-component horizontal `right`; defaults to success and `[1,0]` when supplied |
| `player` | `result`, `generation`, `age_ms`, three-component `position` |
| `navigation` | `result`, `generation`, `sequence`, `age_ms`, flags 0 through 15, three-component `position` and `up`; `CRML_NAV_UP_VALID` requires unit up |
| `camera` | `result`, `generation`, `age_ms`, `mode`, `flags`, `position`, nine-component `basis`, `fov`, `aspect` |
| `physics` | `result`, `age_ms`, `flags`, `linear_damping`, `angular_damping`, `linear_speed`, `angular_speed` |
| `physics_status` | Scripted status 0–9, matching the guest API's status numbers |
| `ui` | Scripted startup `screen` (0–9) and `actions` (0 or 1), both defaulting to 0 when supplied |
| `ui_renderer` | Boolean, initially true; false stops simulated UI polling and delivery |
| `ui_outcome` | `dispatched` (default), `skipped`, `failed` or `none`; outcome assigned when a command is delivered |
| `ui_new_page` | A true value replaces the simulated UI document before that frame; false does nothing |
| `settings` | Array of `{ "mod": "package-id", "key": "setting-key", "value": 1 }` edits; `value` may also be a UTF-8 string for text settings; uses real owner-local definitions and validation |
| `media` | Native media observation: `identity`, `active`, `ready`, `elapsed_ms`, `name`, `source` |
| `media_observer` | Boolean, initially true; false stops media observations, allowing native freshness expiry |
| `media_consume` | Boolean, initially true; false withholds consumption of accepted skip requests |
| `returns` | Forced negative command outcomes; set an operation to `null` to restore ordinary handling |

Navigation scenarios require capability `navigation.read`. `generation` and
`sequence` default to 1; age, flags and vectors default to zero. With no up-valid
flag the provider clears up. Focus loss, emergency state or age above 500 ms
returns not-ready; failed reads zero the whole output. Sequence and age are
explicitly scripted and persist across frames, not automatically inferred from
frame duration. This tests guest response to supplied observations, not the
native movement-plane adapter or actual gravity transitions.

Text setting edits use a JSON string for `value`, including `""` to clear it.
The scenario encoder rejects invalid UTF-8, controls, line separators and values
above 255 UTF-8 bytes before invoking the host; the real setting definition's
smaller byte bound still applies. The simulator does not display text controls
or establish native focus/IME behavior.

Snapshots start unavailable (`result: 0`) until supplied. Supplied snapshots
default to success, version 1, age 0 and generation 1 where applicable. Player
position defaults to zero. Camera defaults are identity basis, mode 0, pose flag
1, field of view 1 radian and aspect 1.7777778. Physics defaults to flags 3 and
zero values. Snapshot result codes are -1, 0 or 1, with -2/-3 also supported for
physics. Non-success reads zero the output through the ordinary guest bridge.

`capabilities` selects simulated services independently of each mod's declared
permissions. Omitting it enables the supported simulated services; `[]` disables
them. Supported names are `input.actions`, `input.buttons`, `input.motion`,
`player.motion`, `player.visibility`, `player.read`, `camera.read`,
`physics.damping`, `feedback`, `ui.read`, `ui.activate`, `ui.presentation`,
`media.read`, `media.skip`, `navigation.read` and `lists`. Logging and typed settings remain available to
mods that declare those permissions. Storage and the legacy native-key noclip
helper remain unavailable in the simulator.

Setting edits are applied in array order; missing mods/keys and invalid values
fail the scenario. They do not simulate pointer clicks or persist across runs.
Storage requires separate real-filesystem checks.

### Scripted lists

The `lists` capability runs the real bounded list service using the scenario
clock. `lists_renderer` defaults to `true`; setting it to `false` disables the
service and clears pages and pending actions. Re-enabling it does not restore
old pages. This models host availability, not actual native pixels or focus.

Supply up to 16 `list_actions` in an initial or frame state:

```json
{
  "lists_renderer": true,
  "list_actions": [
    {"mod": "catalog", "row": 42},
    {"mod": "catalog", "row": 43, "revision": 1}
  ]
}
```

Each action requires a validated manifest ID and nonzero unsigned 64-bit row ID.
An optional nonzero unsigned 64-bit `revision` exercises stale requests; omitting
it selects that owner's current published revision. Initial actions apply after
guest initialization. Frame actions apply in array order after advancing frame
number and time, before the guest tick. They are one-shot commands, not persistent
state. The named mod must have a currently published page; an unknown or hidden
page fails the scenario. Missing rows on an existing page use the real service's
rejection result. No text assets or synthetic gameplay actions are inferred.

Reports include these additional events:

| Operation | Result | `args` |
| --- | --- | --- |
| `list_activate` | Service status: `200` accepted, `409` stale/disabled, `404` absent row, `429` full queue | `[revision, row_id]` |
| `list_page` | `1` for a new/changed admitted page | `[revision, row_count, selected_row_id_or_zero]` |
| `list_page` | `0` when an observed page disappears | `[last_revision, 0, 0]` |

Page observations occur after initialization and each tick, and after runtime
shutdown for cleanup. Identical-revision republishes do not emit extra page
events. Activations can accumulate to the service's 16-event limit across frames;
full queues reject additional clicks without discarding accepted events. Guests
read them through destructive `list_next` with the normal observation allowance.
Expected integer arguments compare exactly, including IDs above 2^53 and
`UINT64_MAX`; floating-point arguments retain tolerant numeric comparisons.

These events establish copied host page state and action admission. They do not
establish native list presentation, gameplay eligibility or a completed mutation.

Media uses the native observation/request service. Defaults when supplied are
identity 1, active and ready true, elapsed 0, empty name, and source `engine`.
Source may instead be `mapped` or `none`; names use ASCII letters, digits,
spaces and `_./\\:-` (at most 255 bytes). Observations occur before and after
initialization and every tick. Elapsed time is explicit, not automatically
advanced. Native readiness requires elapsed time above 2000 ms. Gaps above
250 ms invalidate the observation generation, even with the same identity.
Consumption retires that identity until an inactive observation or a different
identity arrives. A `media_consumed` trace records native queue consumption,
not video playback or a completed engine transition.

The UI simulation uses the native queue and receipt service, with a synthetic
renderer. Supply `ui` to publish the first observation. UI polling occurs before
initialization, after initialization, and before and after each guest tick.
An accepted command is delivered on a poll; its configured outcome is reported
on the next poll. `none` drops the report, allowing expiry and uncertain-delivery
tests. Changing `ui_outcome` does not rewrite an already prepared report.
`ui_renderer: false` pauses these polls, so real service freshness and command
expiry still apply. Replacing the page discards pending renderer reports.

No game event is triggered. A simulated `DISPATCHED` receipt does not advance
the screen, change readiness, or enforce the actual renderer's once-per-visit
Continue policy. Script those state changes explicitly. This tests guest
behavior and native queue ownership; the browser and game provide separate
evidence for rendering and engine transitions.

UI presentation uses the native lease service, but there is no DOM. Its numeric
trace arguments are generation, target kind, hidden flag and duration. The copied
name is in the event's `target` field; assert it to catch requests aimed at the
wrong element. Use browser tests to verify target resolution and actual
presentation.

## Outcomes and ownership

Movement and visibility each have one simulated owner with a 500 ms lease.

`visibility_read` uses the same lease/evidence helper as the runtime. No renderer
evidence is fabricated by default. Set `visibility_observation` to exercise the
guest's observed/submitted branches; this is scripted evidence, not engine or GPU
execution. Reads preserve expiry and ownership. Expired lease observations remain
until release or takeover, matching the native helper.

Another owner receives `-2`, including on release requests. The simulator reuses
the runtime's pure flight state machine and visibility lease. Movement permits
renewal at exactly 500 ms; visibility expires at its deadline. A missed movement
renewal, focus loss or stale keyboard state produces a cancellation record. The
next enabled request reports cancellation instead of silently rearming flight.
`motion_read` observes the same owner-local lease and stop reason without
consuming cancellation. Focus, stale-input, emergency and lease-expiry scenarios
can exercise these guest branches; engine-only controller stop reasons still
require native evidence. A scripted request refusal does not fabricate a stop
reason for a lease that was never acquired.
Automatic cleanup after a guest trap releases its ownership before the next mod
runs. Mods run in package-directory order.

Camera reads are independent of focus and keyboard freshness. Player reads
return 0 without focus and remain independent of keyboard freshness. A visibility
request without focus releases that caller's lease; keyboard freshness alone does
not gate an explicit visibility request. An unavailable service returns -1 on
release too. These distinctions let scenarios exercise the same guest branches
as the API contracts.

Physics selection claims an owner and creates a new token. The scenario controls
when `physics_status` becomes selected (3), active (4), finished (6), or another
state. `physics_target` returns the owner's token only in selected state.
Application records the request; it does **not** change scripted damping or
status automatically. Supply later status and snapshot changes explicitly to
test the distinction between queue acceptance and observed execution. Restore
and automatic cleanup retire the simulated token. Selection is not a spatial
search: the supplied query coordinates are recorded for assertions.

To make a command fail, use a state change such as:

```json
{"returns": {"motion_set": -1, "physics_apply": -3}}
```

Supported operations are `motion_set`, `visibility_set`, `physics_select`,
`physics_apply` and `physics_restore`. Overrides accept -1, -2 or -3 and apply
after ordinary availability/ownership checks; they cannot turn a refusal into
success. They persist until reset to `null`.

## Assert the trace

`expect`, when supplied, must match the complete event count and order. Each
entry requires `op`; other fields are optional assertions. Omitting `expect`
records events without asserting them. Guest failures still default to zero.

```json
{
  "op": "motion_set",
  "frame": 1,
  "time_ms": 10,
  "result": 1,
  "args": [1, 0, 0, 5]
}
```

Events also contain `owner`, an opaque host identity for that run. It is not a
mod ID or persistent identifier. Arguments follow the operation's guest API,
except `physics_select` always records `[offset_x, offset_y, offset_z, radius]`.
Numeric arguments use relative tolerance 1e-5 and absolute tolerance 1e-6.
Frame, owner, time and result comparisons are exact.

Cleanup events are `release`, `motion_cancel`, `visibility_cancel` and
`physics_cancel`. `release` is emitted only when automatic or explicit shared
cleanup owns a movement, visibility or physics resource. UI cleanup is visible
through its receipt state rather than a separate cleanup event. UI submissions
emit `ui_action_submit` (positive receipt or negative error) or legacy
`ui_activate`; their arguments are generation and action. The `returns` override
does not apply to these operations: script screen, readiness, renderer and page
changes to exercise the actual native queue. A mod's own shutdown commands are also
recorded. `expect_failures` allows an intentional number of rejected/trapped
guests; healthy mods continue through the normal runtime isolation path.

Limits are a 1 MiB scenario, 10,000 frames, 50,000 command/cleanup events and a
60-second host timeout. Ordinary guest fuel, memory and callback budgets still
apply. Host protocol errors fail the run even when guest failures are expected.

## What a passing scenario establishes

The `feedback` simulation capability enables a fake renderer using the scenario
clock. It polls after initialization and each tick, acknowledging the previous
poll's messages. A message published during initialization is queued during the
first tick and normally acknowledged by the second tick. Set the Boolean
`feedback_renderer` state to `false` to withhold polls; the installed service
remains available and message deadlines still expire. This tests delivery failures
without drawing UI. Runtime shutdown is checked for orphaned messages.

Typed settings are a core host service: guests can register, read and modify their
own values in a scenario. This does not simulate the native Mods tab or renderer
edits. The scenario's gameplay-capability mask does not disable core settings.

The simulator currently does not provide persistent storage. Mods declaring
`storage` can load, but capability queries omit it and storage imports return
`-1`. Use the standalone host with a temporary mods directory to exercise the
[actual disk service](mod-storage.md); its asynchronous completion is not part
of the deterministic scenario clock.

A pass establishes how the guest reacts to the supplied observations and
responses. The simulator does not integrate movement, render geometry, run
collision, select real bodies, restore real physics values or implement engine
thread scheduling. Its small ownership model is not a second implementation of
the engine adapters. Controller-sample gaps, identities, teleport handling,
physics worker queues and body lifetimes are not simulated. Use live checks for native operation semantics, reload,
real input suppression and restoration. Keep scenarios focused on guest policy
and error handling so those live checks can be shorter.
