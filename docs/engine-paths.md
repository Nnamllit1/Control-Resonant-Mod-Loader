---
title: Northlight engine operation paths
description: Reviewed CONTROL Resonant code paths for scripting, rendering, physics filters, events, audio, and entity operations, with evidence limits.
---

# Reviewed engine operation paths

These paths connect selected entries in the [engine atlas](engine-atlas.md) to their implementations. They extend the [resource/lifetime mappings](engine-internals.md) and [physics dynamics research](physics-dynamics.md). All findings here come from static inspection; no new runtime operation was installed or tested in a gameplay session.

The [executable reference map](research/engine-paths-map.json) checks **87 encoded references**. The [shape backend map](research/physx-shape-map.json) checks another **4 references**. Those checks reproduce addresses and instruction encodings, not semantic conclusions or callable ABIs. Field interpretations below require the accompanying instruction/data-flow review.

Executable fingerprint: `2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0`. Unless explicitly marked **PhysX DLL**, addresses are executable RVAs. Game updates require remapping.

## Script resources and the VM

The previously traced `LuaScriptResource` reader at `0x712150` separates one envelope byte into resource `+0xb8` and places the rest in a compact buffer at `+0xa0`. The consumer is now connected:

```text
processThrottledLoading registration
  -> dispatcher 0x19bd7b0
  -> processing 0x19c4d60
       -> per-entity loading 0x19c4310
       -> resource/cache lookup 0x19c2050
            -> cache miss 0x19c3730
                 -> wrapper 0x3229280
                      -> bytecode loader 0x2c27b70
```

The preload processing path pops resource references from the end of its array, calls the resource/cache helper, then releases the owned resource reference. The cache helper checks a recent ID/value pair, performs a hash lookup, and creates a VM reference and cache entry on a miss. A resource being loaded, being cached in the VM, and being initialized for an entity are distinct states.

At `0x19c3730`, the code obtains a chunk label through `0x711e90`, selects the compact buffer's inline or heap storage, reads its length at `+0xa4`, and passes the bytes through `0x3229280` to `0x2c27b70`. Thus the VM loader consumes the bytes **after** the resource envelope byte.

The loader performs these observable checks:

| Input | Instruction evidence | Interpretation |
| --- | --- | --- |
| First byte | Read at `0x2c27b9b`; zero branch at `0x2c27ba9` | Zero selects an error-message payload |
| Bytecode version | Subtract 3 at `0x2c27bfa`, unsigned compare against 3 | Accepted range is 3 through 6 |
| Optional type version | Read for bytecode version at least 4; subtract 1 and compare against 2 | Accepted type-version range is 1 through 3 |
| String count | Seven-bit groups accumulated beginning at `0x2c27d20` | Variable-length integer followed by string-table processing |

