# State watch

A read-only Wasm example that reacts to player movement and selected-camera changes. It requests `player.read`, `camera.read` and `log`; it does not request movement, visibility or physics writes.

Every quarter second, the mod reads copied player and camera state. It logs when player position becomes available, when displacement from its last anchor reaches five world units, and when the selected camera's generation or mode changes. These decisions live in the guest source. A new player generation or an unavailable interval resets the movement anchor.

`state-watch.c` is the normal build input; `state-watch.wat` remains a low-level reference fixture. The mod uses the layouts in `sdk/include/crml_state.h`, checks each read result and retains only copied values. Snapshots come from separate engine callbacks and do not represent a synchronized frame. The camera snapshot describes the selected engine component; cinematic or renderer overrides may produce a different final image.

With Alpha 4.2, copy the compiled package to `crml/mods/state-watch` while the game is closed. Load a save and walk several metres. `crml.log` should contain **player position available**, **selected camera changed**, and later **moved at least five world units**. Backgrounding the game makes player position unavailable. This example has no overlay or controls and never takes ownership of the player or camera.

From the SDK or repository root:

```powershell
python tools/mod.py build examples/state-watch --output mods/state-watch
python tools/mod.py check mods/state-watch
```

The standalone host can load the package, but reads report unavailable because no game services are present. See the [API reference](../../docs/api.md) for field layouts, capability requirements and limits.
