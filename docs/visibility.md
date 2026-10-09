---
description: Explore the experimental CONTROL Resonant Wasm visibility mod, its renderer connection, capability requirements, and controls.
---

# Experimental player visibility

The `visibility` Wasm example hides the player's root mesh while F7 is held. It changes rendered visibility only; it does not affect collisions, AI awareness, or gameplay invisibility. Separately rendered equipment and effects may remain visible.

Build and preview installation:

```powershell
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
.\build.bat -ExperimentalGameplay -Test
python tools/install.py "$gameDir" --update --experimental-visibility
```

Close the game and add `--apply` to install. Omit `--update` for a fresh installation. The installer owns `crml/visibility.enabled` and the example under `crml/mods/visibility`. Inspector capture can remain enabled alongside visibility changes. Alpha 4.2 can run visibility alongside movement and physics. The marker is retained for older installations; new mods can request `player.visibility` directly in their manifest.

Load a playable save, hold F7, then release it. The expected result is a hidden root mesh while held, followed by normal engine visibility on release. Also check Escape and focus loss. The Wasm input-to-render path is experimental. A renewed lease alone does not confirm a rendered result. The movement log's `visibility_submissions` counter reports submitted hide commands.

The native bridge runs after `coregame::mesh::applyHide`, preserves the game's own hide reasons, updates its cached hidden flag, and submits the observed single-handle renderer command. On release the original system recomputes visibility on its next eligible update. There is no saved pointer or saved visibility state to restore into a different entity. If gameplay is paused and the mesh system stops updating, restoration waits for it to resume.

Removing the `visibility` mod folder disables its heartbeat, but restore installer-owned files before a receipt-validated update or uninstall. To remove the complete loader, use the installer's `--uninstall --apply` command with the game closed. Logs and unrelated files are preserved.

## What runs in Wasm

The Photo Visibility example also checks `visibility_read` before renewal. It
stops on lease expiry and reports when the adapter has observed a hidden mesh.
The API distinguishes an accepted lease from hidden-component evidence and
command publication. These flags do not establish completed rendering or
visibility restoration; see the
[lease observation contract](api.md#visibility-lease-observations).

The example imports `input_buttons` and `visibility_set`. Its Wasm code reads the bounded input mask, chooses F7, and sends a boolean visibility request. Change `CRML_BUTTON_F7` to `CRML_BUTTON_F8` in `visibility.c` to select F8 without rebuilding the native runtime. C is the normal build input; the WAT file remains a reference fixture.

The runtime implements the engine-facing operation: it validates capability and ownership, expires stale requests, and applies the native renderer command on the correct engine phase. Wasm never writes engine memory directly. The older example only called `visibility_poll`, leaving both the F7 decision and renderer operation in native code; that import is retained for compatibility.
