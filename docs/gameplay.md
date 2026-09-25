# Gameplay and noclip

**Status: experimental. Wall/ceiling traversal and partial improvement of out-of-bounds behavior have been reported in gameplay. A remaining engine teleport cancelled flight in the recorded session. The latest position-restoration change targets that reset and needs in-game verification.**

The prototype targets the controlled character through the game's character-controller movement routine. Private per-call arguments supply the requested transform, select keyframed movement, and suppress the subsequent contact/push pass. Flight retains its requested position across small ground corrections. Original component values stay intact, so subsequent normal calls resume movement. Native tests cover argument isolation and automatic cancellation; they do not replace in-game traversal tests.

## Build and install for testing

```powershell
.\build.bat -ExperimentalGameplay -Test
python tools/install.py "C:\Games\CONTROL Resonant" --update --experimental-noclip
python tools/install.py "C:\Games\CONTROL Resonant" --update --experimental-noclip --apply
```

Replace the path with your installation. Close the game first. Omit `--update` for a fresh installation. This installs the sandboxed `noclip` example and `crml/noclip.enabled`. Normal builds omit the native hook. Experimental builds still require the enable file and a mod requesting `player.noclip`; merely loading the runtime does not activate flight.

## Controls and test sequence

Use a disposable save or a backed-up save for the first test. Launch normally through Steam and load a playable area. The status panel is rendered into the DirectX 12 back buffer. Automated tests verify its pixels, late initialization, resizing, and visibility; Steam screenshots and fullscreen behavior still require testing in the game.

| Control | Action |
| --- | --- |
| F6 | Toggle noclip; starts off |
| W / S | Forward / backward along the camera's horizontal heading |
| D / A | Right / left relative to the camera |
| Space / Ctrl | Up / down |
| Shift | Three times the base speed |
| Esc | Cancel noclip |

The example requests 5 world units per second. Looking up or down does not change altitude when using WASD; Space/Ctrl controls altitude separately. If the camera cannot be validated, the panel indicates that only vertical movement is available. The status panel is informational and does not take mouse or keyboard focus. Controller support, a clickable panel, and a speed slider remain future work. Camera collision can still pull the view around when the player passes through geometry.

While noclip is active, the bridge consumes WASD, Space, Ctrl, and Shift before they reach normal keyboard actions. Space should raise the player without jumping, and Ctrl should lower them without activating its normal action. Mouse look, F6, Escape, and other keys remain available. This filtering ends with the movement lease; it does not change the operating system's physical keyboard state or add gamepad support.

The bridge also bypasses the player's fall-recovery checks and excludes that player from boundary-reset trigger targets while noclip is active. The fall check does not record new safe positions during flight. NPCs remain under game control. Return to a safe area before switching noclip off: normal fall recovery resumes immediately, including resets when the player is still outside the level. This is not invulnerability or a bypass for every scripted level restriction.

Once noclip has acquired a position, it restores its requested position when the same player is teleported within the same world. This also overrides intentional teleports: **turn noclip off before fast travel or scripted travel**. The controller call receives a private cleared teleport flag so it follows noclip movement; the real engine flag is left intact. The first flight update cannot adopt an already-teleporting player. Player/world replacement, stale heartbeats, disabled controllers, and engine keyframing still cancel flight.

The independent player fall monitor, its fall-action trigger, and the active fall-recovery routine are bypassed during flight. The two fall values exposed to game scripts are set to zero on the monitor's game-thread call. The fall-camera updater receives a private copy of recovery data with its active bit cleared, allowing its native fade-out and camera cleanup to run. Normal monitoring resumes after noclip ends. Ambient fog and the global fade renderer remain unchanged; disappearance of the reported white fog has not yet been confirmed.

The movement hook waits for the short state lock instead of allowing a normal collision update when diagnostics hold it. Cancellation snapshots record the reason and coordinates so a transient teleport or large correction remains visible after the next frame.

1. Confirm the overlay shows OFF. If it stays UNAVAILABLE, inspect `crml/crml.log` and `crml/movement-probe.jsonl` instead of repeatedly toggling.
2. Press F6 and move a short distance in open space. Rotate the camera and check all four WASD directions. Check ascent and descent, including descending through a floor and returning upward through it.
3. Hold Space and Ctrl separately and confirm their normal character actions do not activate. Check mouse look and try toggling F6 while a movement key is held.
4. Fly below the level and across a boundary that previously reset the player. Check that noclip remains active and that the white-fog effect no longer builds up. Return to open space above solid ground, then press F6 again and check walking, jumping, gravity, collision, and normal falling.
5. Test Escape, alt-tab, pause/resume, and save reload. Report the first step that fails along with both logs. Do not save while inside geometry or outside the level.

