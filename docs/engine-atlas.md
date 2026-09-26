---
title: Northlight engine atlas
description: Browse CONTROL Resonant engine systems, script bindings, rendering, physics, resource lifetimes, and native reference maps.
---

# Engine atlas

This reference maps the recovered engine surface from packaged assets through ECS systems, scripting, rendering, physics, and gameplay. It describes subsystem responsibilities and native code references. For guest functions, see the [Wasm API](api.md).

The [complete system index](engine-system-index.md) contains **2,474 recovered system declarations across 543 namespace families**. The [machine-readable atlas](research/engine-atlas.json) also contains **904 script-binding candidates**, **1,044 imported functions from 49 modules**, and the declared component/environment inputs. Registration patterns identify candidate dispatchers for **2,143 systems**; **331 remain unresolved by that pattern**. Families group the first two namespace segments and are not a count of independent engine subsystems.

Coverage is complete for the supplied executable catalog. It is not complete source recovery: unnamed code, stripped declarations, dynamically resolved functions, scheduler dependencies, and untraced branches remain outside that claim. A registered debug system may also be inactive in ordinary gameplay.

## Reading the evidence

| Level | What it establishes | What it does not establish |
| --- | --- | --- |
| Declaration | Exact compiler-generated name and declared argument types, checked against the executable | Layout of every type, execution order, or behavior |
| Registration pattern | Encoded name/callback associations; candidate system descriptor slot `+0x140` | A public callable ABI or proof that the system runs |
| Reviewed path | Selected disassembled instructions and calls, with fingerprinted reference checks | Behavior of unvisited branches or live thread/lifetime guarantees |
| Runtime observation | A recorded operation or value in a particular session | General safety across reloads, updates, and every entity type |

The atlas preserves raw access and qualifier numbers from ECS templates. Their enum meanings have not been established, so they must not be treated as read/write permissions. Registration code, a dispatcher that unpacks ECS arguments, and the actual operation are different functions. A PE exception-table range may cover only a fragment of a function.

All executable RVAs refer to SHA-256 `2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0`. Backend DLLs have separate fingerprints and address spaces. An RVA is added to the actual loaded module base; it is not an absolute address or a portable API identifier.

## Foundation, identity, and world ownership

