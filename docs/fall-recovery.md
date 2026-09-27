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

This explains a potential partial-reset failure: preventing the return transition while leaving recovery active can leave the fade latched. Clearing a rendered effect alone also leaves its activation and transition state intact. A future movement exemption must account for activation, completion and camera cleanup together.

`heron::player_fall_monitor` is a separate system family with script-visible outputs. Its existence does not establish that it caused a particular recovery. The recovery observer does not suppress that monitor or any other gameplay system.

## Recovery trace

Experimental builds in [Wasm movement mode](movement.md) write `crml/fall-recovery.jsonl` when all four recovery hooks pass the executable and entry checks. The startup log reports whether the trace is active. Each original callback runs with its original arguments; the observer does not write components, alter queries, skip callbacks or clear fades.

The trace records paired snapshots around inactive, trigger, active-recovery and camera updates. `controller` records changes between character-controller observations, including the teleport flag. Each event includes the stage, thread, timestamp, player/source handles, recovery flags, safe/current positions and timing fields. Camera events also include the raw fade request. Continuous position and timer changes do not emit an event by themselves. The first matched invocation of each hook is included as a baseline.

For a matched producer invocation, a before/after change from flags bit 0 clear to set narrows activation to that invocation. Trigger observations cover one trigger update, which can process multiple targets. Controller observations span time between callbacks and do not identify a producer. Event sequence numbers describe enqueue order; callbacks on different threads can overlap. Component reads are generation checked and guarded, but are not an atomic engine snapshot.

The trace follows the recently observed player for up to 500 ms after its last controller sample, independently of whether flight remains active. It uses a 128-event buffer, a 2,048-event session limit and the movement recorder's 600-report recording window. Contention or overflow increments `dropped`; failed snapshots increment each stage's `unreadable` counter. `identity_unavailable` counts hook calls without a fresh player identity, including sampling-lock contention. An absent event does not prove a path was unused: consider counters, freshness, hook coverage and other transition producers. Non-finite float fields are written as JSON `null`. Logs contain local gameplay positions and entity handles and should be reviewed before sharing.

To capture a recovery, launch with movement mode, walk briefly, enable flight and reproduce the boundary or downward flight that normally causes the reset. Let the game's fade/return finish before exiting. The expected gameplay behavior is unchanged: recovery may still occur and cancel flight. This build adds evidence for selecting an intervention point; it does not implement a recovery exemption.
