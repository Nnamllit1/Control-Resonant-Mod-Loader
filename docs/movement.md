---
title: Guest-controlled movement
description: Build a Wasm movement mod with explicit velocity requests, camera heading, bounded keyboard input, and native cleanup.
---

# Guest-controlled movement

This is a developer reference for the Wasm movement example. For the framework download, use the [runtime installation guide](installation.md). Individual mod downloads provide their own controls and installation instructions.

The `movement` example owns its toggle, controls, speed and camera-relative calculation in Wasm. The native service accepts a bounded world-space velocity and applies noncolliding character movement on the controller callback. This is an experimental alternative to the legacy `noclip_poll` helper.

This mode suppresses the normal WASD, Space, Ctrl and Shift keyboard actions while the lease is active. Its experimental [boundary guard](fall-recovery.md#flight-boundary-guard) prevents two scripted boundary handlers and the native height check from starting new recovery during flight. Already scheduled recovery and other transition producers remain active; a teleport still cancels movement. Camera collision can still change the view when traversing geometry. This mode does not provide free-camera control.

## Installation

With Alpha 4.2, install the runtime and copy the compiled example into `crml/mods/movement`. Its manifest starts the movement service. Other Wasm mods can request visibility and physics alongside it.

For source builds, or the legacy explicit startup marker:

```powershell
.\build.bat -ExperimentalGameplay -Test
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
python tools/install.py "$gameDir" --update --movement-wasm --apply
Copy-Item -Recurse dist/examples/movement "$gameDir/crml/mods/movement"
```

Omit `--update` for a fresh installation. Copy the example only if the destination does not exist; preserve customized packages. The installer removes owned diagnostic and old physics-mode markers while preserving other mods. In Alpha 4.2 their manifests still request services independently. The legacy native noclip helper remains unavailable while explicit movement is selected. Diagnostic capture markers cannot be combined with these explicit gameplay markers. Untested executables require startup consent and must still pass individual native signature checks.

## Controls and expected behavior

Start in an open area with solid ground beneath the player. The panel should show **Movement off** once the player is available.

| Control | Expected behavior |
| --- | --- |
| F6 | Toggle movement; starts off |
| Insert | Show/hide the CRML panel without changing flight; remembered until the game closes |
| W / S | Move forward/backward along the camera's horizontal heading |
| A / D | Strafe left/right relative to that heading |
| Space / Ctrl | Move up/down without normal jump or crouch actions |
| Shift | Increase speed from 5 to 15 world units per second |
| Esc, focus loss | Cancel movement and return control to the game |

Try short movements in open space, rotate the camera, and repeat all four horizontal directions. Diagonal travel should not be faster. Release the movement keys while active: the character should hold position. For a collision check, cross a nearby low obstacle and return. With the boundary guard active, crossing a covered boundary during flight should not start its fog or scripted return. Return above safe ground before toggling off, then confirm normal walking and jumping resume. Turning flight off outside the level can allow normal recovery or falling.

When camera heading is unavailable, this example requests only vertical motion. When a teleport, player/world replacement, stale sample or missed heartbeat cancels the lease, the example turns itself off; press F6 again after normal gameplay resumes. A camera snapshot describes observed orientation, not exclusive ownership of the game's camera.

Activation requires a player sample no older than 100 ms. An existing lease can
renew using a sample up to 250 ms old, matching the controller's maximum step
gap. Renewal publishes intent; live controller checks still run before movement
is applied. A brief frame hitch therefore does not use the stricter activation
cutoff to cancel an otherwise valid lease.

## Mod source and API

`examples/movement/movement.c` is compiled by the build; `movement.wat` remains a low-level reference fixture. Both live beside the manifest. The guest reads fixed buttons, calculates normalized world velocity, and calls `motion_set`. To change controls or speed, edit the C source and rebuild the guest. The native runtime does not interpret F6 or WASD as movement commands in this mode.

The service performs player and generation checks, enforces the speed and lease bounds, creates private controller-call arguments, and suppresses the character's contact/push pass. It does not overwrite the original transform/keyframing argument arrays. Native cleanup remains available after a guest trap or shutdown.

See the [API contract](api.md#experimental-movement-requests) for result semantics, capabilities, and limits.

The current C example requires runtime `0.1.0-alpha.4.4.dev.0` or later. On a
failed renewal it reads `motion_read` and logs the cancellation reason before
cleanup clears it. The WAT fixture preserves the older ABI-only behavior.
See [cancellation snapshots](api.md#movement-cancellation-snapshots) for receipt
lifetime and the distinction between lease acceptance and applied movement.

## Diagnostics and removal

`crml.log` identifies the service, loaded mod and whether the boundary guard started. If its checks fail, movement remains available with normal engine recovery. `movement-probe.jsonl` uses diagnostic schema 7 with mode `wasm-movement`. `motion_requests` counts accepted set/release requests; `motion_velocity` records the sampled requested world velocity while active. `overrides` counts controller-call substitutions. `last_override` compares the requested position with the controller's result, and `last_stop` records cancellation. Input counters indicate filtering, not by themselves proof that every gameplay action was suppressed. The legacy fall/reset counters remain zero; the scoped guard has separate counters in `fall-recovery.jsonl`.

The capture samples once per second for up to ten minutes and is replaced on the next launch. To disable this mod, close the game and remove its package folder. If an installer-owned movement marker remains, remove it with `python tools/install.py "$gameDir" --update --disable-movement-wasm --apply`. On Alpha 4.2 removing a marker does not disable capabilities requested by other installed mods.

`last_stop.source` distinguishes a rejected player sample, keyboard freshness,
a request-side stop and the controller callback. `last_stop.sample_age_ms`
records sample age for request-side stops; `UINT64_MAX` means the sample was
absent or its timestamp was ahead of the sampled clock. Controller-side stops
use zero in that field.
