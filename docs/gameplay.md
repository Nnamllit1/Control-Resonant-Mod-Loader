# Gameplay and noclip

**Status: experimental prototype; live noclip behavior is unverified.**

The prototype targets the controlled character through the game's character-controller movement routine. It uses a private copy of the current call's transform and keyframed-mode arguments. It leaves the original entity components untouched and lets subsequent normal calls resume movement. Native tests cover this argument isolation and automatic cancellation; they do not prove that the game traverses walls correctly.

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
| W / S | Positive / negative world Z |
| D / A | Positive / negative world X |
| Space / Ctrl | Up / down |
| Shift | Three times the base speed |
| Esc | Cancel noclip |

The example requests 5 world units per second. Movement is relative to world axes, **not the camera**. The status panel is informational and does not take mouse or keyboard focus. Camera-relative steering, controller support, a clickable panel, and a speed slider remain future work.

1. Confirm the overlay shows OFF. If it stays UNAVAILABLE, inspect `crml/crml.log` and `crml/movement-probe.jsonl` instead of repeatedly toggling.
2. Press F6 and move a short distance in open space. Check ascent and descent before approaching a wall.
3. Cross a nearby wall, return to open space above solid ground, and press F6 again. Check walking, jumping, gravity, and collision afterward.
4. Test Escape, alt-tab, pause/resume, and save reload. Report the first step that fails along with both logs. Do not save while inside geometry or outside the level.

The native bridge cancels its override on focus loss, Escape, a missing mod heartbeat for 500 ms, changed player/world identity, engine keyframing, or a controller-update gap exceeding 250 ms. Cancellation returns control to the game at the current position; it does not rewind to the activation point. Menus, death, cutscenes, and streaming transitions still require live testing.

## Diagnostics and disabling

The bridge checks the executable SHA-256, hook bytes, entity generation, player tag, component addresses, and controller ownership. A failed check leaves the original call in place. The bounded diagnostic log records counters, sampled coordinates, and controller flags for up to ten minutes. It is overwritten at the next start.

The runtime handle comes from the entity chunk header. The `GlobalID` component identifies persistent content and is not interchangeable with that handle. Earlier experimental builds confused these values, causing every sample to be rejected even though the Steam launch successfully loaded the mod.

Diagnostic schema 2 adds `rejections` counters for failed validation stages and an `overlay` object:

| Field or value | Meaning |
| --- | --- |
| `player_samples` | Valid observations of the player |
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

1. Verify the movement hook's live arguments against the external player-position observations.
2. Validate that the keyframed movement path provides actual noclip and restores normal movement.
3. Verify transitions across menus, loading, death, cutscenes, and player replacement.
4. Add camera-relative movement and a bounded UI control API.
5. Mark the compatibility profile supported only after live testing passes.

## UI direction

The current UI is a small native DirectX 12 status panel with a keyboard toggle and an unavailable state. It draws into the game image rather than a desktop window. Later APIs can support interactive panels and selected game-UI manipulation. Arbitrary native callbacks, unrestricted JavaScript/Lua evaluation, and raw UI pointers are not mod APIs.

No engine offsets or function signatures are inferred from the original Control. Free-camera strings and component names in a binary are investigation leads, not sufficient evidence for calling a function or changing player state.
