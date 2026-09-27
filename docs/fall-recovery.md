# Player fall recovery

Northlight's `heron::fall_respawn` systems coordinate safe-position storage, recovery activation, a camera fade and a return transition. They operate separately from character-controller collision. Bypassing collision therefore does not disable out-of-bounds recovery.

The addresses below are RVAs for the executable fingerprint in [the reference map](research/fall-recovery-map.json). They describe recovered implementation details, not a supported engine ABI or a Wasm API.

## Activation paths

| Path | Implementation | Behavior |
| --- | --- | --- |
| Player inactive update | `0x25a7230` → `0x25a55c0` | Maintains safe-position history and checks vertical distance from the stored safe position. |
| Trigger targets | `0x25a8360` | Iterates a trigger's target entities, resolves their recovery components and activates eligible targets. |
| Active player recovery | `0x25a7620` | Advances the recovery timer, requests a return transition and completes recovery after the transition state clears. |
| Player camera state | `0x25a85f0` | Observes the recovery-active bit, controls camera overrides and writes fade requests. |
| Teleport handling | `0x25a6f50` | Updates or restores safe-position records after a teleport. |

The inactive update first rejects `FallRespawnData+0xe4 != 0` and another per-entity exclusion byte. Its shared helper stores safe positions when the caller permits caching. Otherwise, with a valid safe position and a positive configured threshold, it activates recovery when:

```text
stored_safe_y - current_y > configured_drop_distance
```

That comparison is at `0x25a588c–0x25a589a`; activation sets bit 0 of `FallRespawnData+0xe5` and resets the timer at `+0xe0`. The threshold comes from the helper's configuration argument at `+0x18`. This is a relative drop test, not a single global map-floor height.

The trigger path is independent. It resolves each candidate with `0x25ad320`, rejects already-active recovery, and checks a target category against a configured list. A match sets the same active bit and timer, records the trigger entity at `+0xd0`, and copies or overrides recovery and camera timings. This path does not use the shared height-check helper or test `+0xe4`. Blocking only the height check does not cover trigger-driven recovery.

The recorded source differs by path: the height path stores the recovering entity; the trigger path stores the trigger entity. Compare it with the full player handle, including generation. This is useful supporting evidence alongside the observed producer callback, rather than a substitute for tracing the callback.

## Recovery and fade sequence

`FallRespawnData` has a recovered stride of `0xf0` bytes. Relevant fields are:

| Offset | Interpretation supported by the traced consumers |
| --- | --- |
| `+0x20` | Stored safe-position vector; Y is at `+0x24`. |
| `+0x40` | Safe-record validity byte. |
| `+0xa0/+0xa8/+0xac` | Safe-history storage pointer, count and capacity; entries are `0x40` bytes. |
| `+0xb0` | Delay used by the optional effect/damage branch. |
| `+0xb4` | Delay before submitting the return transition. |
| `+0xc0` | Position captured when recovery activates. |
| `+0xd0` | Source entity recorded by the activation path. |
| `+0xe0` | Elapsed recovery time. |
| `+0xe4` | Exclusion byte checked by the inactive update. Its complete ownership is unresolved. |
| `+0xe5`, bit 0 | Recovery active. |
| `+0xe5`, bit 1 | Return transition submitted; active recovery waits for transition state to clear. |
| `+0xe5`, bit 2 | Completion work pending; the next active update consumes and clears it. |

The active updater requires bit 0 and a valid safe record. Once its timer exceeds `+0xb4` and the checked blocking component permits it, it resolves a return position, builds a transition payload and calls `0x1b49f30` with selector `0x10`. It then sets bit 1. A later invocation checks transition-state bit `0x1000`; when clear, it performs completion work, clears bits 0 and 1, and sets bit 2. The downstream transition executor is not identified by this call reference alone.

The camera updater compares bit 0 with byte 0 of the player's `FallRespawnCameraData` (stride `0x10`). On activation it requests a camera override. Its fade branch writes raw level `255` and a duration to the fade request and latches camera-data byte 1. On deactivation it writes raw level `0`, uses the clear duration at camera-data `+0x0c`, clears the latch and releases the camera override. The request's final renderer/color interpretation remains separate from this producer.

This explains a potential partial-reset failure: preventing the return transition while leaving recovery active can leave the fade latched. Clearing a rendered effect alone also leaves its activation and transition state intact. A movement exemption must account for activation, completion and camera cleanup together.

`heron::player_fall_monitor` is a separate system family with script-visible outputs. Its existence does not establish that it caused a particular recovery. The movement boundary guard does not suppress that monitor.

## Scripted boundary recovery

