# Guest-controlled character movement

The complete mod policy lives here: F6 toggle, WASD/Space/Ctrl axes, camera-relative conversion, diagonal normalization and 5/15-unit speed selection. `movement.c` is the normal build input. `movement.wat` remains a low-level reference fixture.

The C example requires runtime `0.1.0-alpha.4.4.dev.0` or later. Failed renewals
read an owner-local `motion_read` snapshot and log the reason before cleanup.
After focus loss, stale input, Escape or a rejected movement request, release
F6 before pressing it again to resume. The C guest uses `input_read` to distinguish
a usable release from input masked by the host.
The WAT fixture retains the original movement imports and behavior without these
recovery checks or diagnostics.

From the SDK or repository root, build the guest without rebuilding the runtime:

```powershell
python tools/mod.py build examples/movement --output mods/movement
python tools/mod.py check mods/movement
```

The host exposes fixed button input, two floats describing horizontal camera right, and a world-velocity request. It checks ownership, player identity, request bounds and expiry before applying the controller override. Zero velocity holds position; disabling the request releases control. The example switches off when a request fails, and the runtime releases ownership even if guest shutdown cannot run.

The native movement service includes a limited boundary guard while the flight lease is active. Pending recovery and other transition producers can still cancel movement. The example does not move the camera independently. See [installation, controls and limitations](../../docs/movement.md).
