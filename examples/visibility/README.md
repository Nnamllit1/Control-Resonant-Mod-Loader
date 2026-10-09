# Hold to hide the player mesh

`visibility.c` is the normal build input; `visibility.wat` remains a low-level reference fixture. The mod chooses **F7**, reads the bounded input mask, and requests hiding only while it is held. Release restores normal engine visibility on the next eligible update.

The button decision is mod code. `visibility_set` is the engine operation: the runtime checks ownership and expiry, resolves the current player mesh, and submits the renderer command. The guest does not use the legacy `visibility_poll` helper, whose F7 behavior is fixed inside the runtime.

To use **F8**, change `CRML_BUTTON_F7` to `CRML_BUTTON_F8` in C. No runtime edit is required.

From the SDK or repository root, build the guest without rebuilding the runtime:

```powershell
python tools/mod.py build examples/visibility --output mods/visibility
python tools/mod.py check mods/visibility
```

See the [visibility guide](../../docs/visibility.md) for installation. Renderer scheduling and guarded native access remain host responsibilities; mods compose the exposed operations to define behavior.
