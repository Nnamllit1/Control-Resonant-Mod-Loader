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
| **F9** | Select the single eligible prop within two world units of the player |
| **F10** | Set the selected prop's linear damping to `8` for five seconds |
| **F11** or **Esc** | Request immediate restoration, or cancel an unused selection |

Stay still while the panel shows **Searching nearby props**. Selection searches the body table across physics callbacks, checking at most 4,096 slots per callback and yielding after approximately two milliseconds. Tables up to 1,048,576 slots are supported. Moving more than a quarter world unit, a scene or body-lifetime change, a table-size change, or a search lasting 15 seconds cancels the search. Press **F9** to retry.

Completed selection expires after 15 seconds. A selection request fails if multiple eligible props are nearby; move closer to an isolated prop and try again. The selector excludes the player, entities with a character-controller component, instances with multiple bodies, and alternate damping storage. It rechecks the selected body's identity and proximity after the search before enabling application.

Gently move the prop before and during the trial to compare how quickly its motion slows. Damping does not change friction or mass, and its effect may be hard to see on an object already at rest. The panel reports when the trial is active and when it finishes. A finished trial can include retirement or a conflicting game change; the log distinguishes these outcomes.

Losing focus, losing the worker heartbeat, or reaching the diagnostic session limit requests restoration. Restoration runs in a subsequent physics callback, so pausing or loading can delay it. A different value written by the game is preserved instead of overwritten. Scene destruction and body release invalidate the corresponding selection; the runtime does not restore into a replacement body. These controls do not save or reload the game.

## Logs and reports

Each session writes `crml/physics-trial-<pid>-<tick>.jsonl`. Logs contain selection identities, native write attempts, readback results, trial outcomes, callback counts, retirement counts and dropped-record counts. Addresses are replaced with session-specific tokens. Logs are capped at 4 MiB; new requests stop after ten minutes or a logging failure, while pending restoration continues through the physics callback.

Generate a report locally:

```powershell
$capture = Read-Host 'Path to a physics-trial JSONL capture'
New-Item -ItemType Directory -Force .local | Out-Null
python tools/analyze_physics_trial.py "$capture" --output .local/physics-trial-report.json
```

The report separates property restoration from visible gameplay effects. `restoration_observed` requires a successful native restoration write and a restoration outcome. Missing records or an unfinished trial are not proof of successful cleanup. The JSONL write event's `before` and `after` values are the expected old value and requested new value; only a successful status confirms their readback.

## Runtime behavior

The trial runs after the original physics simulation dispatcher returns and before its ECS task releases successors. Each access resolves the current callback's world, scene, full entity handle and full body handle again. The native setter is called only after identity, virtual-target, getter and attached-scene checks; readback checks the resulting linear damping and unchanged angular damping. The setter's busy-scene check is not a lock.

The implementation is restricted to the fingerprinted engine paths described in [physics dynamics](physics-dynamics.md). It does not expose a public Wasm property-write API. Use a disposable prop for this diagnostic mode.
