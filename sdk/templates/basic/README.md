# C mod template

Edit `main.c` for mod behavior and `mod.ini` for its ID and capabilities. This
is freestanding C targeting wasm32; it has no C runtime or WASI access.

From the SDK root, build into a separate package directory:

```powershell
python tools/mod.py build path/to/source --output mods/my-mod
python tools/mod.py check mods/my-mod
python tools/mod.py test mods/my-mod --ticks 20
```

`check` executes module start, initialization and shutdown in the standalone
sandbox. `test` also runs worker ticks. Engine services are unavailable in this
host; these commands do not verify gameplay. Review the matching API reference
for capability checks and operation results before adding engine behavior.
