# Guest-controlled character movement

The complete mod policy lives here: F6 toggle, WASD/Space/Ctrl axes, camera-relative conversion, diagonal normalization and 5/15-unit speed selection. `movement.wat` is the source used by the normal build. `movement.c` is an equivalent C alternative; editing C alone does not change the WAT-built package.

Rebuild just the guest from the repository root:

```powershell
.\dist\crml\crml_wat.exe examples/movement/movement.wat dist/examples/movement/movement.wasm
```

Or compile the C source using Clang with wasm32 and `wasm-ld` support:

```powershell
clang --target=wasm32 -O2 -nostdlib -Isdk/include examples/movement/movement.c -Wl,--no-entry -Wl,--export-memory -Wl,--initial-memory=131072 -Wl,--max-memory=16777216 -o dist/examples/movement/movement.wasm
```

The host exposes fixed button input, two floats describing horizontal camera right, and a world-velocity request. It checks ownership, player identity, request bounds and expiry before applying the controller override. Zero velocity holds position; disabling the request releases control. The example switches off when a request fails, and the runtime releases ownership even if guest shutdown cannot run.

The native movement service includes a limited boundary guard while the flight lease is active. Pending recovery and other transition producers can still cancel movement. The example does not move the camera independently. See [installation, controls and limitations](../../docs/movement.md).
