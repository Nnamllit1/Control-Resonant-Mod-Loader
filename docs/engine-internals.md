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

## Player mesh and collision ownership

The supported executable's component registrations and lifecycle callbacks establish the layouts below. Sizes are supported by both registration descriptors and array-stride instructions. These are static findings, not live payload validation or an SDK ABI. The [entity resource map](research/entity-resource-map.json) pins the encoded references to the executable fingerprint.

| Component | Hash | Size / alignment | Observed payload |
| --- | --- | --- | --- |
| `MeshResourceID` | `6f477177` | 8 / 8 bytes | Eight-byte IDs gathered by mesh streaming |
| `MeshResource` | `eece657a` | 8 / 8 bytes | Owned resource pointer at +0 |
| `PhysicsResourceID` | `4a8e27fd` | 8 / 8 bytes | Registered ID storage; acquisition path unresolved |
| `CollisionResource` | `e1da5fb1` | 16 / 8 bytes | Owned resource pointer at +0; byte at +8, meaning unresolved |
| `MeshMaterialSet` | `355cf83d` | 1 / 1 byte | Resolved material-set selector, not an owned resource pointer |
| `CharacterControllerBody` | `167fac8a` | 8 / 8 bytes | Owned controller-body object pointer |

Mesh registration at RVA `0x197ba60` installs cleanup `0x196fa60` and move `0x196fb00`. Collision registration at `0x1914650` installs cleanup `0x1904990` and move `0x1904a50`. Both cleanup routines atomically decrement the pointed-to resource's 32-bit reference count at +0x64. When the old count equals one, they call `0x31cf8c0`, passing the resource and its +8 value. Collision cleanup also clears its pointer slot. Move callbacks transfer pointers and clear source slots without acquiring another reference; collision moves additionally copy the +8 byte. A raw pointer copied by an observer therefore does not acquire ownership.

Mesh stream-in registration selects dispatcher `0x1977e20`, which calls implementation `0x196b9e0`. The implementation gathers eight-byte IDs, passes input/output arrays to batch lookup `0x31cabf0` with type descriptor `0x5e73b20`, and stages returned pointer values in command-buffer storage. Helper `0x19730c0` uses the `MeshResource` component hash. Command execution and resource readiness checks remain to trace. Stream-out dispatcher `0x1977b60` reaches `0x1974b90`; its registration query associates it with removal of `MeshResource` from entities outside the streamed-in set.

Accessor offsets require the correct base address. The mesh path passes `world+8` to a query iterator builder, while accessor fragments use component metadata offsets +8/+0x1c and chunk-table offset +0x48. Adding eight reconciles these with the movement inspector's +0x10/+0x24 metadata and +0x50 chunk table. This is evidence for a shifted query view, not an additional component table. Trace the iterator builder's output before reusing those accessor fragments against a movement snapshot.

The physics trace below follows `physics_module::streamIn` through resource acquisition and object staging; collision-template selection and backend actor creation remain incomplete. Its executable signature includes transform, layer, scale, frozen/keyframed state, collision resources and physics-scene inputs; the signature alone does not establish field offsets or parameter semantics. Neither mesh nor collision pointers should be retained across frames or exposed to Wasm based on this static pass.

Verify the reviewed references without running the game:

```powershell
python tools/verify_engine_map.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant\CONTROLResonant.exe" docs/research/entity-resource-map.json
```

### Material selection and visibility commands

`MeshMaterialSet` registration `0x18f5800` specifies one-byte storage. `updateMaterialsToRenderer` dispatcher `0x19783c0` calls `0x196b0c0`. On dirty bit 0, that implementation looks up a 32-bit material-set name through `0x303d5c0`, stores the result's low byte into the selector, and emits renderer opcode `0x70` with a render handle and the selector. Lookup treats name 0 and `0x933b5bde` as selector 0; otherwise it searches 24-byte entries at object+0x180, count+0x188, and returns a one-based index. Missing names are reported invalid and the caller falls back to selector 0. This establishes selection behavior, not material replacement or shader parameter editing.

`applyHide` registration `0x197d0e0` selects dispatcher `0x1977540`, which calls `0x1969ca0`. Its inputs are a four-byte `HideReason` and one-byte `InvisibilityReason`; either nonzero means hidden. A state change updates one-byte `MeshHidden`, changes bit 16 of the four-byte `MeshFlags`, and collects the 32-bit renderer handle from `RenderObject+0x10` (component stride 24). Renderer opcodes `0x69` and `0x6a` respectively hide and show those handles. Small command headers pack opcode into bits 0?8 and element count above bit 8.

Command storage comes from `*(renderer_global+0x18)`, with global RVA `0x5e69000`. Allocation helper `0x2fb8340` receives storage, byte length and a wait flag. Producers write the payload, publish its length at allocation-8, increment storage+0x30 by two atomically, and notify if the previous low bit was set. The notification thunk `0x393109d` imports `__std_atomic_notify_one_direct`. This producer-side protocol is traced; downstream GPU execution is not.

The [visibility experiment](visibility.md) uses this observed path, on the owning mesh phase, with fresh entity and query-membership validation. It deliberately leaves native hide reasons intact and lets the original system restore its cached state after the lease ends. Live behavior remains unverified.

### Physics acquisition and controller ownership

Physics stream-in registration selects `0x191ac10`, whose dispatcher calls `0x18fdf10`. One resource-ID branch calls typed resource lookup `0x31cab80` using descriptor `0x5d215f0`, transfers the returned reference out of its temporary wrapper, and calls metadata accessor `0x2d44280`. That accessor validates a type key before returning metadata. The path branches on metadata+0x3b; the semantic name of this flag is not established.

In one branch, factory `0x1914520` allocates a 0x1b8-byte object and calls constructor `0x2d0b3a0`. The stream-in implementation copies resource data reached through resource+0xa0, then calls `0x2d0b810` to populate the new object and additional helpers to process its contents. These instructions establish resource-to-runtime-object staging. Actor/shape creation, physics-scene insertion, mass and collision-filter semantics still need backend tracing; this object must not yet be described as a verified PhysX actor.

`CharacterControllerBody` registration `0x1ba1aa0` installs an eight-byte stride. Cleanup `0x1b9ac90` delegates each slot to `0x1ba3be0`: it releases an allocation at owned-object+0x38, destroys objects at +0x10 and +0 through `0x2d17060`, frees those objects, then frees the container. Its move callback `0x1b9ad10` transfers the pointer and zeros the source. This is distinct from the shared mesh/collision resource reference-count protocol. The two nested physics objects' exact roles remain unresolved.

### Collision package loader

RTTI identifies `physics::CollisionPackageResource`, instance vtable `0x4d6db50`, with reflected size 0x270 and alignment 8. Its request-load slot reaches `0x2d441b0`, which uses FileBuffer loader `0x31cf050`. The callback at `0x2d45570` forwards to reader `0x2d44a60`.

The reader validates metadata through its type key. When metadata+0x3b is nonzero, it retains a copy of incoming bytes in a compact buffer: pointer at resource+0xa0, length at +0xa8, capacity at +0xac. It then calls decoder `0x2d442c0` and publishes completion through `0x31cee00`, the same completion helper previously traced for Lua and material resources. Thus the +0x3b branch controls byte retention in this reader, although its original field name and all consumers are not established.

The decoder creates another copy aligned to 0x80 bytes, owned at resource+0xb0 and initially referenced by +0xb8. It processes a structured stream and populates additional resource data. The complete binary schema and native actor bindings remain unresolved; these fields are not exposed as a modification API.
