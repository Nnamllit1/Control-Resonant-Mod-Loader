# Internal engine mappings

The integration goal is to make engine operations available to mods: finding a live object, changing its material, creating an entity, or subscribing to a gameplay event. That requires tracing the engine's own object relationships, lifetime rules, and execution stages. The [asset catalog](engine-research.md) supplies names and storage locations; this page records what happens beyond the files.

Rendering is one part of this work, alongside world/entity management, scripting, physics, UI, and streaming. A native bridge should ultimately issue engine operations through the appropriate subsystem. It must distinguish a resource being present on disk, loaded into memory, instantiated in a world, and ready for rendering.

## Working model and evidence boundaries

```text
Resource ID + runtime type
        |
        v
Resource manager ----> typed resource object + owned references
        |                          |
        v                          v
Loader / file adapter       type-specific load callback
        |                          |
        v                          v
Pack2 blocks                decoded data + resource state

Script path:    bundles -> resource IDs -> preload queue -> VM / script instance
Material path: metadata -> dependency IDs -> owned resource references
World/render:  ECS material ID -> resource / render handle -> RenderQueue -> GPU
```

This is a **working model**, not a complete verified execution graph. The Lua preload collection, resource callback paths, and material dependency resolver below were inspected in disassembly. The VM transition, material ECS handoff, render-queue consumer, and final GPU work still have gaps. Scheduling order cannot be inferred from the order of names in the executable.

## Shared resource infrastructure

Lua scripts and materials have different payloads but share infrastructure:

| Operation | Lua script | Material |
| --- | --- | --- |
| Runtime class | `content::LuaScriptResource` | `rend::MaterialResource` |
| Instance vtable RVA | `0x3bebee8` | `0x4f6ca38` |
| Reflected object size | `0xc0` | `0xc8` |
| Async loading method RVA | `0x711db0` | `0x30427c0` |
| Blocking loading method RVA | `0x711d30` | `0x3042730` |
| Shared async helper RVA | `0x31cef20` | `0x31cef20` |
| Shared blocking helper RVA | `0x31cf2a0` | `0x31cf2a0` |
| Type-specific reader RVA | `0x712150` | `0x3043bc0` |
| Shared state-publication helper RVA | `0x31cee00` | `0x31cee00` |

Both callback identities are supported by RTTI names that contain `requestLoad` or `blockingLoad`. Each pair converges on its class-specific reader. Both readers finish through a helper that atomically exchanges the resource state at `+0x68` with the value 4. Calling this a loaded-state transition is an interpretation of its position in the path; the complete state enum is not established.

Resource reference cleanup atomically decrements `+0x64`, and calls another engine routine when the previous count was one. This means keeping a resource usable involves ownership and release behavior, not merely remembering an address. The reflection/type-info vtable is also distinct from the resource-instance vtable.

All RVAs here belong to executable SHA-256 `2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0`.

## Lua: bundle identity to preload ownership

The `collectScriptsToLoadIntoVM` string initially leads to system **registration**, at `0x19c0330`. That function installs dispatcher `0x19bbee0`, which unpacks three arguments and transfers to the actual implementation at `0x19b3b80`.

The implementation performs these observable steps:

1. Reads bundle records through `SharedBundleState`: the array pointer is at `+0xd20`, count at `+0xd28`, and record stride is `0x90`.
2. Selects records whose associated object has state value 1 at `+4`, then compares their IDs with previously observed bundle IDs in `LuaPreloadQueue`. The meaning of that state value is not yet independently established.
3. Visits resource references associated with those bundles and compares their runtime type index against the Lua resource type index getter.
4. Collects the resource identifiers from resource objects at `+8`, sorts/deduplicates them, and checks the `LuaCachedScripts` hash table.
5. For a missing ID, calls typed request helper `0x19b7ce0`. That helper supplies a type descriptor at `0x5bec370` and forwards to the common resource manager at `0x31cab80`.
6. Moves a successful owned resource reference into the preload queue. The queue stores references at `+0`, count at `+8`, and capacity at `+0xc`; its previously observed bundle-ID array begins at `+0x10`.

This separates four identities/lifetimes: bundle ID, resource ID, owned resource object, and eventual script instance. An entity's persistent GlobalID and generation-checked runtime entity handle are additional, distinct identities.

