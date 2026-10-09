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

`runtime/diagnostics/native_ui_panel.html` extends the engine-owned options menu with a **Mods** tab using the game's classes and Cohtml renderer. Development builds populate it from the bounded [typed settings API](mod-settings.md). Guests provide definitions and read values; they do not submit HTML, JavaScript or native pointers. This does not establish arbitrary page creation or engine mutation.

Build with `build.bat -MovementProbe -Test`. The runtime package includes the panel and prepares it when a mod declares `settings`; no diagnostic marker is required. The `examples/settings` package demonstrates Boolean, integer and numeric preferences. Other UI-replacement mods may rewrite the same document or replace its resource handler; coexistence is not guaranteed.

Open **Options → Mods** with the mouse. Controls are grouped by mod ID. Values belong to the loaded guest instance and survive page replacement; guests choose whether to persist them through the separate storage capability. The example does not change gameplay. Controller navigation for mod rows remains unsupported. The generalized page is experimental; automated browser checks do not establish live renderer behavior.

The tab keeps the existing native Options state and its input context. While its controls are visible, it temporarily detaches the six stock Options control callbacks by their exact function identities and installs its own cancel handler. Unrelated event listeners remain registered. Missing callback exports leave the original options visible. The stock tabs keep their binding-owned classes, with their selection styling suppressed until Mods closes.

Cancellation restores the stock callbacks after the current event dispatch, so one cancel cannot also close the underlying Options menu. Closing Options, replacing its route or replacing its DOM dismisses the extension. Callback restoration is limited to the same active native route; inactive menus rely on their normal activation lifecycle.

The [UI stack queue map](research/ui-stack-queue-map.json) traces native `stack.push` and `stack.back`: both enqueue commands, and `back` has no expected-state guard. The Mods extension does not issue either command or add an unregistered engine state. It remains an isolated development experiment; concurrent menu extensions are unsupported.

The game's visibility binding can cache a page through `cloneNode(true)`. This preserves markup and marker attributes but drops JavaScript event listeners. Mounting therefore checks node identity and replaces stale cloned controls instead of treating a marker as proof that handlers are attached.

The interceptor requires the exact reviewed executable, middleware and original HTML fingerprints. A mismatch passes the original document through. Captures are limited to four pending responses and eight MiB per document. Each extension file is limited to 64 KiB, with 128 KiB of combined extension markup. It forwards unrelated routes and unsupported response modes. `crml/native-ui.jsonl` records bounded counters for observed requests, transformed documents, source mismatches, completion and outstanding responses; it records no document contents or URLs. A transformed response proves that bytes were delivered, not that widgets were rendered. Actual display and navigation require an in-game check.

To disable the extension, close the game and remove `crml/native-ui.enabled`. The next launch uses the original page. Stopping native interception during a session does not remove an already loaded document; its controls remain owned by that page until cleanup or replacement. Deferred response ownership is retained, but concurrent method calls on the same response are not an established contract.

A general bridge still needs bounded mod-owned descriptions, owner-scoped event delivery, verified readiness and execution phases, and cleanup across navigation, page replacement and reload. Native callback pointers, shared engine model containers and unrestricted JavaScript must not cross the Wasm boundary. The Cohtml and V8 dependencies describe the UI middleware; they do not identify the gameplay scripting VM.

### Native input contexts

The [input context map](research/input-context-map.json) records the October executable's controls flags, derived context predicates and UI navigation checks. The [0.564.478.0 adapter map](research/input-context-hotfix-map.json) records the reviewed input and menu observer relocations for the later hotfix. Each fingerprint is separate from the older UI stack map; addresses must not be mixed between builds.

`nl_player_controls_enabled` tests two raw control-enable bits. The engine separately derives a context word and attaches predicates to action views. Both digital and analog action readers check these predicates, backend availability and generation. The raw Lua query can return true while the derived context word is zero, so it cannot by itself authorize mod input suppression.

The traced input-environment population copies the third controls view into the player environment, cloning its predicate while retaining a borrowed reference to the controls object. A disconnected controls object selects an empty fallback instead of its embedded view. This establishes one producer-to-consumer path; it does not establish every environment's refresh frequency or prove that all input consumers share the same view.

