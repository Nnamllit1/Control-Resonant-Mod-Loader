---
title: Native physics property trial
description: Select a nearby prop, temporarily change its linear damping, and inspect restoration and lifetime events in CONTROL Resonant.
---

# Native physics property trial

The physics trial temporarily sets one nearby prop's linear damping to `8`, then restores its original value after five seconds. This is an opt-in native diagnostic mode. It suspends Wasm mods and other gameplay features for the session; the read-only engine observer remains a separate mode.

## Installation

Build the diagnostic runtime, close the game, and install the trial:

```powershell
.\build.bat -EngineObserver -Test -Jobs 4
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
python tools/install.py "$gameDir" --update --physics-trial --apply
```

Omit `--update` for a new installation. Switching from the engine observer removes its owned marker. Both the executable and physics backend must match the supported fingerprints; a mismatch prevents the trial from starting. Conflicting observer and trial markers prevent either mode from starting.

To return to ordinary startup, close the game and run:

```powershell
python tools/install.py "$gameDir" --update --disable-physics-trial --apply
```

## Controls

Launch through Steam and load a playable save. Stand beside a loose, movable prop and use the in-game panel:

| Key | Action |
| --- | --- |
| **F9** | Select the nearest eligible prop within two world units of the player |
| **F10** | Set the selected prop's linear damping to `8` for five seconds |
| **F11** or **Esc** | Request immediate restoration, or cancel an unused selection |

Stay still while the panel shows **Searching nearby props**. Selection searches the body table across physics callbacks, checking at most 4,096 slots per callback and yielding after approximately two milliseconds. Tables up to 1,048,576 slots are supported. Moving more than a quarter world unit, a scene or body-lifetime change, a table-size change, or a search lasting 15 seconds cancels the search. Press **F9** to retry.

Completed selection expires after 15 seconds. Stand closest to the prop you intend to change. The search compares all eligible props and selects the nearest; if the closest two distances differ by less than 0.1 world units, the panel asks you to move closer to one and retry. Distances use entity origins and the player's position at the start of the search, rather than the camera crosshair or mesh surfaces.

The selector excludes the player, entities with a character-controller component, character-attached items (`ItemAttached`), instances with multiple bodies, and alternate damping storage. It rechecks the selected body's identity and proximity after the search before enabling application. Player, controller and attachment exclusions are also checked on subsequent property accesses. Unreadable component metadata prevents access. The panel distinguishes no nearby candidate, nearly equal distances, a changed selection, and a timed-out search.

Give the prop a brief push, then stop touching it and watch its free slide. Repeat during the five-second trial on the same surface with a similar push. With increased linear damping, expect a shorter slide and quicker loss of speed. The trial does not change friction, mass or angular damping, so continued spinning is not a failure. Continuous pushing, an object wedged against geometry, or an object already at rest makes this comparison inconclusive.

The panel reports when the trial is active and when it finishes. To check manual restoration separately, press **F11** about one second after **F10**: the active trial should finish before the five-second timeout, and subsequent pushes should have the original response. Restoration does not recover momentum already lost. A finished trial can also include retirement or a conflicting game change; the log distinguishes these outcomes.

The runtime samples the selected body's linear and angular speed at up to 10 Hz before application, during the trial, and for five seconds after it finishes. Busy or unreadable samples are marked as unavailable. This helps distinguish translation from rotation: the trial changes linear damping only. A comparison under different player contact or collision forces does not isolate damping's effect.

Losing focus, losing the worker heartbeat, or reaching the diagnostic session limit requests restoration. Restoration runs in a subsequent physics callback, so pausing or loading can delay it. A different value written by the game is preserved instead of overwritten. Scene destruction and body release invalidate the corresponding selection; the runtime does not restore into a replacement body. These controls do not save or reload the game.

To inspect lifetime handling across a reload, select a disposable prop with **F9**, run a trial with **F10**, and wait for it to finish. Reload the save at your own pace. After loading, **F10** alone should have no effect; use **F9** to select a fresh prop before starting another trial. The runtime retains a separate read-only identity watch after restoration or cancellation, until that body is released, its scene is destroyed, or another successful selection replaces the watch. This watch neither keeps the body alive nor permits further writes.

## Logs and reports

Each session writes `crml/physics-trial-<pid>-<tick>.jsonl`. Logs contain selection identities, native write attempts, readback results, trial outcomes, callback counts, retirement counts and dropped-record counts. Addresses are replaced with session-specific tokens. Logs are capped at 4 MiB; new requests stop after ten minutes or a logging failure, while pending restoration continues through the physics callback.