The Lua resource reader at `0x712150` uses the file object's virtual operations to obtain its length, reads one byte into resource `+0xb8`, allocates/resizes a compact buffer at `+0xa0`, and reads the remaining bytes into that buffer. The first byte's meaning and the consumer of the buffered representation still need tracing. Loading these bytes is not proof that the VM has instantiated or initialized the script.

The registered `processThrottledLoading` dispatcher at `0x19bd7b0` is the next concrete target for following preload ownership into the VM. ECS signatures also name `LuaScriptPendingResource`, `FreshlyCreatedLuaScript`, `LuaInitEvents`, and `LuaPendingCallbackInstallations`; these identify additional lifecycle stages without yet proving their complete ordering.

## Rendering: material data and dependency ownership

The material reader replaces an owned data object stored at resource `+0xa8`, explicitly destroys the previous object, and invokes decoder `0x304c9b0`. The decoder receives a file wrapper, the material data object, and a metadata body selected after checking the metadata type. This is a concrete connection between packaged metadata and runtime representation.

A separate method dispatches to dependency resolver `0x3042b90`. It traverses three metadata arrays of resource IDs and requests each dependency through the same resource manager used by script preloading:

| Metadata body | Runtime material data destination | Requested type descriptor |
| --- | --- | --- |
| Array `+0x30`, count `+0x38` | Array at `+0x10`, stride `0x20`, reference at record `+8` | `0x5e60cc0` |
| Array `+0x20`, count `+0x28` | Reference array at `+0x20`, stride 8 | `0x5e76dd8` |
| Array `+0x10`, count `+0x18` | Reference array at `+0x30`, stride 8 | `0x5d2b790` |

The IDs become owned runtime references, with old references released during replacement. The exact names of all three dependency types are still unresolved; they should not be labeled texture/shader dependencies solely from appearance.

`rend::TextureResource` has another distinct object layout: reflected size `0x1b8`, instance vtable `0x4ddc780`, and async loading entry at `0x2e8ed20`. One branch builds metadata-driven setup arguments; another creates a `LambdaStreamJob` identified by RTTI as belonging to `TextureResource::requestLoad`, atomically appends it to a queue, and notifies a worker. The branch selector and metadata field semantics still require investigation. Texture loading is therefore not interchangeable with invoking the generic material reader.

ECS metadata separately names `MaterialResourceID`, `MaterialResource`, `MaterialRenderHandle`, `MaterialOverrideTargets`, and `coregame::global::RenderQueue`, with material stream-in, stream-out, and destruction systems. These are leads for connecting world objects to rendering. Material release code also allocates command storage through a global context at `0x5e69000`; identifying its consumer is a specific next step.

The GPU-facing work remains open: command decoding, shader selection, pipeline-state objects, descriptor binding, visibility, pass scheduling, and resource retirement. None of the recovered load methods is established as a safe draw or material-edit API yet.

## What this means for the mod bridge

The intended boundary is an operation with an engine-owned lifetime, rather than exposing raw memory to Wasm. For example, a future material override could:

1. Resolve a validated entity handle to its material target.
2. Resolve and retain the requested material resource and dependencies.
3. Apply the override through the engine's update/render command path.
4. Release it on removal, entity destruction, or stream-out.

This is a proposed integration contract, not an implemented SDK call. The same approach applies to spawning, scripting events, and physics operations: find the engine's existing operation, establish ownership and execution context, then expose a bounded guest API. This can give sandboxed mods useful native engine capabilities while keeping native code in the trusted bridge.

## Reproduce the static checks

The reviewed maps preserve object-layout evidence, function roles, exact reference sites, and unresolved questions:

- [Lua resource map](research/lua-resource-map.json)
- [Material resource map](research/material-resource-map.json)

```powershell
python tools/verify_engine_map.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant\CONTROLResonant.exe" docs/research/lua-resource-map.json
python tools/verify_engine_map.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant\CONTROLResonant.exe" docs/research/material-resource-map.json
```

Each map checks 24 encoded references after requiring an exact executable fingerprint. Checks cover vtable pointers, relative calls/jumps, address loads, and constants. They detect a mismatched map; they do not prove semantic labels, thread ownership, complete function signatures, or live behavior. No game functions are executed.