Native UI navigation also requires a matching applied stack state and an active stack. Absence of navigation polling does not establish gameplay ownership: transient guards, mouse interaction and text entry can follow other paths. The map records static behavior; exclusive gameplay ownership and a complete mapping of context bits remain unestablished. The SDK's foreground-focus flag continues to describe window focus only.

The named `menuModeSystem` provides a separate positive menu-transition path. It matches navigation contexts against applied, active UI stacks and compares the result with raw controls bit 5. A mismatch queues an enter or leave request; the controls configuration is applied elsewhere. Request production alone is therefore not a completed input-mode change. The `ui::PageContent` listener also has a Cohtml `OnTextInputTypeChanged` slot, but its imported default handler does not establish a stored capture flag, notification lifetime or active-page ownership.

#### Optional input observation

Development builds with `CRML_MOVEMENT_PROBE` include two independent read-only observers enabled by an empty `crml/input-context.enabled` file beside the installed runtime. Restart the game after adding or removing this file. Each observer requires the reviewed October update or **0.564.478.0** hotfix executable and its matching hook entry; original and unknown executable profiles are refused. The hotfix moves the controls update, static object and predicate table while retaining their reviewed fields. The menu helper and its caller remain at the same addresses.

The observer copies flags, connection state, predicate identity checks, backend generation, source availability and the update thread ID after the native context update returns. It follows the native reader's short-circuit order: a disconnected or stale view does not justify following its predicate and source pointers. Pending updates invalidate the previous sample; copied observations expire after 100 ms. An engine update that unwinds leaves observation unavailable for the rest of the process.

The second observer copies the menu helper's two output bytes after normal return to its reviewed caller: whether an active context matched and the raw `+0x5c` byte from the last matching context. It checks that the caller initially zeroed both outputs and retains no engine pointers. Pending, overlapping or unwound calls are unknown; copied observations expire after 100 ms. The second byte has no established menu-subtype meaning. A negative scan does not establish exclusive gameplay input ownership.

The session log records at most 64 semantic changes **per observer**, sampled no more often than every 250 ms. Status and observed fields trigger records; callback-thread migration alone does not spend this budget. Thread ID is included when a record is emitted. `source_checked=0` means source availability was not evaluated, not that the engine lacks a source. `status=unavailable` can also mean there is no fresh completed update. The separate controls and menu records are not a coherent engine-frame snapshot or a complete transition history. No input is suppressed, no controls data is written, and no guest permission is granted by these observations. Menu, text-entry, loading and reload behavior still require gameplay evidence.

## Tutorials

The [tutorial map](research/tutorial-map.json) separates two native presentation
paths. Dynamic tutorials provide HUD hints with localized text, optional images,
timer progress and a dismiss-input callout. Static tutorials use the
`game/static_tutorial` UI state, center or side layouts, native input handling
and UI events. Their sync system also receives fullscreen-blur state.

The recovered `show_tutorial`, `set_tutorial_active` and `complete_tutorial`
bindings operate on entity tutorial data. `show_tutorial` validates a string
identifier against an existing table before queuing a request; an unknown
identifier produces a script error. It does not accept arbitrary heading/body
text or register a custom tutorial.

The pre-save initializer converts asset tutorial records into an owned hash
table in `TutorialData`. Runtime entries contain page vectors and auxiliary
vectors; page construction, move insertion and whole-table destruction are
mapped. Insertion can rebuild the table or relocate slots within its existing
allocation, so lookup pointers are not stable handles. Duplicate detection
compares a 32-bit key, which also means a mod
namespace alone cannot guarantee collision-free registration.

Both presentation paths resolve message text from the same page data. The
resolver selects a matching conditional message or the page's default text
and returns a borrowed view. This establishes the shared data path, but not a
guest interface for localized text or remappable control prompts. Whole-table
destruction is not suitable for removing one mod's tutorials.

The request lifecycle distinguishes activation, deactivation, dismissal,
completion and display. Deactivation removes active-list membership but does
not clear every queued or selected reference. Completion changes native
progression state and expects the tutorial record to remain present; deleting
that record while completion is pending reaches an assertion path. Neither
operation is a general unload function.