Each search also records the player and up to 16 distinct nearby entities, including full entity/body identities, positions, local body counts and component hashes. Multiple bodies belonging to one entity share one diagnostic record. Candidate `exclusion` values are `0` (eligible), `1` (player), `2` (character controller), `3` (body-count or local-index restriction), `4` (alternate damping storage), `5` (character-attached item), and `6` (unreadable entity metadata). These read-only records help distinguish nearby props from player-associated entities. They do not establish the attachment's owner by themselves. `target_dropped` reports losses from the separate bounded diagnostic queue.

Generate a report locally:

```powershell
$capture = Read-Host 'Path to a physics-trial JSONL capture'
New-Item -ItemType Directory -Force .local | Out-Null
python tools/analyze_physics_trial.py "$capture" --output .local/physics-trial-report.json
```

The report separates property restoration from visible gameplay effects. `restoration_observed` requires a successful native restoration write and a restoration outcome. Missing records or an unfinished trial are not proof of successful cleanup. The JSONL write event's `before` and `after` values are the expected old value and requested new value; only a successful status confirms their readback.

`motion_summary` groups valid speed samples by phase; its maxima describe observed movement, not proof of a causal effect. `controls` records button edges and requests consumed by the physics callback (`1`: select, `2`: apply, `4`: restore/cancel). Callback restoration requests also include focus loss and session shutdown. Trial-mode input is polled every 10 ms.

`lifetime_events` records entry into the watched body's release or scene-destruction hook (`action: 6`, result `0` or `1`). Each event retains the original selection, scene token, entity and full body handle, even after the writable selection is cleared. The first matching hook consumes the watch. `lifetime_missed` counts diagnostic lock contention; event-buffer losses remain in `dropped`. A missing event does not prove that the body survived. A retirement event does not by itself prove restoration or completion of destruction, and the older `retirements` counter tracks only the writable selection's watcher.

## Runtime behavior

The trial runs after the original physics simulation dispatcher returns and before its ECS task releases successors. Each access resolves the current callback's world, scene, full entity handle and full body handle again. The native setter is called only after identity, virtual-target, getter and attached-scene checks; readback checks the resulting linear damping and unchanged angular damping. The setter's busy-scene check is not a lock.

The implementation is restricted to the fingerprinted engine paths described in [physics dynamics](physics-dynamics.md). Native trial mode suspends guests. Wasm mods use the bounded [Wasm damping API](api.md#experimental-prop-damping). Use a disposable prop for either mode.

## Wasm damping example

With Alpha 4.2, install the runtime and copy the compiled example into `crml/mods/physics-damping`; its manifest requests the service. Movement and visibility can run alongside it. The Wasm service has no ten-minute session cutoff, and a full or failed diagnostic log does not stop it. Individual operations still expire and restore.

For source builds, or the legacy explicit startup marker:

```powershell
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
python tools/install.py "$gameDir" --update --physics-wasm --apply
Copy-Item -Recurse dist/examples/physics-damping "$gameDir/crml/mods/physics-damping"
```

Copy the example only if that destination does not already exist; preserve any customized package. The installer replaces owned native-trial/observer markers and preserves existing mods. The source package is in `examples/physics-damping`; its compiled Wasm binary is produced by the build. No native mod DLL is loaded.

Launch through Steam and load a save. **Home** asks the guest to select a nearby prop; stay still for a moment before applying. A physics-only session shows **PROP SELECTED**; with movement active its panel takes precedence. The example also logs selection and active state transitions to `crml.log`. **End** asks the guest to apply damping `8` for five seconds. **F11** or **Esc** requests native restoration. These differ from the native trial's F9/F10 controls. Push the prop yourself by briefly walking into it, then step away: expect shorter free sliding while damping is active. The example does not apply a push automatically. Normal response should return after five seconds; rotation is unchanged. Select again before another application.

The guest reads its manifest actions and supplies the search region through `physics_select_near(offset_x, offset_y, offset_z, radius)`. The example chooses zero offset and radius two; mods may choose different bounded regions. It also supplies damping and duration to `physics_apply`. Native code resolves the eligible target and performs the guarded write on the physics callback. The example logs queued requests and missing-target or rejected requests as well as operation status. The same physics-trial JSONL logs record native execution and restoration, with `mods_suspended: false` in the header. F9/F10 do not initiate native trials in this mode.

To return to ordinary startup, close the game and run `python tools/install.py "$gameDir" --update --disable-physics-wasm --apply`. On Alpha 4.2 the installed example itself requests physics support. Remove its mod folder to disable it; removing only the marker does not disable manifest requests.
