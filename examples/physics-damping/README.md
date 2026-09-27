# Temporary prop damping

This example selects a nearby prop with **F7** and applies linear damping `8` for five seconds with **F8**. Push it yourself by briefly walking into it, then step away; the example does not apply a push automatically. The prop should slide a shorter distance while the effect is active. **F11/Esc** cancels through the native service.

The complete guest behavior lives in this directory:

- `physics-damping.wat` is the source compiled by the repository build.
- `physics-damping.c` expresses the same behavior with the C SDK. It is an alternative source, not a file automatically compiled by the repository build.
- `mod.ini` declares the imports' capabilities and the binary filename.

The guest chooses the buttons, detects new presses, requests selection, obtains a token, and chooses the damping value and duration. The runtime handles engine-specific access, the execution phase, identity checks and restoration. It does not interpret F7/F8 as physics commands itself.

From the repository root, rebuild just this guest without rebuilding the runtime:

```powershell
.\dist\crml\crml_wat.exe examples/physics-damping/physics-damping.wat dist/examples/physics-damping/physics-damping.wasm
```

To compile the C alternative, use Clang with the wasm32 backend and `wasm-ld`:

```powershell
clang --target=wasm32 -O2 -nostdlib -Isdk/include examples/physics-damping/physics-damping.c -Wl,--no-entry -Wl,--export-memory -Wl,--initial-memory=131072 -Wl,--max-memory=16777216 -o dist/examples/physics-damping/physics-damping.wasm
```

Change the input masks (`1` for F7, `2` for F8), damping value or duration in the guest source, then recompile and replace only its installed `.wasm` file with the game closed. The accepted bounds are damping 0–8 and duration 1–5,000 ms. Use `physics_status` for additional mod UI or control flow; this minimal example uses the native panel and discards command return codes.

See [installation and expected behavior](../../docs/physics-trial.md#wasm-damping-example) and the [API contract](../../docs/api.md#experimental-prop-damping).