| Area | Recovered surface | Operations | Current boundary |
| --- | --- | --- | --- |
| ECS/world | `ecs::World`, typed queries, removed-component queries, command buffers, entity generation | Enumerate entities, read components, request structural changes | Declarations indexed; entity inspection and selected access paths covered in [entity internals](engine-internals.md) |
| Scheduling | Fixed/variable update declarations, factory and stream flushes, task helpers | Select the owning update stage, defer commands, wait for completion | Selected flush/AI paths reviewed; full dependency graph and worker ownership unresolved |
| Identity | `global_id_to_entity_map`, resource IDs, bundle IDs, live entity handles | Resolve persistent content to a current entity | These identifiers have different lifetimes; raw scene slots are not validated entity handles |
| Transforms/hierarchy | `transform`, attachment factories/sockets, skeleton slaves, local time scale | Move an object, attach it, update local/world transforms | Declarations indexed; transform propagation and ownership are not specified by declarations |
| Time/input | `time`, `legacy_time`, fixed/variable input, gyro, player input and input logic | Read actions, control time, suppress gameplay input during a mod operation | Declaration coverage; ordering, action consumption, focus and pause semantics unresolved |
| Streaming/lifetime | `streaming`, broadphase, bundles, lifetime, hotload, manually managed resources | Load/unload regions, retain resources, react to removal | Selected resource retain/release paths reviewed; no arbitrary long-lived component pointers |
| Spawn/remove | `spawning`, `luaspawning`, NPC/player spawners, bundle roots | Instantiate bundles, await readiness, remove entities and children | [Spawn and removal paths](engine-paths.md#world-spawning-and-removal) show structural commands and cleanup stages; full creation ABI unresolved |

An entity can exist before its resources, physics body, or render handle are ready. Removal can likewise start before each subsystem has released its owned objects. A pointer lookup alone does not establish readiness, validity, or world ownership.

## Physics and movement

The physics surface spans engine resource construction, scene ownership, backend actors/shapes/materials, simulation, and gameplay systems that consume the results. The [physics dynamics reference](physics-dynamics.md) and [scene/body mappings](engine-internals.md) contain the deeper field and call evidence.

| Property or operation | Evidence available | Scope limits |
| --- | --- | --- |
| Static/dynamic friction and restitution | Material registry fields and backend material association | Shared-material ownership, per-shape override/clone policy, existing contacts, restoration |
| Mass and inertia | Backend setters/inverse storage; game inertia calculation, principal-axis conversion and conditioning | Units for each game-facing operation, compound-shape updates, wake and phase guarantees |
| Force/impulse/torque | Script-to-game helpers, backend mode dispatch, accumulator storage and consumption | Entity-type coverage and phase-correct invocation |
| Velocity/acceleration | Distinct accumulated velocity-change and acceleration terms; conditional retention | Public route to every retention flag; sleeping, kinematic, and controller behavior |
| Damping/sleep/stabilization | Concrete backend setters, tagged alternate damping storage and propagation paths | Game ownership, thresholds/units, state transitions and override producers |
| Solver/contact tuning | Iteration packing, contact impulse/report limits, depenetration storage, CCD advance and axis-lock entries | Full CCD/lock flag semantics, input ranges, solver consumers and engine-facing contract |
| Gravity | Body enable flag, scene gravity construction, temporary simulation override | Interaction with local gravity anomalies and gameplay resets |
| Collision filtering | Separate shape query/simulation filter storage and game filter packer | Full layer/mask meanings, filter shader, pair invalidation and gameplay ownership |
| Geometry/shapes | Shape creation/attach/release path; collision package resource loading | Geometry format, ownership of shared shapes, safe resizing/replacement |
| Contacts/queries | Raycast, contact report, physics event and sensor declarations | Filter callbacks, payload ABI, trigger/contact timing and lifetime |
| Constraints/joints | Immediate TGS contact/joint construction and solver imports | Complete constraint descriptors, joint ownership and result writeback |
| Simulation phases | Normal completion task and separate immediate solver path | Complete scheduler graph, lock ownership, every consumer after completion |
| Character/ragdoll/cloth | Character controllers, movement models, physics animation, ragdoll, cloth and NvCloth dependency | Relationship between authoritative pose, gameplay movement, and backend objects |
| Destruction/out-of-bounds | Destruction systems, rigid-body factory bounds handling, player fall/respawn systems | Detection-to-effects-to-reset ownership; disabling collision alone does not disable these systems |

“Zero friction everywhere” therefore involves more than one float: material sharing, contact updates, and non-rigid-body movement all matter. Likewise free flight, collision bypass, camera clipping, fall detection, and respawn effects are separate integration problems.

## Rendering, animation, and presentation

| Area | Recovered surface | Reviewed evidence and gaps |
| --- | --- | --- |
| Mesh/material resources | Mesh and material stream-in/out, material IDs/resources/render handles, override targets | Resource ownership plus [material command submission](engine-paths.md#materials-and-renderer-handoff); GPU consumer unresolved |
| Material parameters | Material parameters and mesh overrides | Reviewed emission/color parameter helper calls; parameter types, complete set and lifetime remain open |
| Visibility/LOD | Hide reasons, parent/bounds hiding, hierarchy/visual LOD, culling-related gameplay systems | [Player visibility](visibility.md) has a prior live confirmation; unrelated renderer operations are not thereby verified |
| Lights/environment | Baked/directional/point/spot/cinematic lights, fog, scattering, illumination/SDF volumes, wind | Declared inputs indexed; individual render command semantics untraced |
| Effects | Particles, decals, flares, gfx graphs, collision/impulse/sound/shake emitters | Factory/update/removal families indexed; effect-instance handles and completion callbacks unresolved |
| Renderer synchronization | Variable update renderer sync and command publication | Reviewed counter wait and queue notifications; no established GPU fence or full render graph |
| GPU/middleware | NRI, NRD, FidelityFX loader, Streamline interposer imports | Imported function names are evidence of linkage, not the executed pass graph or full feature configuration |
| Animation | Clips, sources, graphs, inputs, mixer stages, skeletons, RBF, look-at, wrinkle, animation slaves | Declarations indexed; [animation-to-script event production](engine-paths.md#animation-events) reviewed; full pose-buffer ownership open |
| Timeline/cinematics | Timeline transforms, animation, dialogue, camera, particles, scripting, video, early exit | Families indexed; seeking, cancellation, and state restoration unresolved |
| Camera | Camera/cameraman, FPS/free/timeline cameras; gameplay camera mixer and clipping | [FOV getter and free-camera update](engine-paths.md#camera) reviewed; activation and ownership unresolved |
| UI | Page lifecycle, input bindings, texture views, data bindings, callbacks, captions, blur | [UI model and event paths](engine-paths.md#ui-models-and-events) reviewed; page ownership and resource extension contract open |

The renderer queue is a real intermediate boundary. Changing an ECS material field is not equivalent to submitting a GPU operation, and publishing a render command is not proof that the GPU has finished using its resources.

## Scripting and callback surface

The traced `.binlua` resource path reaches a **Luau-like bytecode loader** with bytecode versions 3–6 and type versions 1–3 in this executable. The interpretation follows the binary checks and comparison with [upstream Luau's loader](https://github.com/luau-lang/luau/blob/master/VM/src/lvmload.cpp); the exact fork and compiler compatibility remain unverified. See the [script path](engine-paths.md#script-resources-and-the-vm) for evidence and the resource envelope.

| Callback family | Observed or declared behavior | Integration concern |
| --- | --- | --- |
| Script update | Active-script callback registration plus fixed-update dispatch reviewed | Script instance, callback reference, replacement and teardown ownership |
| Custom script events | Handler lookup and immediate invocation reviewed | Reentrancy, callback failure, mutation during dispatch |
| General handler installation | Active-script check and immediate/deferred registration branches reviewed | Which branch applies, parameter schema, removal while unloading |
| Animation events | Names/values serialized and event records queued | Payload lifetime and the later script-consumption stage |
| UI callbacks | UI pending-event processing and serialization reviewed | Page/model lifetime and fixed/variable cleanup phases |
| Physics events | Contact/trigger/raycast systems declared; selected body callbacks previously traced | Simulation completion, copied payloads, no retained transient backend pointers |
| Spawn events | `luaspawning::listen_for_spawn_events`, script initialization stages declared | Spawn request accepted versus entity ready versus script initialized |
| Audio/music callbacks | Callback and playing-instance families declared | Audio-thread handoff, event lifetime and cancellation |

Engine scripting is separate from the Wasm sandbox. The mapped operations are not guest APIs and do not grant access to the engine VM or native callback pointers.

## AI, audio, and gameplay

| Area | Recovered surface | Current boundary |
| --- | --- | --- |
| AI decisions | Bonsai behavior trees, knowledge, target picking, hazard handling, game AI behaviors | Behavior-tree update constructs parallel work; complete [job lifecycle](engine-paths.md#ai-and-navigation) unresolved |
| Navigation | Navmesh, obstacles, modifier zones, links, EQS, workspots, cover, reservations, Mercuna-named systems | Declarations indexed; navigation queries are distinct from physics queries |
| Audio | Wwise controls, listeners, transforms, parameters, playing instances, pause and sound spawning | [Control enqueue and buffer handoff](engine-paths.md#audio-controls) reviewed; full backend execution ABI unresolved |
| Acoustics | Acoustic grids, obstruction/occlusion, baked propagation, requests/results, load balancing | Producer/consumer families indexed; propagation data and scheduling untraced |
| Dialogue/music | Dialogue resources, timeline sound, subtitles, music callbacks and gameplay music | Declaration coverage; synchronization and cancellation contract open |
| Player/combat | Player lifecycle, health/damage/death, targeting, melee, projectiles, abilities, status effects | Full recovered declarations indexed; names do not establish writable health/ability APIs |
| Progression/content state | Quests, facts, world state, encounters, inventory UI, currency, rewards, shops, upgrades/unlocks | Dependencies across persistent and transient state remain untraced |
| Game flow | Zone transition, fast travel, challenge/new-game-plus, save/load and UI flow | High-level systems must be distinguished from low-level world/resource actions |
| Save/load | Core save containers, request processors, header caching, gameplay save requests | [Write-request traversal](engine-paths.md#save-and-load) reviewed; file schema, transaction and recovery behavior open |
| Platform/services | Achievements, entitlement, analytics, activity, remedyservices, Steam/Nakama/cURL imports | Static surface only; no service calls, credentials, or network behavior researched by invocation |
| Diagnostics/tooling | Debug draw, animation preview, live camera, hotload, internal free camera | Presence does not prove accessibility or support in a retail session |

## Searching and reproducing the atlas

The generated index lists every recovered family, including anonymous namespaces and game-specific systems that do not fit a single category above. The JSON preserves overloads and duplicate registration sites rather than collapsing them into an assumed API.

```powershell
python tools/engine_atlas.py query docs/research/engine-atlas.json system coregame::spawning
python tools/engine_atlas.py query docs/research/engine-atlas.json component Physics --limit 20
python tools/engine_atlas.py query docs/research/engine-atlas.json environment LuaEvents
python tools/engine_atlas.py query docs/research/engine-atlas.json binding gravity
python tools/engine_atlas.py query docs/research/engine-atlas.json import PhysX
```

Rebuild from a locally generated [research catalog](engine-research.md), without executing the game:

```powershell
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
python tools/engine_atlas.py build "$gameDir\CONTROLResonant.exe" .local/engine/baseline.json --output docs/research/engine-atlas.json --index docs/engine-system-index.md
python tests/test_engine_atlas.py
```

The builder checks the exact executable fingerprint, declaration strings, name references, binding callback references and store encodings. Dispatcher scanning stops at the next system-name reference or exception-table fragment boundary. This intentionally misses some noncontiguous registration sequences rather than inventing associations. Import indexing covers the normal PE import table, not delay imports, runtime resolution, or all statically linked code.

## Runtime diagnostics

The [engine observer](engine-validation.md) records update phases, body and entity identities, and damping readbacks. Its analyzer reports event order, identity changes, and dropped records. See the [capture format](engine-validation.md#capture-format-and-loss-handling) for record fields and interpretation.