The native bridge cancels its override on focus loss, Escape, a missing mod heartbeat for 500 ms, changed player/world identity, engine keyframing, an unflagged displacement exceeding 5 units from the requested position, or a controller-update gap exceeding 250 ms. A teleport before the first flight position is acquired also cancels it. Cancellation returns control to the game at the current position; it does not rewind to the activation point. Menus, death, cutscenes, and streaming transitions still require live testing.

## Diagnostics and disabling

The bridge checks the executable SHA-256, hook bytes, entity generation, player tag, component addresses, and controller ownership. A failed check leaves the original call in place. The bounded diagnostic log records counters, sampled coordinates, and controller flags for up to ten minutes. It is overwritten at the next start.

The runtime handle comes from the entity chunk header. The `GlobalID` component identifies persistent content and is not interchangeable with that handle. Earlier experimental builds confused these values, causing every sample to be rejected even though the Steam launch successfully loaded the mod.

Diagnostic schema 7 includes validation counters, graphics status, camera availability, input/reset filtering, the most recent movement result, cancellation evidence, and teleport restoration:

| Field or value | Meaning |
| --- | --- |
| `player_samples` | Valid observations of the player |
| `input_consumed` | Keyboard messages or raw key-down events converted to releases or neutral messages |
| `fall_checks_skipped` | Player fall-recovery updates bypassed during noclip; these are checks, not confirmed reset attempts |
| `boundary_targets_skipped` | Valid player targets excluded from boundary-reset trigger queries |
| `fall_monitors_skipped` | Player fall-monitor calls bypassed with script-visible fall values neutralized |
| `fall_actions_skipped` | Player fall-action trigger updates bypassed during noclip |
| `active_recoveries_skipped` | Active recovery routine calls bypassed; not necessarily pending resets |
| `fall_camera_overrides` | Calls using an inactive recovery view for native fall-camera cleanup |
| `fall_camera_clear_requests` | Overrides that found the fall camera already active |
| `last_stop.reason` / `count` / `tick_ms` | Most recent noclip cancellation, total cancellations, and system-uptime timestamp |
| `last_stop.requested` / `observed` | Requested flight position and observed player position when cancelled |
| `teleport_restores.count` / `tick_ms` | Controller updates restoring a displaced, teleport-flagged player and latest system-uptime timestamp |
| `teleport_restores.requested` / `observed` | Flight target retained and displaced position observed before the correction; use `last_override.controller_result` to check the result |
| `camera_valid` / `camera_entity` | Validated camera basis used for horizontal movement |
| `last_override.target` | Requested noclip position |
| `last_override.controller_result` | Controller position read after the movement routine returned |
| `last_override.valid` / `age_ms` | Whether that read succeeded and how old it is |
| `rejections.generation` / `location` / `layout` | Identity or component validation failed |
| `rejections.memory` | A diagnostic read encountered unreadable memory |
| `overlay.presents` | Intercepted non-test presentation calls |
| `overlay.queue_matches` | Observed back-buffer transitions submitted on a direct queue |
| `overlay.frames` | Panel copies submitted to the GPU |
| `waiting_for_present` | No eligible presentation intercepted yet |
| `waiting_for_backbuffer_queue` | Waiting to identify the buffer's rendering queue |
| `rendering` | Panel submitted successfully |

An increasing `overlay.frames` counter confirms submission, not visibility in a Steam screenshot. Include both logs when reporting missing UI. Unsupported graphics paths leave the game rendering unchanged.

To disable the feature without removing the loader, close the game and rename `crml/noclip.enabled` to `noclip.disabled`. To disable the diagnostic hook too, rename `crml/movement-probe.enabled` if present. Restore those filenames before using receipt-based removal or update, since the installer intentionally refuses modified installations.

For observation without a movement feature, build with `-MovementProbe` (an alias for `-ExperimentalGameplay`) and create only `crml/movement-probe.enabled`. This patches the routine for observation but makes no gameplay-state changes.

## Remaining integration work

1. Verify camera-heading controls, floor traversal, keyboard isolation, boundary triggers, and white-fog suppression in gameplay.
2. Validate restoration of normal walking, jumping, gravity, and collision.
3. Verify transitions across menus, loading, death, cutscenes, and player replacement.
4. Improve camera collision behavior and add a bounded UI control API.
5. Mark the compatibility profile supported only after live testing passes.

## UI direction

The current UI is a small native DirectX 12 status panel with a keyboard toggle and an unavailable state. It draws into the game image rather than a desktop window. Later APIs can support interactive panels and selected game-UI manipulation. Arbitrary native callbacks, unrestricted JavaScript/Lua evaluation, and raw UI pointers are not mod APIs.

No engine offsets or function signatures are inferred from the original Control. Free-camera strings and component names in a binary are investigation leads, not sufficient evidence for calling a function or changing player state.