Static panels can read either the primary tutorial table or a separate table
in the same component. The primary path queues completion after the final
page; the secondary page-end branch does not. This distinction may support
separate mod presentation, but custom registration and isolation are not yet
established. Closing a panel navigates the shared UI stack, so an adapter must
account for intervening menus and observe the resulting transition before
reporting that input has been restored.

Secondary records own a page vector and move it into the table on successful
insertion; duplicate keys leave ownership with the caller. Their slot pointers
can change during either table growth or in-place compaction. Destroying a
page vector alone does not remove its hash-table entry or any UI references.

Initializer registration refers to `Variable update before fixed` and
`Fixed Update`, with writable access to `TutorialData`. These are scheduling
metadata, not proof that a mod can mutate the component from a native UI
callback or worker thread. Actual ordering and component lifetime remain
requirements for an adapter.

The initializer also has an archetype-job caller at `0x2044cc0`, which
constructs the same two-component query and calls the worker from `0x2044d60`.
Live observation found hint synchronization, panel synchronization and panel
events executing on multiple worker threads. A cached thread ID therefore
cannot serve as an ownership check. An initializer invocation reached the
worker through a different caller than the original dispatcher; that capture
did not record its return address, so it cannot identify the alternate job as
the caller. The observer now records module-relative caller addresses while
continuing to reject unreviewed callers before reading their arguments.

The experimental [Wasm tutorial API](mod-tutorials.md) uses separate mod-owned
page data passed to the native renderers. It does not insert records into campaign
tables. Native payload construction, scoped open/close operations and retirement
guards have automated coverage; gameplay presentation, input restoration and
world transitions still require qualification. Reusing tutorial CSS classes for
passive feedback is a separate path and does not provide these lifecycle behaviors.

### Record removal

The traced tutorial cleanup functions destroy whole tables, component arrays or
temporary setup data. None provides a verified per-key removal operation.
Erasing a record also requires maintaining the table's empty and deleted slot
markers, mirrored control bytes, size and insertion capacity; freeing its page
vector alone leaves that bookkeeping inconsistent.

A removal routine for a different native table supplies a metadata comparison.
It chooses between an empty marker and a tombstone based on neighboring groups,
preserving lookup paths for surviving entries. Tutorial tables use matching
control conventions, including special mirror positions in small tables and an
alignment-padding write in larger tables. The map records these details for
adapter development. The other table's routine accepts a different key type and
is not a callable tutorial-removal API. This structural evidence does not resolve
payload ownership, outstanding references or a safe mutation phase.

### Text storage and retirement

Both presentation paths submit the selected page message through `0x1e54fa0`
to the model string writer at `0x1b279b0`. Values shorter than 16 bytes are
copied into inline model storage. Longer values use an engine string pool at
`0x1b2a420`: an existing content-hash entry supplies an index, while new or reused
slots receive a string assignment. The resulting model value does not retain
the original page-string pointer.

The native `garbageCollectFactDictionary` worker at `0x1b28d60` marks pooled
strings referenced by dictionary entries, enabled Lua fact listeners and Lua
fact events. It clears unreferenced strings, removes their hash entries and
makes their indices reusable. Clearing a string retains its allocated storage;
this is not proof that memory usage drops immediately or remains bounded under
repeated messages. Live collection timing and synchronization with tutorial
producers remain unestablished.

The pool belongs to the shared `FactDictionary`, not to a particular tutorial
or mod. Its reset and destructor paths destroy all pooled strings. An adapter
must let native collection manage reuse after model and event references retire;
it must not reset the dictionary or clear pooled strings on mod unload. Pool
indices can be recycled and must not become persistent guest handles.

The stock templates display localized heading text and render message
translations through `data-bind-html`. The `LocString` wrapper forwards the
identifier, not a supplied `translation` value. The shared native getter first
looks up known localization keys; a valid UTF8 missing key takes a cached echo
fallback. This is not an exact-literal interface: key collisions and localization
debug modes can change the displayed text, and the body binding does not escape
guest markup.

