# Hold to hide the player mesh

`visibility.wat` contains the guest that the repository build compiles. `visibility.c` is an equivalent C SDK source. Both choose **F7**, read the bounded input mask, and request hiding only while it is held. Release restores normal engine visibility on the next eligible update.

The button decision is mod code. `visibility_set` is the engine operation: the runtime checks ownership and expiry, resolves the current player mesh, and submits the renderer command. The guest does not use the legacy `visibility_poll` helper, whose F7 behavior is fixed inside the runtime.

To use **F8**, change `CRML_BUTTON_F7` to `CRML_BUTTON_F8` in C, or the mask `1` to `2` in WAT. No runtime edit is required. From the repository root, compile the source you edited:

```powershell
.\dist\crml\crml_wat.exe examples/visibility/visibility.wat dist/examples/visibility/visibility.wasm
```

Or, with a Clang distribution supporting wasm32 and `wasm-ld`:

```powershell
clang --target=wasm32 -O2 -nostdlib -Isdk/include examples/visibility/visibility.c -Wl,--no-entry -Wl,--export-memory -Wl,--initial-memory=131072 -Wl,--max-memory=16777216 -o dist/examples/visibility/visibility.wasm
```

The normal repository build compiles WAT, so editing C alone will not change its output. See the [visibility guide](../../docs/visibility.md) for installation. Renderer scheduling and guarded native access remain host responsibilities; mods compose the exposed operations to define behavior.
