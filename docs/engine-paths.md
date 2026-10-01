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

The sampled `systems.binlua` header `00 06 03 2b` is consistent with envelope `00`, bytecode version `06`, type version `03`, and string count `0x2b`. The envelope byte's meaning remains unknown. The old Lua 5.1.4 identifier alone does not identify the serialized format. No game script body is reproduced here. Separate [engine Lua integration tests](engine-lua.md) cover execution of self-authored bytecode without replacing game assets.

Per-entity helper `0x19c4310` constructs tables using names `__index`, `_ENV`, and `self`, calls the resource/cache helper, and manages VM references. The [environment and callback contracts](engine-lua.md#script-environments-and-callback-ownership) describe this setup and active-owner resolution. Failure cleanup and stream-out teardown remain separate questions. The engine VM is separate from the Wasm host API and is not a guest sandbox.

## Script callbacks and event timing

| Operation | Reviewed path | Observable behavior |
| --- | --- | --- |
| Fixed script update | `0x19bbda0` → `0x19b4ae0` → `0x1a0a6e0` | Traverses update-related state and invokes VM call helper `0x2c4fc90`; contains `update_system` lookup and error handling |
| `nl_update_callback` | Binding callback `0x1a16680` | Requires active script context, accepts function or nil, creates a reference, installs/replaces or removes callback state |
| `nl_add_event_handler` | Binding callback `0x1a15670` | Checks active script context, retains a callback reference, chooses registration helper `0x1a1eec0` or `0x1a1ec90` |
| `nl_send_custom_event` | Binding callback `0x1a15d30` | Builds the `lua.` event name, looks up handlers through `0x1a00060`, and invokes matching callbacks through `0x2c4fc90` within the same call |

The custom-event error branch releases a VM reference, queues handler removal, and calls error helper `0x1a0a010`. Delivery can therefore be immediate even when cleanup is deferred. Callers must account for reentrancy and callback failure; delivery is not necessarily deferred to the next frame.

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

Material override dispatcher `0x1995730` calls `0x198e550`, which routes named values including `EmissionMultiplier`, `EmissionIntensity`, and `ColorMultiplier` through parameter helpers. This is a higher-level parameter-update path than direct render-packet submission. It does not yet specify all accepted value types or which materials support those names.

`variableUpdateRendererSync` dispatches through `0x18d22c0` to `0x18d3e60`. The latter compares a renderer-context counter at `+0x468` with a current-counter-derived target and waits through `0x3933315`. This is evidence of producer/consumer synchronization; it is not sufficient to label the counter a GPU fence.

Open work includes command decoding, shader/pass selection, descriptor and pipeline-state ownership, culling, and final GPU resource retirement. The material-resource loader, ECS handoff, and command producer are connected; the complete draw pipeline is not.

## UI models and events

The engine UI uses Cohtml/Gameface views, authored page resources and native model/event bindings. Three maps separate these responsibilities:

- [UI resources](research/ui-resource-map.json): packed HTML/JavaScript, typed resource loading, URL routing and response ownership.
- [Page lifecycle](research/ui-page-lifecycle-map.json): authored `UIPageComponent`, reflected page construction, renderer registration, readiness and removal.
- [Model lifetime](research/ui-model-lifetime-map.json): copied binding descriptions, queued requests, model construction and context teardown.

### Resources and native rendering

`data/uiresources/game/ui/ui.ui` is a serialized `ui::PageResource`, not a standalone HTML file. The recorded build contains 3,631 file records, including 743 embedded resources, and a shared 21,244,434-byte source buffer. Each file record identifies a path, resource reference, source offset and length. HTML, JavaScript and SVG can reside in that buffer; stylesheets and texture resources also use external resource references. The decoder at `0x346bea0` establishes the corresponding native file array and byte buffer. Trailing binding, navigation and event arrays remain only partly decoded, so this is not a complete custom-page writer.

The engine's `ui::BlobResourceHandler` resolves requested URLs against registered page resources. Its regular request entry, `0x34446b0`, copies an embedded resource into middleware-owned response storage and finishes the response within that call. External resources and textures take separate paths; streaming requests use a different response interface. The [resource map](research/ui-resource-map.json) records those boundaries and both executable and middleware fingerprints.

The general middleware interface allows requests on different threads and permits deferred responses, including races with abort notifications. `Finish` invalidates the response object. These rules come from the official [resource-handler reference](https://docs.coherent-labs.com/cpp-gameface/api_reference/classes/classcohtml_1_1_i_async_resource_handler/) and [response reference](https://docs.coherent-labs.com/cpp-gameface/api_reference/classes/classcohtml_1_1_i_async_resource_response/). They do not establish binary compatibility: this game's reviewed response vtable has nine slots, with `Finish` at `+0x40`; current middleware declarations contain additional methods. Use the fingerprinted binary evidence for call layouts, and do not treat the synchronous embedded branch as a guarantee for every resource request.

Page creation follows a separate lifecycle: request the authored resource, instantiate the reflected page class, attach it to the UI manager, create the Cohtml view, then register its render representation. The manager chooses direct or queued render registration according to the current thread. Stream-out queues component removal; later cleanup detaches the view and releases the page. A resource being loaded does not mean a page is ready to receive model or event calls.

### Models and callbacks

`nl_coherent_texture_view_add_data_bindings`, callback `0x19da040`, reads `bindings` and `listBindings` tables and fields including `keyName`, `modelName`, `transformID`, and `type`. Producer `0x1b4ff30` deep-copies the descriptions into an owning request. Its consumer replaces the target context's binding arrays; it does not create an independent mod namespace. A missing view can consume the request without applying bindings.

Model setup resolves the middleware view, initializes its binding manager, and processes resource and custom descriptors. Duplicate model keys can be refused rather than overwritten. Context removal detaches the view and destroys its event state, binding manager and owned arrays. Complete value tags, existing-view updates and callback-release acknowledgement remain open.

`coregame::ui_events::system::handleLuaCallbacks` dispatches through `0x17e06a0` to `0x17df510`, which prepares serialized values and publishes event records using storage from `0x1a016f0`. This event path is distinct from resource-response completion and from page readiness.

### Development page extension

`runtime/diagnostics/native_ui_panel.html` is a self-authored development extension for the engine-owned options menu. It defines a **Mods** tab with example toggle, amount and reset controls using the game's existing classes and Cohtml renderer. Its controls do not grant engine mutation or establish arbitrary page creation. This development extension is not a public Wasm UI API.

Build with `build.bat -MovementProbe -Test`. With the game closed, install the resulting runtime, copy `dist/examples/native-ui/native-ui-panel.html` into the installed `crml/` directory, and create an empty `crml/native-ui.enabled` file. The regular player package does not enable this extension. Disable other UI-replacement mods for an isolated check; they can rewrite the same document or replace its resource handler.

Open **Options → Mods** with the mouse. Click the toggle, click or drag the amount bar, and use reset to restore **OFF / 50**. Values belong to the loaded page and have no gameplay effect or disk persistence. Controller navigation for the new rows is not implemented. Close and reopen Options, then reload a save and revisit the menu: the tab should remain usable without duplicate rows, and ordinary game options should continue working.

The tab requires a separate options-stack state before hiding the original content. If that state cannot be entered, the extension leaves the original options visible. Native option selection must not remain active behind a custom page. Cancellation and page teardown retain cleanup responsibility and wait while an unrelated state is observed above the extension's state.

The [UI stack queue map](research/ui-stack-queue-map.json) traces native `stack.push` and `stack.back`: both enqueue commands for a later engine update. A request must retain its owner while entry or exit is pending; the state immediately after a call is not an acknowledgement. Each transition is submitted once, then observed on later updates. Cancellation during pending entry must retain cleanup responsibility until that entry has completed.

The queue processes commands in order, but `back` applies to the current state at execution time and has no expected-state guard. Observing the current state before submission does not establish atomic isolation from other queued menu changes. This tab remains an isolated development experiment; concurrent menu extensions are unsupported.

The game's visibility binding can cache a page through `cloneNode(true)`. This preserves markup and marker attributes but drops JavaScript event listeners. Mounting therefore checks node identity and replaces stale cloned controls instead of treating a marker as proof that handlers are attached.

The interceptor requires the exact reviewed executable, middleware and original HTML fingerprints. A mismatch passes the original document through. Captures are limited to four pending responses, eight MiB per document and 32 KiB of extension markup. It forwards unrelated routes and unsupported response modes. `crml/native-ui.jsonl` records bounded counters for observed requests, transformed documents, source mismatches, completion and outstanding responses; it records no document contents or URLs. A transformed response proves that bytes were delivered, not that widgets were rendered. Actual display and navigation require an in-game check.

To disable the extension, close the game and remove `crml/native-ui.enabled`. The next launch uses the original page. Stopping native interception during a session does not remove an already loaded document; its controls remain owned by that page until cleanup or replacement. Deferred response ownership is retained, but concurrent method calls on the same response are not an established contract.

A general bridge still needs bounded mod-owned descriptions, owner-scoped event delivery, verified readiness and execution phases, and cleanup across navigation, page replacement and reload. Native callback pointers, shared engine model containers and unrestricted JavaScript must not cross the Wasm boundary. The Cohtml and V8 dependencies describe the UI middleware; they do not identify the gameplay scripting VM.

## Audio controls

The [audio control map](research/audio-control-map.json) connects an entity control request to authored event resources, backend playing IDs, stopping and emitter cleanup. These are reviewed static paths; CRML does not yet expose audio playback or stopping as Wasm imports.

### Request and consumption

`nl_audio_execute_control` at `0x3345940` takes an entity and a nonempty control-name string. It resolves component hash `0x352890c4`, **ActiveExecutionControlOutput**, and appends a 32-bit hash to its buffer. It returns no Lua value or playing ID. The hash starts at `0x811c9dc5`, sign-extends each string byte, ORs it with `0x20`, XORs the accumulator, then multiplies by `0x1000193` modulo 2^32. Zero is rejected. This is not a general Unicode or ASCII-lowercase routine.

The separate `flush_execution_control_queue` dispatcher at `0x3307570` calls `0x32f53b0`, which swaps 16-byte pointer/count/capacity records for **QueuedExecutionOutput** (`0x724ec178`) and the active output, then clears the queued count. The Lua binding writes the active buffer directly; a bridge cannot assume that every request passes through the queued buffer or append safely from an arbitrary thread.

`update_event_control` dispatches through `0x3305f00` to `0x32f5120`. Its definitions branch performs the following work:

1. Read control hashes from the active output and compare them with authored definition records. `AudioDisabled` skips processing; definition state at `+0x38` also gates this branch.
2. For a matching definition, remove callback bookkeeping and stop previously tracked playing IDs, then clear their count.
3. Post the definition's resolved event ID on the active backend object through `0x33e6d70`. A nonzero return is retained as a playing ID; zero is not added.

An unmatched name or unresolved event ID produces no new instance. Hash `0x13254bc4` bypasses the definition-name comparison and applies to every definition; its string spelling is unresolved. An optional authored control component also has a stateful branch through `0x32f4e00`. Consequently, an audio *control* is not equivalent to an arbitrary sound filename or an unconditional play operation.

### Resources and backend identity

`event_definitions_stream_in` reaches `0x32f3930`. Helper `0x32f3830` acquires each event resource through the shared resource manager, reads its backend event ID at `+0xbc`, retains the resource in active-event storage, and releases the temporary reference. Invalid or missing resources resolve to zero. Named definition records have stride `0x30`: resource reference at `+0x00`, control hash at `+0x18`, resolved event ID at `+0x1c`, and a playing-ID pointer/count/capacity record at `+0x20`.

`ActiveGameObject` has stride `0xa0`; its first eight bytes hold the backend object identity. The post wrapper receives this identity separately from the event ID and calls `0x352b1a0`, then records the result through `0x33e8290`. A context-local suppression list can also reject an event before posting. An ECS entity handle, a resource reference, a backend object ID and a playing ID are distinct values with different lifetimes.

### Stop and emitter cleanup

`nl_audio_stop_playing_id` at `0x3344220` reads a playing ID and numeric fade duration. For a nonzero ID, wrapper `0x33e79c0` multiplies the duration by `1000`, truncates it to an integer and submits backend command `0x21` through `0x35321f0`. This strongly supports seconds-to-milliseconds conversion, but backend timing has not been measured in gameplay. The binding also removes engine playing-ID bookkeeping through `0x33e7150`. It does not wait for audible completion.

Emitter stream-out reaches `0x32f4380`: it collects playing IDs for deferred bookkeeping cleanup, requests object-level stopping, queues the backend object for retirement, and replaces the active object ID with the all-ones sentinel. The separate cleanup system reaches `0x32f3350`, removes the collected playing IDs, retires queued backend objects through `0x33e5ae0`, and clears both queue counts. Stop submission, bookkeeping removal and object retirement are separate steps.

The first bounded candidate is executing an existing authored control on a live emitter. Before exposing it, the bridge needs evidence for execution phase and thread ownership, resource residency, callback completion, reload behavior and cancellation ownership. Raw playing IDs must not grant a mod permission to stop another mod's or the game's audio. The map does not establish custom sound-bank loading, arbitrary file playback, or a complete callback API.

## Animation events

`coregame::animationevent::updateLuaEvents` installs `0x1c1e190`, which calls `0x1c175b0`. The reviewed portion iterates animation event data, tests values/transitions, reserves storage atomically, copies event-name strings and value tags, and appends event records through `0x4a7870`. Storage allocation uses `0x1a016f0`, also seen in the UI event path.

This demonstrates queued animation-to-script event production. It does not establish a public serialization schema or the final consumption phase. Clip/source/mixer/graph/skeleton declarations in the index describe the surrounding surface; pose ownership, root-motion authority, and physics-animation synchronization need deeper tracing before a general animation API.

## Camera

`nl_camera_horizontal_fov` at `0x19cb5f0` resolves a camera through `0x19e0d80`, reads two floats at `+0x30` and `+0x34`, converts them through `0x1ba97c0`, and pushes one result. This callback is a **getter**. Its name alone is not evidence for a writable horizontal-FOV property or its units.

The [camera ownership map](research/camera-ownership-map.json) covers selection, free-camera transforms and the native switching path. Camera selection is separate from player movement and from the gameplay camera's mixer, lock-on and clipping systems. The existing Wasm `motion_camera` operation samples the player camera's horizontal basis; it grants no camera ownership.

### Camera selection and output

The `coregame::global::Camera` environment uses hash `0xfe90f7f8`. It contains a signed selector at `+0x00` and unaligned, 64-bit generational entity handles:

| Selector | Handle offset | Reviewed use |
| --- | --- | --- |
| 0 | `+0x04` | Player camera |
| 1 | `+0x0c` | Free camera |
| 2 | `+0x14` | Second mode using a free-camera transform; initialization assigns the same entity as slot 1 |
| 3 | `+0x1c` | Separate special-camera mode; its purpose is unresolved |

`coregame::camera::update` dispatches through `0x1bac190` to `0x1baa070`. The implementation selects the handle at `Camera + 4 + selector * 8`, resolves its view through `0x1addef0`, updates position history and derived velocity, prepares the camera listener output, and invokes the render-view path at `0x1bae4a0`. The debug labels in `0x1baddd0` independently identify the player and free slots. The render-view path can use a `CameraManView` when the selected entity has the corresponding component, so `CameraView` alone is not necessarily the final rendered pose.

A passive in-game snapshot found selector 0, valid generation-checked entities in all four slots, and an existing free-camera entity shared by slots 1 and 2. This establishes that free-camera state can exist during ordinary gameplay. It does not establish that every world provides those entities or that switching is safe at an arbitrary update phase.

### Free-camera source state

`CameraView` has component hash `0x46967561` and stride `0x40`: a nine-float basis at `+0x00`, position at `+0x24`, and two lens values at `+0x30`. `FreeCameraTransform` is a separate component, hash `0x01a3288e`, stride `0x70`.

The Lua binding `free_camera_position` at `0x23952b0` resolves slot 1 and reads or writes the free transform's position at `+0x30`. `free_camera_rotation` at `0x2395430` reads or writes three values at `+0x50`. Neither binding creates or selects a free camera. Rotation units and axis order are not established here.

An internal `coregame::freecamera::update` implementation is present. Registration installs `0x1addab0`, whose reviewed call goes to `0x1ad9e30`. Its first gate requires selector 1. It processes the free-camera source state, input and transforms; pose publication also reaches `0x1ae2c40`, which writes the `CameraView` basis and position. Editing only the published view can therefore compete with its producer.

Initialization dispatches through `0x1add760` to `0x1ada8c0`, initializes pose data and assigns the entity to slots 1 and 2. Removal dispatches through `0x1addc40` to `0x1adb040`, which clears both handles to the invalid sentinel. That removal helper does not itself restore the selector.

### Switching and restoration

The full native switch at `0x1ba8900`, reached through wrapper `0x1ba8b80`, does more than change the selector. For modes 1 and 2 it copies the current lens values, clears the destination free transform's handle at `+0x60`, derives a pose from the current view, and initializes the destination through `0x1ad9150`. It then changes both the world selector and its mirror at RVA `0x5c2c8b8`, refreshes position history, and submits a separate engine message. The meaning and ownership of that message are not established by this trace.

The simpler setter at `0x1ba7ea0` updates the selector, its mirror and position history without that pose-initialization path. An existing engine caller at `0x2274d29` uses the full switch to return from mode 1 to mode 0. A mod must respect such external transitions rather than continually reasserting its selected camera. Neither entry checks all the lifetime and destination preconditions a mod-facing API needs. These recovered addresses are not exposed as callable mod APIs.

A camera lease must account for world and entity generations, engine update order, native free-camera input, competing camera switches and component removal. Releasing a lease should return to the still-valid gameplay camera without restoring an old player pose over its current state. A transition that invalidates the owned camera must cancel pending writes. Live switching, conflict handling and restoration remain outside the current camera API's guarantees.

## AI and navigation

The registered Bonsai behavior-tree update specialized for `heron::GameBehaviorTreeContext` dispatches through `0x2522b00` to `0x2521230`. That implementation collects work into a temporary buffer, uses task helpers `0x3271560` and `0x3271140`, submits through `0x3271690`, and calls additional completion/lifetime helpers before releasing its temporary storage. The reviewed path contains a parallel-for source anchor and an update-behavior-tree work label.

This supports parallel work in that update path; it does not establish that the main thread owns all AI state or that those helpers constitute a public job API. Navigation mesh updates, link/obstacle changes, workspot searches, knowledge, and EQS are separate families in the index. Full worker lambda arguments, cancellation, and interactions with world unload remain open.

## Save and load

`coregame::savegames::processWriteRequests` dispatches through `0x18c3770` to `0x18c0f90`. The latter traverses records at stride `0x5d8`, branches on request state, builds save-chunk storage and invokes serialization-related helpers. `processLoadRequests`, container updates, typed header caching, and gameplay save bindings are separately registered.

This is a concrete request-processing entry, not a recovered save-file schema or proof of atomic/durable disk writes. Save cancellation, completion callbacks, version migration, checksums, and reconstruction of live entity/resource state remain unresolved.

## Reproduce the reference checks

```powershell
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
python tools/verify_engine_map.py "$gameDir\CONTROLResonant.exe" docs/research/engine-paths-map.json
python tools/verify_engine_map.py "$gameDir\PhysX_64.dll" docs/research/physx-shape-map.json
python tools/verify_engine_map.py "$gameDir\CONTROLResonant.exe" docs/research/ui-page-lifecycle-map.json
python tools/verify_engine_map.py "$gameDir\CONTROLResonant.exe" docs/research/ui-model-lifetime-map.json
python tools/verify_engine_map.py "$gameDir\CONTROLResonant.exe" docs/research/ui-resource-map.json --middleware "$gameDir\cohtml.WindowsDesktop.dll"
```

Static reference checks compare encoded references with the fingerprinted binary. The UI resource map contains a separate middleware fingerprint and reference set: `--middleware` checks that set against the DLL rather than the executable. Omitting it checks only the executable portion and reports that middleware was not checked. These commands do not execute engine functions or establish callable ABIs, object lifetimes or gameplay behavior.