The missing-key cache is separate from the `FactDictionary` string pool. Each
distinct key adds a 456-byte node, with possible additional string allocations.
Localization resource replacement and manager destruction clear it; no periodic
or practical size eviction is established. Queue and rate limits alone therefore
do not bound retained text. An adapter needs finite distinct-text admission and
an explicit localization and escaping contract, without clearing shared state.

The HUD tutorial's dismiss callout uses the native `CLOSE_TUTORIAL` action for
its label, icon and hold indicator. The stock controls schema connects this
action to `AUDIO_LOG_PLAY`. Its input worker at `0x1fe8e60` checks the displayed
tutorial, its dismiss mode and the native input predicate, then appends request
kind 3: completion. It does not perform owner-scoped cancellation. Static panels
instead display `MENU_SELECT` and `MENU_HORIZONTAL`; mouse continuation sends
`static_tutorial_continue`. These stock navigation controls do not establish an
arbitrary guest action-to-glyph interface.

Removing a tutorial record is not a cancellation operation. Static panel
synchronization returns without closing when an unchanged page loses its
record; the changed-page branch instead calls the shared stack-close helper.
Even with a valid record, an out-of-range page can update page metadata while
leaving the previous heading and message intact. Page bounds must remain valid
until the presentation has retired.
The active hint refresh can also return after a failed lookup without clearing
its published state. A fading hint still reads its record. Timer expiry changes
its state to hidden before a later update clears or replaces its displayed ID;
the input worker can still submit completion for that ID in between. Neither
deselection nor hidden state alone acknowledges that these references are gone.
An adapter must stop new requests, retire the owned
presentation and its references, and only then remove its data through a
verified per-entry erase operation. Whole-table cleanup cannot provide this
contract, and model-owned strings must not be freed as mod-owned page storage.

The internal request adapter can withdraw an owned identifier from `Requests`,
`PendingCompletion` and `SelectionQueue` within a matching request-dispatch
scope. It preserves other entries and their order, leaves allocations intact,
and reports remaining active/selected references. Native completion admission
also deduplicates pending identifiers through its append helper. Neither
mechanism establishes that an identifier's presentation or callbacks have
retired. The withdrawal routine has native-layout test coverage; registration
ownership and a live retirement path are not connected, and no guest tutorial
API is exposed.

Queue preparation rejects inaccessible, overlapping or oversized buffers before
writing. An unexpected fault during in-place compaction is a partial failure;
the caller must retain the payload and reconcile or recover, rather than retry
blindly. This operation does not change game completion or dismissal history.

For static panels, the native visibility predicate also accepts an ancestor
screen underneath another menu. Closing such a panel through the native stack
helper can pop intervening screens. An owned close must match the tutorial ID
and mode, preserve the native context-active check, and require the first
matching screen to be the current applied entry above index zero. These checks
belong at the UI execution boundary. The event processor receives mutable
`TutorialState` and `UIStateStacks` environment references; they are not covered
by the request worker's ECS component access. Environment writes are registered
separately: readers and writers of the same environment enter the scheduler's
conflict graph without the entity-query filters. Mutable environments also force
whole-system execution instead of archetype subdivision. A synchronous operation
inside the event processor can use its declared environment access until that
callback returns. This does not establish a particular thread or permit writes
to `TutorialData`, which the event processor reads only.

The native context-active predicate at `0x17d8680` accepts a nonzero explicit
activity byte at context `+0xb8`; with that byte clear it follows relationships
between contexts. Preserve the full predicate instead of inferring activity
from one flag or screen name. A close operation still requires later
confirmation that visibility and input ownership have been released.

The internal panel adapter implements these close guards for the reviewed
executable. It also checks the current world, full entity handle, event caller,
and three declared environment arguments. Any pending native presentation event
defers the close. Canonical stack and state names come from the same native
string objects used by the close helper; visibility alone is never sufficient.
A successful call returns `await_sync`, and a fault during the call returns
`partial`. Neither result permits payload destruction. The registration owner
must retain its record until presentation and input retirement are established.
These guards have automated native-layout coverage; custom registration and
the live retirement sequence remain incomplete.

### Read-only observation

