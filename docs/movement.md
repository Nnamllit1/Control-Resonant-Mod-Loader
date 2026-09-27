---
title: Guest-controlled movement
description: Build a Wasm movement mod with explicit velocity requests, camera heading, bounded keyboard input, and native cleanup.
---

# Guest-controlled movement

The `movement` example owns its toggle, controls, speed and camera-relative calculation in Wasm. The native service accepts a bounded world-space velocity and applies noncolliding character movement on the controller callback. This is an experimental alternative to the legacy `noclip_poll` helper.

This mode suppresses the normal WASD, Space, Ctrl and Shift keyboard actions while the lease is active. It does not install the legacy fall/reset overrides or change camera position. Out-of-bounds recovery can still run, and a teleport cancels movement. A [read-only recovery trace](fall-recovery.md) records activation and fade state when its hooks are available. Camera collision can still change the view when traversing geometry. It is not a finished free-camera or boundary-free flight feature.

## Installation

Build, close the game, then switch modes:

```powershell
.\build.bat -ExperimentalGameplay -Test
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
python tools/install.py "$gameDir" --update --movement-wasm --apply
Copy-Item -Recurse dist/examples/movement "$gameDir/crml/mods/movement"
```

Omit `--update` for a fresh installation. Copy the example only if the destination does not exist; preserve customized packages. The installer removes owned observer and physics-mode markers. It preserves the other mods and settings, but this mode does not start their visibility, physics-property, or legacy noclip services. Conflicting isolated-mode markers refuse startup. Unknown executable fingerprints leave the game unchanged and do not start guests in this mode.

## Controls and expected behavior

Start in an open area with solid ground beneath the player. The panel should show **MOVEMENT MOD: OFF** once the player is available.

| Control | Expected behavior |
| --- | --- |
| F6 | Toggle movement; starts off |
| W / S | Move forward/backward along the camera's horizontal heading |
| A / D | Strafe left/right relative to that heading |
| Space / Ctrl | Move up/down without normal jump or crouch actions |
| Shift | Increase speed from 5 to 15 world units per second |
| Esc, focus loss | Cancel movement and return control to the game |

Try short movements in open space, rotate the camera, and repeat all four horizontal directions. Diagonal travel should not be faster. Release the movement keys while active: the character should hold position. Toggle off above safe ground and confirm normal walking and jumping return. For a collision check, cross a nearby low obstacle and return; keep the initial check inside the level, since boundary recovery is deliberately still active.

When camera heading is unavailable, this example requests only vertical motion. When a teleport, player/world replacement, stale sample or missed heartbeat cancels the lease, the example turns itself off; press F6 again after normal gameplay resumes. A camera snapshot describes observed orientation, not exclusive ownership of the game's camera.

## Mod source and API

`examples/movement/movement.wat` is compiled by the build; `movement.c` is equivalent readable C source for Clang's wasm32 target. Both live beside the manifest. The guest reads fixed buttons, calculates normalized world velocity, and calls `motion_set`. To change controls or speed, edit and rebuild the guest. The native runtime does not interpret F6 or WASD as movement commands in this mode.

The service performs player and generation checks, enforces the speed and lease bounds, creates private controller-call arguments, and suppresses the character's contact/push pass. It does not overwrite the original transform/keyframing argument arrays. Native cleanup remains available after a guest trap or shutdown.

See the [API contract](api.md#experimental-movement-requests) for result semantics, capabilities, and limits.

## Diagnostics and removal

`crml.log` identifies the service and loaded mod. `movement-probe.jsonl` uses diagnostic schema 7 with mode `wasm-movement`. `motion_requests` counts accepted set/release requests; `motion_velocity` records the sampled requested world velocity while active. `overrides` counts controller-call substitutions. `last_override` compares the requested position with the controller's result, and `last_stop` records cancellation. Input counters indicate filtering, not by themselves proof that every gameplay action was suppressed. The fall/reset override counters should remain zero in this mode.

The capture samples once per second for up to ten minutes and is replaced on the next launch. To remove this mode, close the game and run `python tools/install.py "$gameDir" --update --disable-movement-wasm --apply`. Ordinary startup then follows any remaining feature markers. To switch directly back to the bounded physics example, use `--physics-wasm` instead.
