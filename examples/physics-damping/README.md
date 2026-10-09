# Temporary prop damping

This example selects a nearby prop with **Home** and applies linear damping `8` for five seconds with **End**. Push it yourself by briefly walking into it, then step away; the example does not apply a push automatically. The prop should slide a shorter distance while the effect is active. **F11/Esc** cancels through the native service.

The complete guest behavior lives in this directory:

- `physics-damping.c` is the normal build input.
- `physics-damping.wat` remains a low-level reference fixture.
- `mod.ini` declares the imports' capabilities and the binary filename.

The guest chooses the buttons, detects new presses, supplies the search offset and radius, obtains a token, and chooses the damping value and duration. The runtime handles engine-specific access, the execution phase, identity checks and restoration. It does not interpret Home/End as physics commands itself.

The example calls `physics_select_near(0, 0, 0, 2)`: a sphere of radius two world units centered on the player when searching begins. Edit these arguments in WAT, or `search_offset` and `search_radius` in C, to choose another region. Offsets follow world axes, not the camera; offset length and radius are each bounded to 20 world units. The query returns the nearest eligible prop's token asynchronously. It does not enumerate arbitrary entities.

Change `damping` and `duration_ms` in C, or the matching globals in WAT, to choose the effect: for example, `0.25` for `1200` ms instead of `8` for `5000` ms. Both value and duration are supplied by this mod. The native runtime provides temporary linear damping, not a named gameplay preset; mass, friction and other properties are not exposed yet.

Watch `crml.log` for **search queued**, then **prop selected** before pressing End. **Damping queued** means the request was accepted; **damping active** means it reached the native operation. Missing selection, busy services and rejected application requests are also logged. Neither selection nor queueing alone confirms a property change.

The example retains the accepted operation's token and calls `physics_read` once per tick while it holds that token. **Sampled damping matches request** appears once when a copied snapshot contains the requested linear damping value. This confirms the sampled property, not how far the prop moved or whether this request changed its previous value. Push the prop yourself to compare its behavior.

Snapshots are copied from an engine callback and may be up to 500 ms old. The example checks the version and damping availability flag before using the value. A temporarily unavailable or busy sample does not discard its token; a stale token or completed, idle, retired, conflicted or refused operation does. The example uses at most six gameplay/input calls per tick, including simultaneous Home and End presses, within both the released shared budget of eight and the development build's separate observation/command allowances.

From the SDK or repository root, build the guest without rebuilding the runtime:

```powershell
python tools/mod.py build examples/physics-damping --output mods/physics-damping
python tools/mod.py check mods/physics-damping
```

Requires Alpha 4.2. Rebind `action.0` and `action.1` in `mod.ini` and restart the game. Changing damping or duration requires editing and recompiling the guest source. The accepted bounds are damping 0–8 and duration 1–5,000 ms. Use `physics_status` for guest control flow and log feedback; this example logs selected, active and idle state transitions to `crml.log`. It has no custom on-screen UI. A physics-only session has a native panel; when movement is also active, its panel takes precedence. A mod should inspect status codes when it needs feedback in either configuration.

See [installation and expected behavior](../../docs/physics-trial.md#wasm-damping-example) and the [API contract](../../docs/api.md#experimental-prop-damping).
