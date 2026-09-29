# Lua counter controller

This minimal trusted engine-Lua example keeps a counter in its private environment. It has no visible gameplay effect and acquires no engine resources.

With a source-enabled development runtime installed, copy `main.luau` into `crml/lua-mods/lua-counter/`. The package's `calls` value in `crml/lua-mods.jsonl` should increase. Add an empty `disabled` file beside the entry point to unload it; remove that marker to load it again.

Save edits to `main.luau` while the game is running to replace the controller. A syntax error leaves the previous working version active. A callback error stops that revision; correcting the source allows a new revision to start after cleanup. Check the package's `state`, `status` and `line` for its current condition. The aggregate `failures` count retains earlier errors even after recovery.

When a save reload retires the controller's engine owner or world, the runtime can recreate the controller from the same source. Its local counter and private environment start fresh; they are not stored in the save.

The returned function receives no arguments for updates and one truthy argument for shutdown. Construction runs before the host retains that function, so resource acquisition belongs inside the callback. Use this structure as a starting point for a source controller. See [Engine Lua integration](../../docs/engine-lua.md) for installation, ownership and trust requirements.