This ordering and the version-error diagnostics correspond closely to [Luau's upstream bytecode loader](https://github.com/luau-lang/luau/blob/master/VM/src/lvmload.cpp). **Luau-derived or compatible loader** is the supported inference; an exact upstream revision, unchanged opcode set, and compatibility with a current stock compiler are not established. The supported numeric ranges above come from this executable, not today's upstream constants.

The sampled `systems.binlua` header `00 06 03 2b` is consistent with envelope `00`, bytecode version `06`, type version `03`, and string count `0x2b`. The envelope byte's meaning remains unknown. The old Lua 5.1.4 identifier alone does not identify the serialized format. No game script body is reproduced here, and no replacement bytecode has been executed.

Per-entity helper `0x19c4310` constructs tables using names `__index`, `_ENV`, and `self`, calls the resource/cache helper, and manages VM references. Complete initialization ordering, failure cleanup, and stream-out teardown remain open. Wasm guests should eventually reach reviewed engine operations through the native bridge; the engine VM is not an alternative guest sandbox.

## Script callbacks and event timing

| Operation | Reviewed path | Observable behavior |
| --- | --- | --- |
| Fixed script update | `0x19bbda0` → `0x19b4ae0` → `0x1a0a6e0` | Traverses update-related state and invokes VM call helper `0x2c4fc90`; contains `update_system` lookup and error handling |
| `nl_update_callback` | Binding callback `0x1a16680` | Requires active script context, accepts function or nil, creates a reference, installs/replaces or removes callback state |
| `nl_add_event_handler` | Binding callback `0x1a15670` | Checks active script context, retains a callback reference, chooses registration helper `0x1a1eec0` or `0x1a1ec90` |
| `nl_send_custom_event` | Binding callback `0x1a15d30` | Builds the `lua.` event name, looks up handlers through `0x1a00060`, and invokes matching callbacks through `0x2c4fc90` within the same call |

The custom-event error branch releases a VM reference, queues handler removal, and calls error helper `0x1a0a010`. Delivery can therefore be immediate even when cleanup is deferred. A future bridge must account for reentrancy and callback failure; it cannot assume that every event is delivered next frame.

These callbacks receive an engine VM context, not an ordinary application C function argument list. Complete argument schemas, VM stack rules, payload variants, and callback removal during entity destruction still need establishing. The numeric access modes in the recovered ECS declarations do not answer those questions.

## World spawning and removal

`coregame::spawning::handle_spawn_requests` declares access to `SpawningPoolRegistry`, `ecs::CommandBuffer`, `ecs::EntityGeneratorApi`, and the GlobalID-to-entity map. Its registration installs dispatcher `0x1893610`, which transfers to `0x188f780`. Separate systems name pending instantiation, stream-in, despawns, and bundle-child destruction.

Three script-facing paths provide concrete entry points:

| Binding | Callback RVA | Downstream path |
| --- | --- | --- |
| `spawn_bundle_with_instigator` | `0x2390e10` | Builds request arguments, calls `0x188e260`, has a spawn-failure diagnostic |
| `npc_spawn` | `0x2381d40` | Resolves script/ECS context, parses arguments, calls `0x22e7030` |
| `npc_despawn` | `0x23822a0` | Resolves entities, calls `0x188e890` → `0x443a30` |

`0x443a30` appends an entity index to command pages. It tracks the incoming handle for command bookkeeping and writes a four-byte entry into a page, allocating/linking another page when necessary. This is a **queued structural operation**, not immediate deletion of an entity pointer. Where the full generation is validated and exactly which downstream cleanup systems run remain separate questions.

`handle_despawns` consumes a removed `SpawnedEntity` query; `destroyBundleChildren` consumes removed `SpawnedBundleRoot` records and accesses world/GlobalID state. These declarations support separate child and spawning ownership stages, but do not prove that every child/resource has been destroyed when the initial request returns.

The `flushEndOfFixedUpdate`, `flushStreamInOutputs`, and `flushStreamOutOutputs` registrations share dispatcher `0x1af20b0`, which calls `0x1d4d160`. That implementation includes synchronization/task waiting and command-processing helpers. The complete command schema and scheduler dependency graph are not recovered. Finding this boundary does not make it safe to invoke a flush from an arbitrary overlay or worker thread.

## Shape filters and collision ownership

**PhysX DLL fingerprint:** `ec53e67b700e67a5420a2c951340efd9a8ed9d086da539eed6ee88368fe0ce51`.

Backend diagnostics identify two distinct setter bodies:

| Operation | PhysX DLL RVA | Observed writes |
| --- | --- | --- |
| Simulation filter data | `0x3bd20` | Four words at shape `+0x30`, `+0x34`, `+0x38`, `+0x3c`; then calls change helper `0x3b1a0` with flag 4 |
| Query filter data | `0x3bc30` | Four words at shape `+0xc0`, `+0xc4`, `+0xc8`, `+0xcc`; no call to that change helper in this body |

Both inspect simulation-running state before writing. The simulation-filter helper propagates through the associated owner and calls `0xff730`; simply copying those four words would omit that behavior. Candidate vtable base `0x163b50` has these methods at slots `+0x68` and `+0x78`; constructor-based ownership confirmation remains open.

The executable's shape construction helper `0x2cff810` obtains a material, creates a backend shape, constructs filter data through `0x2cdd5b0`, invokes query slot `+0x78` and simulation slot `+0x68`, attaches the shape, then releases its local shape reference. Both channels receive the same initial data in this particular path; they remain separate properties thereafter.

The game filter packer has a small, concrete layout:

```text
word[0] = bitwise_not(second_argument_u32)
word[1] = third_argument_u32
word[2] = fourth_argument_u32
word[3] = zero_extend(fifth_argument_u16)
```

This call site supplies second argument `0x15`, zero for the next two words, and `0x190` for the final 16-bit value. The meanings of those constants and individual bits are **not established**. In particular, do not call `0x15` a specific collision layer until its producers and filter consumer agree. Query filtering, simulation filtering, gameplay body groups, trigger delivery, and out-of-bounds detection must be mapped separately.

## Materials and renderer handoff

`coregame::material::streamIn` installs dispatcher `0x1995f80`, which calls `0x1987860`. That implementation combines resource work with command construction through global renderer context `0x5e69000` and allocator `0x2fb8340`.

Two normal packet forms are visible:

| Header | Payload size | Evidence |
| --- | --- | --- |
| `(count << 9) \| 0x5d` | `16 * count` bytes after the four-byte header | Allocation at `0x1987a82`, header construction at `0x1987a8e` |
| `(count << 9) \| 0x7a` | `12 * count` bytes after the four-byte header | Allocation at `0x1987b61`, header construction at `0x1987b6a` |

Large payloads take a separate allocation/wrapper path. Publication later notifies through `0x393109d`. These are command encodings associated with material stream-in; exact command names and the consumer's validation/resource-retirement rules have not been recovered. They are not a supported packet-writing interface.

Material override dispatcher `0x1995730` calls `0x198e550`, which routes named values including `EmissionMultiplier`, `EmissionIntensity`, and `ColorMultiplier` through parameter helpers. This establishes a higher-level parameter-update path worth investigating before implementing direct render-packet writes. It does not yet specify all accepted value types or which materials support those names.

`variableUpdateRendererSync` dispatches through `0x18d22c0` to `0x18d3e60`. The latter compares a renderer-context counter at `+0x468` with a current-counter-derived target and waits through `0x3933315`. This is evidence of producer/consumer synchronization; it is not sufficient to label the counter a GPU fence.

Open work includes command decoding, shader/pass selection, descriptor and pipeline-state ownership, culling, and final GPU resource retirement. The material-resource loader, ECS handoff, and command producer are connected; the complete draw pipeline is not.

## UI models and events

`coregame::ui_events::system::handleLuaCallbacks` declares `UIPendingEvents` and `LuaEvents`. Registration installs `0x17e06a0`, which transfers to `0x17df510`. That body traverses pending state, prepares serialized values, obtains event storage through `0x1a016f0`, and publishes records. Separate fixed/variable cleanup and page registration/stream-out systems appear in the atlas.

`nl_coherent_texture_view_add_data_bindings`, callback `0x19da040`, reads `bindings` and `listBindings` tables and fields including `keyName`, `modelName`, `transformID`, and `type`. It builds temporary arrays and calls `0x1b4ff30`, whose downstream helpers copy/retain array contents before the temporary data is cleaned up. This is an engine UI data-model bridge, beyond drawing an independent overlay.

The imported Cohtml dependency and its V8 dependencies concern UI middleware; they do not identify the gameplay VM. Model/page ownership, binding replacement, interactive texture-view updates, and complete value encoding remain open. None of these functions is exposed as a Wasm import by this research pass.

## Audio controls

`nl_audio_execute_control` at `0x3345940` validates the entity/components and control string, hashes the control name, and appends the resulting 32-bit value to a component buffer. The hash starts at `0x811c9dc5`, ORs each byte with `0x20`, XORs it into the accumulator, then multiplies by `0x1000193`. OR-ing with `0x20` is the observed operation; it should not be replaced with a general Unicode or ASCII-lowercase routine.

`sndencore::wwise::flush_execution_control_queue::system` installs dispatcher `0x3307570`. It calls `0x32f53b0` for each matching entry. The entire helper through its return at `0x32f5418` swaps two 16-byte pointer/count/capacity records when distinct, then clears the source count. It is a **buffer handoff**, not the final backend play call. Neighboring functions at `0x32f5420` and `0x32f54e0` are separate routines and must not be attributed to this helper merely because they occur in the same disassembly range.

The atlas separately names Wwise execution, playing instances, RTPC/state/switch operations, listeners, music callbacks and acoustics. Completing the chain requires tracing the consumer after the handoff, its backend object identity, and callback/cancellation lifetimes.

## Animation events

`coregame::animationevent::updateLuaEvents` installs `0x1c1e190`, which calls `0x1c175b0`. The reviewed portion iterates animation event data, tests values/transitions, reserves storage atomically, copies event-name strings and value tags, and appends event records through `0x4a7870`. Storage allocation uses `0x1a016f0`, also seen in the UI event path.

This demonstrates queued animation-to-script event production. It does not establish a public serialization schema or the final consumption phase. Clip/source/mixer/graph/skeleton declarations in the index describe the surrounding surface; pose ownership, root-motion authority, and physics-animation synchronization need deeper tracing before a general animation API.

## Camera

`nl_camera_horizontal_fov` at `0x19cb5f0` resolves a camera through `0x19e0d80`, reads two floats at `+0x30` and `+0x34`, converts them through `0x1ba97c0`, and pushes one result. This callback is a **getter**. Its name alone is not evidence for a writable horizontal-FOV property or its units.

An internal `coregame::freecamera::update` implementation is present. Registration installs `0x1addab0`, whose reviewed call goes to `0x1ad9e30`. The latter references a free-camera debug panel, lens/FOV controls, and input/math helpers, and reaches transform helper `0x1811fe0`. Presence of that path does not prove a supported retail activation mechanism.

Gameplay camera mixing, tail-camera behavior, lock-on, and clipping have separate declared families. A future free-camera operation must establish which system owns the final pose and how to restore that ownership; changing the player's movement is a different operation.

## AI and navigation

The registered Bonsai behavior-tree update specialized for `heron::GameBehaviorTreeContext` dispatches through `0x2522b00` to `0x2521230`. That implementation collects work into a temporary buffer, uses task helpers `0x3271560` and `0x3271140`, submits through `0x3271690`, and calls additional completion/lifetime helpers before releasing its temporary storage. The reviewed path contains a parallel-for source anchor and an update-behavior-tree work label.

This supports parallel work in that update path; it does not establish that the main thread owns all AI state or that those helpers constitute a public job API. Navigation mesh updates, link/obstacle changes, workspot searches, knowledge, and EQS are separate families in the index. Full worker lambda arguments, cancellation, and interactions with world unload remain open.

## Save and load

`coregame::savegames::processWriteRequests` dispatches through `0x18c3770` to `0x18c0f90`. The latter traverses records at stride `0x5d8`, branches on request state, builds save-chunk storage and invokes serialization-related helpers. `processLoadRequests`, container updates, typed header caching, and gameplay save bindings are separately registered.

This is a concrete request-processing entry, not a recovered save-file schema or proof of atomic/durable disk writes. Save cancellation, completion callbacks, version migration, checksums, and reconstruction of live entity/resource state remain unresolved. No save requests or writes were invoked during this pass.

## Reproduce the reference checks

```powershell
python tools/verify_engine_map.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant\CONTROLResonant.exe" docs/research/engine-paths-map.json
python tools/verify_engine_map.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant\PhysX_64.dll" docs/research/physx-shape-map.json
```

The [verification matrix](engine-atlas.md#verification-and-subsequent-integration) describes the subsequent observations needed before these paths become runtime operations. Static reference checks can fail safely on a different build, but passing them cannot substitute for phase, lifetime, error-path, and unload verification.