The request dispatchers supply a world view, a 16-bit system or archetype ID,
and a system descriptor. The inner request worker receives seven component
arrays, a chunk and a row. Its full entity handle is stored in the chunk header.
The runtime checks the handle generation, current location, chunk and row count,
then resolves all seven component hashes back to the supplied arrays. World
context is borrowed only within the current thread's dispatcher call; nested
calls cannot inherit an invalid outer context. Cleanup covers both C++ and
Windows structured exceptions without swallowing engine exceptions.

Panel-event observation uses a separate whole-system dispatcher scope. It
resolves the two entity component arrays and checks that the worker received
the current `TutorialState`, `UIStateStacks` and `AudioEventRequests` objects.
Schema 6 includes the resulting salted world/entity tokens for panel events
as well as requests. The observer does not invoke the close adapter or modify
tutorial data.

The scheduler converts declared component conflicts into directed dependencies
within each graph, ordered by the supplied system sequence. Callback return
precedes release of successor dependencies. Required/excluded component filters
and alternative groups can suppress conflicts, and graph batches have separate
boundaries. This establishes an ordering mechanism for declared components,
not exclusive access to the entire engine. In particular, the request worker's
component access does not authorize changes to the shared UI stack or
localization cache. See the map's `request_writer_boundary.scheduler_contract`
for the traced graph construction and execution paths.

An experimental build can observe five tutorial phases by placing an empty
`tutorial-observer.enabled` file beside `crml_runtime.dll` before launch. The
observer accepts only the executable fingerprint recorded in the tutorial map
and checks the entry bytes and calling instructions before installing hooks.
It forwards the original calls without submitting tutorial requests or changing
their data. Remove the marker and restart to disable observation.

The session log records initialization, hint synchronization, panel
synchronization and panel events as phases 0–3. Schema 3 adds request processing
as phase 4, at the native worker whose access descriptor includes writable
tutorial data and request queues. Both reviewed dispatch paths supply the same
data base and row fields. Schema 5 also hooks the two outer request dispatchers
to check the current world and entity before recording request identity.
Observing this worker does not establish permission to mutate its data.
Capture lasts up to ten minutes,
with at most 1,024 sampled spans per phase and a fixed 256-record buffer.
Records include thread IDs, performance-counter timestamps and salted storage
tokens; counters report overlap observations, query rejections and dropped
samples. Sampling is shared by all callers and rows within each phase. A busy
row can consume the budget; at the maximum rate it is exhausted before the
ten-minute capture ends. Schema 4 reports budget usage and exhaustion explicitly.
Schema 5 adds `identity`, `world` and `entity` fields. Identity status 1 means
the scoped component checks succeeded; 0 means no identity was available, and
2–7 describe rejected scope, query, entity, components, changed data or memory.
World and entity values are salted tokens rather than published addresses.

Capture is one-shot per process. It also stops when the existing runtime worker
exits; it does not keep that worker alive. Closing admission prevents new
observation spans, while already-admitted spans remain pending through event
publication. A closing report with `final=0` is partial; `final=1` follows drainage
of admitted spans. Worker shutdown may leave only a partial report. Neither
status establishes that uninstrumented engine readers have finished. Live totals
remain observational snapshots. Storage tokens describe component locations;
entity tokens include the current handle generation and world address. Neither
token establishes a persistent save identity or a world lifetime. Memory reuse
and unobserved changes can invalidate lifetime inference. Overlap
observations cover these instrumented calls, including observer overhead;
their absence does not establish exclusive access or permission to mutate data.

To collect a trace, load a save, open and close an ordinary menu, then return to
the main menu. Existing tutorials should behave normally; the observer creates
no new prompts. Preserve the session log before another launch. This trace can
check whether the mapped boundaries execute, but it does not establish custom
registration, dismissal ownership or reload cleanup.

## HUD notifications

The [notification map](research/notification-map.json) distinguishes the game's
predefined HUD messages from its stock loot toasts. It applies to executable
version 0.564.478.0; the recorded references do not authorize native calls.

The script binding `hud_notification` accepts a predefined string enum. Its
callback resolves the required ECS environment and appends a 12-byte record,
unless a record of that type already exists. Unknown enum values are errors.
It forwards neither arbitrary message text nor a mod identity or duration, and
returns no Lua receipt. This binding therefore cannot directly implement the
text and ownership contract of [mod feedback](mod-feedback.md).

