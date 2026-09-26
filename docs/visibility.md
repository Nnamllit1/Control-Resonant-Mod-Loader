# Experimental player visibility

The `visibility` Wasm example hides the player's root mesh while F7 is held. It is an initial renderer manipulation experiment; it does not affect collisions, AI awareness, or gameplay invisibility. Separately rendered equipment and effects may remain visible.

Build and preview installation:

```powershell
.\build.bat -ExperimentalGameplay -Test
python tools/install.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant" --update --experimental-visibility
```

Close the game and add `--apply` to install. Omit `--update` for a fresh installation. The installer owns `crml/visibility.enabled` and the example under `crml/mods/visibility`. Inspector capture can remain enabled, but this session is no longer read-only. Noclip is disabled while visibility mode is installed.

Load a playable save, hold F7, then release it. The expected result is a hidden root mesh while held, followed by normal engine visibility on release. Also check Escape and focus loss. The native hide/show operation has been reported working in gameplay. The revised guest-controlled input path still needs live verification; a renewed lease alone does not confirm a rendered result. The movement log's `visibility_submissions` counter reports submitted hide commands.

The native bridge runs after `coregame::mesh::applyHide`, preserves the game's own hide reasons, updates its cached hidden flag, and submits the observed single-handle renderer command. On release the original system recomputes visibility on its next eligible update. There is no saved pointer or saved visibility state to restore into a different entity. If gameplay is paused and the mesh system stops updating, restoration waits for it to resume.

Removing the `visibility` mod folder disables its heartbeat, but restore installer-owned files before a receipt-validated update or uninstall. To remove the complete loader, use the installer's `--uninstall --apply` command with the game closed. Logs and unrelated files are preserved.

## What runs in Wasm

The example imports `input_buttons` and `visibility_set`. Its Wasm code reads the bounded input mask, chooses F7, and sends a boolean visibility request. Change the mask from 1 to 2 in `visibility.wat` to select F8 without rebuilding the native runtime. `visibility.c` is the equivalent SDK example.

The runtime implements the engine-facing operation: it validates capability and ownership, expires stale requests, and applies the native renderer command on the correct engine phase. Wasm never writes engine memory directly. The older example only called `visibility_poll`, leaving both the F7 decision and renderer operation in native code; that import is retained for compatibility.