The asset `data/lua_scripts/heron/generic/out_of_bounds_area.binlua` implements another boundary-recovery path. Its decoded control flow includes trigger handlers, global post-processing parameters and timed callbacks. These operations are separate from the `FallRespawnData` state machine above.

| Script function | Recovered behavior |
| --- | --- |
| `init` | Registers boundary, invalid-area, exclusion-area and jump-completion handlers, with optional gap-transition handlers. |
| `on_exit_oob` | Checks script visibility, exclusion/gap state and `cine_active`; prepares a return marker and starts the visual transition. |
| `on_enter_oob` | Reverses the visual transition when its visibility/gap/cinematic checks permit. |
| `on_invalid_area_enter` | Finds the triggering invalid-area index, copies the corresponding configured safe transform to the player, then calls `nl_teleport`. |
| `update_transition` | Advances or reverses a bounded transition timer. On completion it can request a HUD flash and schedules a delayed callback. |
| Delayed callback, definition line 175 | Enables invalid-area triggers, adjusts the return marker to retain the player's current Y coordinate, copies its transform to the player, calls `nl_teleport`, and schedules completion work. |
| `on_transition_jump_completed` | Schedules hiding of the invalid-area triggers after a delay. |

The visual transition applies `Atmosphere:GroundFog Density`, `Atmosphere:Depth Ramp Distance`, `Atmosphere:Depth Ramp Feather`, `Atmosphere:Scatter Blur`, `Exposure:Max` and `PostProcess:Radial Blur` through `nl_global_param`. Its reverse path uses `nl_global_param_blend_out`. Blocking only the transform copy would leave this visual state and scheduled work intact. Disabling an instance mid-transition also requires accounting for already registered callbacks and enabled triggers; the script's `disable` function alone does not establish complete cleanup.

The native copy binding at `0x1a13a20` calls wrapper `0x1813ab0`, which reaches world-transform writer `0x1811fe0`. The caller return RVAs are `0x1a13af3` and `0x1813bb7`. This chain identifies the binding, but many scripts use it: a native stack alone does not identify `out_of_bounds_area` or either of its return paths. Script caller identity must be observed before applying a script-specific exemption.