The stock toast component has eight game-owned slots. It consumes translated
message/item fields, icon data, category, positioning and enter/leave state.
Notification HUD visibility, queue emptiness and the hide-loot-notifications
setting gate the container. The message field uses an HTML binding: a bridge
accepting plain mod text must preserve its text-only contract when using it.

The separate native notification queue accepts 184-byte tagged requests. Its
producer destructively moves payload ownership, including native strings and
vectors. The loot synchronizer selects active request kind 5 and payload variant
0, a vector of 208-byte item records. It writes each item's message and label
into the corresponding slot's string facts. The single-string variant 7 is a
different payload and is not consumed by this synchronizer.

The UI localization wrapper passes `locID` to its native binding; it does not
pass a caller-supplied `translation` field. The stock toast displays the bound
model's translation. Its native getter looks up the identifier in localization
tables. When a valid UTF8 identifier is absent, the reviewed fallback converts
it to UTF16, caches that value, and returns it as UTF8. This provides a candidate
for literal messages, but matching existing keys and localization debug modes
can change the result. The rich HTML sink still requires plain-text escaping.

Each distinct missing key allocates a cache node of 456 bytes, plus any
out-of-line string storage. Resource replacement and manager destruction clear
the cache; the reviewed lookup and insertion paths have no practical eviction
limit. A mod adapter must therefore bound distinct text in addition to active
messages and publication rate. Message expiry does not reclaim cached keys,
and clearing the shared cache is not a mod-owned cleanup operation.

The queue applies shared category and per-kind limits. Its atomic ticket
counter advances before payload initialization;
this alone does not establish safe enqueue from an arbitrary worker thread.

Assigning the toast models would overwrite shared presentation state. Reusing
the native queue requires bounded text admission and a localization policy,
establishing payload construction and the normal producer phase, and preserving
mod attribution, cleanup and delivery receipts alongside game messages.
The current feedback implementation instead owns its message queue and renders
a separate CRML component inside the game's existing Cohtml document.

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

## Map and sonar presentation

The HUD navigation display and full map have separate native producers. On the
reviewed October hotfix, `heron::ui_sonar` updates HUD orientation, points of
interest and presentation facts. `heron::ui_map`, `heron::ui_map_detail` and
`heron::ui_map_menu` update map position, player markers, map data and menu state.
Their named registrations are recorded in the
[map presentation reference map](research/map-presentation-map.json).

The native UI document binds the sonar's rotation and individual point transforms
separately. The full map binds scale, translation, background bounds, a marker
collection and the player camera transform. Its pan/zoom transform operates on
map coordinates; it does not establish a world-to-map conversion. District, floor,
gravity orientation, clipping and context lifetime must also be accounted for.

The player-marker chain now identifies the district projector, its rotation and
translation helper, and the subsequent map-rectangle placement. The projector
halves world Y before rotation, applies district offset and scale, normalizes
against transformed bounds, and flips the vertical coordinate. Native markers
clamp the result at the map edge; a route needs segment clipping to avoid drawing
spurious lines along that edge. The internal copied calculation rejects
degenerate bounds rather than presenting a collapsed axis as a usable map.

Sonar uses a separate camera/movement-plane projection and a piecewise distance
scale with runtime near/far thresholds. It also filters marker eligibility by
district and sonar state. Neither a fixed X/Z projection nor the full-map
district transform reproduces this path.

With capability observation enabled, the bounded `map_projection` stream
compares the internal district calculation with ordinary native marker output.
`compared` distinguishes an actual comparison from an unavailable result;
`matched` reports agreement only for that sample. Reports contain no district
pointers or world positions. This observation does not establish map-layer
ownership, floor selection or a persistent coordinate identity.

Schematic drawing keeps normalized viewport coordinates. The separate
`map_read`/`map_publish` interface in development runtime 0.1.0-alpha.4.4.dev.2
accepts bounded world-space segments and labels for the native full map. Its
copied district calculation matched 336 native samples. Native rectangle
placement and layer lifecycle still need gameplay qualification; sonar remains
unsupported. See [drawing](mod-drawing.md) for ownership, freshness and results.
Named engine addresses are research references, not callable guest interfaces.

