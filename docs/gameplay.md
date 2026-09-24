# Gameplay and noclip

**Status: planned; no working noclip or in-game UI is included.**

The first gameplay target is player noclip: move the controlled character through geometry, steer relative to the view, adjust movement speed, and restore ordinary movement when disabled. A free camera alone does not satisfy this feature.

## Required game integration

1. Establish a repeatable startup route and a supported executable fingerprint.
2. Identify the active player, world transform, collision/controller state, and a game-thread callback.
3. Verify transitions across menus, loading, death, cutscenes, and player replacement.
4. Add an owner-aware movement override that restores captured state when disabled or when the mod traps. Restoration belongs to the native bridge, not solely to a guest shutdown callback.
5. Expose capability-checked movement commands and a bounded UI toggle API to Wasm.
6. Validate noclip and restoration in a playable save before marking the feature supported.

## UI direction

The intended first UI is a small noclip toggle with a speed control and a clear unavailable state. Later APIs can support panels and selected game-UI manipulation. Arbitrary native callbacks, unrestricted JavaScript/Lua evaluation, and raw UI pointers will not be mod APIs.

No engine offsets or function signatures are inferred from the original Control. Free-camera strings and component names in a binary are investigation leads, not sufficient evidence for calling a function or changing player state.