The bytecode interpretation uses the [Luau instruction format](https://github.com/luau-lang/luau/blob/master/Common/include/Luau/Bytecode.h) and [loader structure](https://github.com/luau-lang/luau/blob/master/VM/src/lvmload.cpp) as references, with the engine's loader and VM layouts traced independently. Format similarity does not establish compatibility with an arbitrary Luau compiler version. Game script payloads are not distributed with CRML.

## Recovery trace

Experimental builds in [Wasm movement mode](movement.md) write `crml/fall-recovery.jsonl` when the recovery and transform hooks pass the executable and entry checks. The startup log reports whether the trace and boundary guard are active. Observation reads do not write components or clear fades. The separate guard can omit the inactive player callback and selected script entry calls while flight is active, as described below; other originals retain their arguments and return behavior.

The trace records paired snapshots around inactive, trigger, active-recovery and camera updates. `controller` records changes between character-controller observations, including the teleport flag. Each event includes the stage, thread, timestamp, player/source handles, recovery flags, safe/current positions and timing fields. Camera events also include a raw fade request when its pending flag is set; an empty request has no initialized payload and is reported as `null`. Continuous position and timer changes do not emit an event by themselves. The first matched invocation of each hook is included as a baseline. Safe-position coordinates are meaningful only when `safe_valid` is set.

Schema 2 also observes the transform writers at `0x1811d60`, `0x1811fe0` and `0x18122b0`. Their five-pointer entity view is matched against the player's world, full entity generation, chunk, row and `WorldTransform` component. Events are emitted for a position change of at least two engine units within one call, a tracked state change, or the first matched call. Smaller ordinary motion still contributes to the matched-call counters.

Transform events add an `origin` object with the requested position, immediate caller RVA and up to eight executable-relative native return addresses. Requested coordinates follow that writer's input convention and are not necessarily world coordinates. The stack is captured only for emitted events, while the caller remains active. Addresses outside the main executable are omitted; no absolute code pointers are logged. These are native stack frames, not Lua script names or line numbers.

Schema 3 adds `script_copy_world_transform` around the `nl_copy_world_transform` binding. It preserves the original argument, return value and error behavior. Each observation spans the original binding call and uses the same player displacement/state-change threshold as transform events, plus a first-call baseline. Calls targeting other entities can contribute to the counter without changing the player. A binding event and nested writer event may describe the same relocation; they are not two separate teleports.

The binding event's `origin.lua` contains `valid`, `truncated` and up to four Lua frames in innermost-first order. Each frame has a source basename, function debug label, function definition line and prototype index. Definition lines are not current instruction lines. C closures are omitted. Source directories and arbitrary chunk text are never logged; stripped, unsupported or overlong labels are empty. An empty label does not identify a script. `valid: false` means the reader could not obtain a bounded snapshot; that also increments the binding stage's `unreadable` counter. No engine pointers are retained or serialized.

The build-specific reader inspects at most 16 call frames on the active VM thread, before forwarding the binding. It follows `lua_State+0x20`/`+0x40` (current/base call frame), a `0x28` frame stride with function value at `+0x08`, and Lua closure prototype at `+0x18`. Prototype source/debug-name pointers are at `+0x58`/`+0x60`, with definition line/index at `+0xa4`/`+0xa8`. These offsets require the checked executable fingerprint; they are not a portable scripting API. The snapshot is local to the invocation, so nested calls or script error exits cannot leave stale caller metadata for a later write. Binding calls that exit through a VM error do not produce an after-snapshot.

The `nl_teleport` binding at `0x1a11a20` calls `0x1813be0`, which reaches `0x1811d60`, and then sets the `Teleported` component. Consequently, the component's flag may still be clear in the transform writer's after-snapshot. Correlate the writer event with the subsequent controller observation instead of requiring the flag to be set during the write. A teleport without observed recovery activation requires tracing its producer; the teleport flag alone does not identify fall recovery. The three writer hooks do not cover arbitrary direct component writes or structural replacements.

For a matched producer invocation, a before/after change from flags bit 0 clear to set narrows activation to that invocation. Trigger observations cover one trigger update, which can process multiple targets. Controller observations span time between callbacks and do not identify a producer. Event sequence numbers describe enqueue order; callbacks on different threads can overlap. Component reads are generation checked and guarded, but are not an atomic engine snapshot.

The trace follows the recently observed player for up to 500 ms after its last controller sample, independently of whether flight remains active. It uses a 128-event buffer, a 2,048-event session limit and the movement recorder's 600-report recording window. Contention or overflow increments `dropped`; failed snapshots increment each stage's `unreadable` counter. `identity_unavailable` counts hook calls without a fresh player identity, including sampling-lock contention. An absent event does not prove a path was unused: consider counters, freshness, hook coverage and other transition producers. Non-finite float fields are written as JSON `null`. Logs contain local gameplay positions and entity handles and should be reviewed before sharing.

## Flight boundary guard

Schema 4 reports mode `flight-boundary-guard` when the additional protected-call hook starts, otherwise `observe-only`. The guard follows the active movement lease, checked player generation/world, foreground focus and controller eligibility. With no active lease it passes calls through. It does not install the legacy monitor, camera or active-recovery overrides.

The protected-call entry at `0x2c4fc90` provides the target closure before script execution. The guard recognizes only these functions from `out_of_bounds_area.lua`:

| Function | Definition line | Prototype index | Instruction words |
| --- | --- | --- | --- |
| `on_exit_oob` | 77 | 5 | 90 |
| `on_invalid_area_enter` | 105 | 7 | 34 |

All four identity fields must match. The VM must be running in the active player's world, the call must request zero results with error-handler index 1, and the function/argument range must fit the checked stack. The existing error-handler slot remains intact. A matched call returns success with its function and arguments removed from the stack, without entering a callee frame. Unsupported shapes, unreadable metadata, C closures and other script functions follow the original protected call. This behavior is tied to the executable fingerprint and is not an interface for third-party Lua mods.

This prevents new scripted boundary effects and delayed returns at their entry point. `on_enter_oob`, `update_transition`, timed callbacks and completion handlers still execute; effects or returns already scheduled before flight starts are allowed to finish. The guard neither patches script assets nor clears global fog, exposure or blur parameters.

The native inactive-player callback is also omitted while flight owns that player and recovery has not begun. This prevents the safe-height check from activating recovery and leaves its stored safe history unchanged during flight. Already active recovery, native trigger recovery, transition execution and camera cleanup continue normally. Other transition producers may still teleport the player and cancel flight. Releasing the lease restores normal callback processing; it does not synthesize missed boundary trigger events or move the player back inside the level.

`boundary_guard` records contain `script_exits_skipped`, `invalid_area_entries_skipped`, `height_checks_skipped`, `calls_during_flight` and `read_failures`. These count interception, not visible success. The trace's inactive-stage matched count includes visits omitted by the guard. Snapshots and counters continue to provide evidence if a different producer starts recovery.

For a boundary check, start with no fade or return in progress, enable flight before crossing the boundary, cross it, and remain outside briefly. A covered entry should no longer start boundary fog or snap the player back. Return to solid ground before disabling flight. If a fade or teleport still occurs, allow it to finish before exiting so the capture includes its completion path.