## AI and navigation

The registered Bonsai behavior-tree update specialized for `heron::GameBehaviorTreeContext` dispatches through `0x2522b00` to `0x2521230`. That implementation collects work into a temporary buffer, uses task helpers `0x3271560` and `0x3271140`, submits through `0x3271690`, and calls additional completion/lifetime helpers before releasing its temporary storage. The reviewed path contains a parallel-for source anchor and an update-behavior-tree work label.

This supports parallel work in that update path; it does not establish that the main thread owns all AI state or that those helpers constitute a public job API. Navigation mesh updates, link/obstacle changes, workspot searches, knowledge, and EQS are separate families in the index. Full worker lambda arguments, cancellation, and interactions with world unload remain open.

## Save and load

`coregame::savegames::processWriteRequests` dispatches through `0x18c3770` to `0x18c0f90`. The latter traverses records at stride `0x5d8`, branches on request state, builds save-chunk storage and invokes serialization-related helpers. `processLoadRequests`, container updates, typed header caching, and gameplay save bindings are separately registered.

This is a concrete request-processing entry, not a recovered save-file schema or proof of atomic/durable disk writes. Save cancellation, completion callbacks, version migration, checksums, and reconstruction of live entity/resource state remain unresolved.

The saved player transform includes a separate `transformLevelBundleId`. On the
reviewed October hotfix, restore consumer `0x227b550` requires a nonzero saved ID,
a present current bundle ID, equality between them, and an open context gate
before calling the transform writers. This establishes a native compatibility
check for that restore path. It does not establish a unique coordinate frame or
campaign identity. The optional diagnostic capture reports the copied entry
conditions separately from native return; it does not claim that a transform was
applied.

The serialized `containerId` and `prefix` strings belong to save selection. The
reviewed header-cache selection path can replace the current values, and is used
by save-menu and New Game Plus requests. The save-request builder also constructs
container names using `slot-{}` and the active slot byte; separate branches choose
save-kind prefixes. A slot-derived name cannot distinguish a new playthrough that
reuses that slot. Neither the selected strings nor a playthrough counter is
currently established as an immutable campaign key. See the
[navigation and save-context map](research/navigation-context-map.json) for the
reviewed layouts, encoded references and remaining semantic limits.

The `persi-global` save chunk serializes `playthroughNum` as a 16-bit value at
offset `0x1ec`. Its capture helper reads the New Game Plus environment counter;
an optional request flag increments the saved value, saturating at `0xffff`.
The apply path restores that value to the environment when the chunk is present.
This is separate from the header's `newGamePlusPlaythrough` and
`isInitialNewGameSave` metadata. A restored progression counter does not identify
a unique campaign, and these static paths do not establish rollback or new-game
lifecycle guarantees.

The current-profile chunk path serializes typed data into named byte buffers,
transfers buffer ownership to a save request, and moves that request into an
asynchronous queue. The returned request ticket is not a write-completion
acknowledgement. The `persi-global` reader looks up a name, parses its bytes into
a fixed typed object, consumes the matched entry, and transfers that object's
ownership to the apply caller. A missing name returns no object; malformed data
is not established to have the same outcome.

These paths are potential save-extension points, not a supported custom-chunk
interface. Unknown-chunk retention across subsequent saves, backend replacement
rules, and recovery after a mod is removed remain unresolved. See
`save_chunk_lifecycle` in the navigation map for the current-profile call chain
and ownership boundaries.

For the inspected Steam backend, ordinary chunk writes become operations on
individual names. The final handler forwards the name, byte count and buffer to
the storage interface. A separate operation removes a named entry. No sweep of
unmentioned names was found in the traced ordinary-write handler, but this does
not establish retention across higher-level container replacement, new-game
flows or cloud recovery. Other distribution backends require separate review.

The removal queue uses a different request kind from ordinary writes. Its
continuation lists names and selects those beginning with the request prefix,
using ASCII case-insensitive comparison, before submitting removal operations.
An empty prefix matches every listed name. The enqueue producer and its
new-game or slot-cleanup triggers are not established; the existence of this
path alone does not imply that it runs on every save.

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
