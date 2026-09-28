---
title: Engine Lua integration
description: CONTROL Resonant engine Lua loading, protected execution, script environments, and the distinction from sandboxed Wasm mods.
---

# Engine Lua integration

CRML's engine Lua integration is experimental. Self-authored Luau 0.650 bytecode has executed in the game's VM, including arithmetic, a contained runtime error, successful execution afterward and stack restoration. A private script environment and a synchronous custom-event registration, delivery and removal sequence have also passed in gameplay. A fixed smoke test is available in developer builds. Loading arbitrary `.lua` mod packages, persistent callback ownership, hot reload and unloading are not supported yet. The existing [Wasm API](api.md) remains the supported mod interface.

## Execution path

The [reference map](research/lua-execution-map.json) records call sites for the executable fingerprint `2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0`. These are executable RVAs; they are internal interfaces, not stable exports.

| Routine | RVA | Recovered calling convention on Windows x64 |
| --- | --- | --- |
| Bytecode loader | `0x2c27b70` | `int(state, chunkLabel, bytes, byteCount, environmentIndex)` |
| Resource load wrapper | `0x3229280` | Boolean result; receives state, bytes, count, label, error destination, environment index |
| Protected Lua call | `0x2c4fc90` | `int(state, argumentCount, resultCount, errorHandlerIndex)` |
| Internal protected callback | `0x2c434f0` | `int(state, callback, userData, savedTopOffset, errorHandlerOffset)` |
| Stack top setter | `0x2c4d040` | `void(state, index)`; positive indices are relative to the current frame base |
| Tagged light userdata push | `0x2c4ec50` | `void(state, value, tag)`; entity arguments use tag 1 |

The loader receives bytecode **without** the resource envelope. An environment index of zero selects the VM's globals table at state `+0x58`; a nonzero index follows the VM's index-resolution path. A successful load pushes a closure; it does not execute the script. The resource wrapper extracts and removes a load error on failure. Its separate error destination is an engine object and must not be replaced with an arbitrary C string buffer.

The protected callback saves call-frame position and native-call depth. Its error path closes pending upvalues, places an error object at the saved stack position, and restores the frame base. Stack positions are offsets because VM allocation can relocate the stack. The callback receives `(state, userData)`. This barrier covers loading as well as execution in the CRML probe; protecting only the later Lua call would leave loader allocation outside that barrier.

The engine's fixed script update makes a protected call at `0x1a0aada` with one argument, zero results and error handler index 1. The probe uses the return from that exact call after success. It shares the existing boundary-guard hook, runs synchronously on the engine thread, and does not retain a VM, world or entity pointer afterward. A suspended VM, non-root call frame, missing context, insufficient stack space or installed protected-error debugger callback prevents execution.

The arithmetic/error/recovery test confirms this loading and protected-execution path for the embedded compiler output. Other ABI interpretations still come from instruction and data-flow inspection. Encoded-reference checks and native mock tests do not establish behavior of additional engine operations.

## Script environments and callback ownership

Per-entity initialization at `0x19c4310` creates a script table and a metatable whose `__index` points to the VM globals. It makes that metatable read-only, attaches it to the script table, stores `_ENV` as a reference to the script table itself, and sets `self` to tagged entity data. It retains the table in the VM registry and installs it as the loaded closure's environment before executing the chunk. The complete lifetime of this entity-owned environment is not yet exposed as a CRML API.

The active-script resolver at `0x1a0a390` walks function environments looking for `self`, decodes it with entity tag 1, and checks the index and generation against the current world. A global binding being present is therefore insufficient: callbacks also depend on a valid script owner and the required ECS environment.

| Binding | Recovered behavior |
| --- | --- |
| `nl_update_callback()` | Gets the active entity's existing callback, or nil |
| `nl_update_callback(function)` | Retains and installs a callback; a previous reference is queued for release |
| `nl_update_callback(nil)` | Removes the entity's update registration; release of the old reference is queued |
| `nl_add_event_handler(entity, name, function)` | Retains the function, associates target and script owner, and returns a tag-4 handler handle |
| `nl_send_custom_event(name, ...)` | Adds the `lua.` prefix, selects listeners for the active entity, and calls them synchronously with the sender followed by the supplied arguments |
| `nl_remove_event_handler(handle)` | Releases the callback reference, sets it to zero, and queues removal of the event-list entry |

Custom-event dispatch skips entries whose callback reference is zero. Removal can therefore stop delivery before deferred list cleanup completes. A failing callback follows a separate release/removal path. The event smoke test has confirmed registration, synchronous delivery and explicit removal in gameplay. The failure and teardown paths remain static findings; the successful test does not establish persistent listener cleanup during entity destruction or world replacement.

`nl_update_callback` has one slot per entity. Replacing that slot can displace game behavior; the event probe only uses its getter. CRML does not claim ownership of an existing game's update callback.

### Registry references and entity teardown

The following contracts come from static instruction and data-flow inspection for the mapped executable. They describe internal engine behavior, not callable CRML APIs.

| Routine | RVA | Recovered behavior |
| --- | --- | --- |
| Retain a stack value | `0x2c507a0` | `int(state, index)`; copies the value into the registry, returns a positive reference for a non-nil value, and does not pop the source |
| Fetch an integer table entry | `0x2c4f1d0` | `int(state, tableIndex, key)`; pushes the value and returns its type tag |
| Release a reference | `0x2c508d0` | `void(state, reference)`; positive references return to the registry free list; nonpositive values are ignored |
| Script-removal dispatcher | `0x19bd610` | Calls the removal worker at `0x19b4270` |
| Entity script cleanup | `0x19c70e0` | Releases entity-keyed references, invokes callback cleanup, clears external wrapper entries and releases the retained environment |
| Entity callback cleanup | `0x1a02180` | Invalidates queued references and processes four event containers for the departing entity |
| Event-owner cleanup | `0x1a09580` | Walks an owner's listeners, releases their callback references, unlinks records and returns their slots for reuse |

The pseudo-index resolver at `0x2c50cd0` maps **-10000** to the registry, **-10001** to the current function environment (or globals at the root frame), and **-10002** to VM globals. The registry value is at global-state `+0xc98`; the reference free-list head is at `+0xcb0`. Release writes a numeric free-list link into the slot. A released reference therefore need not read back as nil, and its number can subsequently identify another value. Neither reference creation nor release provides an entity generation check.

Event registration at `0x1a07910` maintains separate target and script-owner indices. Each 32-byte listener record holds the callback reference at `+0x04`, target-chain link at `+0x08`, owner-chain link at `+0x0c`, target entity at `+0x10` and script owner at `+0x18`. The Lua binding obtains the owner through the active-script resolver; it is separate from the supplied target. Owner cleanup traverses the owner index at event-container `+0x30`, releases each callback, unlinks its target-chain entry, invalidates the record and recycles its slot. Retaining a callback in the registry alone does not add it to this ownership structure.

The script-removal worker calls entity cleanup at `0x19b43d9` and `0x19b4571` on separate branches. Within entity cleanup, references in three entity-keyed maps at script-state `+0x78`, `+0xa8` and `+0xd8` are released and their entries erased. Callback cleanup follows at `0x19c730e`. The routine then clears entries in `external_script_wrappers` and releases the entity's environment reference from the map at VM-context `+0x18`.

This establishes a static cleanup path from script removal to event references and environment ownership. It does not yet establish when that path runs relative to save reload, VM destruction or CRML shutdown. A persistent integration must distinguish its own retained references from engine-owned listener references, retire each reference once, and invalidate ownership before VM replacement. Pointer equality and an old reference number are insufficient proof that a callback is still valid.

## Developer smoke test

The self-authored scripts live in `examples/lua-probe/`:

1. `arithmetic.luau` uses a small local table and returns **42**.
2. `error.luau` deliberately calls a nil value. The expected protected-call status is **2**.
3. The arithmetic script runs again and must return **42**.
4. `bindings.luau` returns a bit mask identifying selected function names in the default environment. It does not invoke those functions.
5. `events.luau` runs in a private environment supplied by the native harness with a valid owner, registers a uniquely named custom-event listener, sends a payload, removes its own listener and sends again. It returns **127** only if delivery, removal, private namespace behavior and preservation of the existing update callback all pass.

The native harness restores the stack after every step and compares the existing stack values and frame identity. Any failed step stops the sequence. It copies the current update's tagged entity argument before the original call and rechecks its world and generation afterward. The script uses that existing owner in its private environment; it creates no entity and never replaces an update callback.

For the event step, the harness builds a stack-rooted table with `_ENV` pointing to itself and `self` set to the checked owner. A read-only metatable inherits the engine globals through `__index`. The loader receives the table's positive stack index. This does not require guest `getfenv`, `setfenv`, `setmetatable`, `rawget` or `assert` globals, and does not add those functions to the engine. The namespace is not a security sandbox: inherited engine functions still require trusted code and valid calling context.

The harness checks that the event script's marker is present only in its private table and absent from shared globals. Environment creation, loading and marker checks run inside the native error barrier. Before executing, it reserves four stack slots and snapshot entries for the environment, results and field checks; frames too large for the bounded snapshot are rejected.

A schema-4 gameplay capture passed all five stages: arithmetic **42**, protected error status **2**, recovery **42**, binding mask **15**, and event result **127**. Every stage restored its stack; the final stage reported both environment creation and namespace verification. This confirms the bounded, same-call event sequence. It does not demonstrate callbacks surviving across updates or cleanup during reload and unload.

The event script attempts to remove its own listener even if sending raises an error. It writes only to its private environment and local state. The harness keeps no registry references and runs at most once per process. The engine retains a function while the listener is registered; removal releases that reference. Temporary closures then become eligible for ordinary VM garbage collection. This bounded register/send/remove operation is not general mod unload support.

Build with the explicit opt-in switch:

```powershell
.\build.bat -LuaProbe -Test
```

Install that build using the normal [movement mode](movement.md) installer option. The probe depends on that mode's fingerprint-gated script hook. Launch the game, load a playable save, move normally for about ten seconds, then exit. No hotkey is needed and no visible gameplay change is expected. Reloading a save in the same process should remain normal; the probe does not run a second time in that process.

The `lua_probe` records in `crml/fall-recovery.jsonl` contain:

| Field | Meaning |
| --- | --- |
| `enabled` | This build armed the probe after its entry and call-site checks |
| `state` | 0 waiting for an eligible call; 1 executing; 2 finished |
| `rejected` | Calls from the selected site whose VM snapshot was unsuitable |
| `schema` | 4 supplies the environment natively and reports its checks; 3 added bounded error details; 2 added the event stage |
| `passed` | All stages produced their expected results and restored the stack |
| `steps` | Load, call and outer protection statuses, numeric result and restoration result |
| `thread` | The engine thread that executed the test |
| `steps[4].environment_created` | The native harness created and populated the event script's private environment |
| `steps[4].environment_verified` | The script's marker was 42 in the private table and absent from shared globals |

Each step also reports `error_kind` and `error_line`. The category is one of `none`, `nil_call`, `no_active_script`, `missing_ecs`, `invalid_handler`, `assertion`, `other` or `unavailable`. A line number is extracted only when the error begins with that step's own chunk label; zero means no matching source location. The runtime reads at most 512 bytes of a bounded string error before stack cleanup and emits neither the original message nor paths or arbitrary engine text. A caught event error can supply these details even when the outer protected call succeeds.

The event script returns **127** for success. Negative values identify failures; a successful VM call returning a diagnostic code is still a failed probe:

| Value | Meaning |
| --- | --- |
| -106 through -110 | A required function is absent: `pcall`, `nl_update_callback`, `nl_add_event_handler`, `nl_send_custom_event`, `nl_remove_event_handler`, respectively |
| -201 | Initial event send raised an error; listener removal was attempted |
| -202 | Listener removal raised an error; cleanup is not confirmed |
| -203 / -204 | No delivery / more than one delivery |
| -205 | Sender or payload differed from the expected value |
| -206 / -207 | Send after removal raised an error / invoked the removed listener |
| -208 / -209 | Existing update callback / private environment marker changed |
| -210 | Private environment checks failed |
| -211 | The update getter returned a value other than a function or nil |

These codes distinguish library availability, context errors, delivery and cleanup. They do not change the engine's dispatch or ownership behavior.

Older schema-3 captures can also contain -101 through -105 for missing `getfenv`, `setmetatable`, `setfenv`, `rawget` or `assert`. Schema 4 does not depend on those guest helpers. A failure before loading can leave load/call statuses at -1; inspect the protection status and environment fields instead of interpreting an unexecuted stage as success.

The binding mask uses bits 1, 2, 4, 8, 16, 32 and 64 for `nl_update_callback`, `nl_add_event_handler`, `nl_send_custom_event`, `nl_copy_world_transform`, `spawn_bundle_with_instigator`, `npc_spawn` and `npc_despawn`, respectively. The execution test returned **15**, confirming the first four names in the default environment. A set bit proves only function presence. A clear bit does not establish that the operation is unavailable in entity-specific environments or under another namespace. Argument schemas, context requirements and behavior remain separate questions.

Normal builds omit `-LuaProbe` and do not arm this test. The compiler is not a runtime dependency. The embedded bytecode can be reproduced from the checked-in sources using a separately built [Luau 0.650 compiler](https://github.com/luau-lang/luau/tree/0.650):

```powershell
$compiler = Read-Host 'Path to Luau 0.650 luau-compile executable'
python tools/compile_lua_probe.py --compiler "$compiler" --check
```

Omit `--check` to regenerate `runtime/lua_probe_bytecode.h`. The header contains only CRML-authored scripts, compiled at optimization level 0 with debug level 2. Matching a bytecode header alone does not establish compatibility with the engine VM.

The native CTest suite covers stack cleanup, owner generation checks and hook routing. `python tests/test_lua_probe_source.py --vm <luau-executable>` additionally exercises the event script against a standalone mock host, including dispatch failure and cleanup. Those mocks do not substitute for the engine's event dispatch and deferred removal behavior.

## Lua and Wasm mod interfaces

Lua can provide convenient access to engine bindings for properties, events and entity operations. Its eventual scope depends on the recovered binding contracts and lifecycle rules, not on the size or complexity of a script. Complex weapon systems may need Lua callbacks, Wasm logic, native bridge operations, resource integration or a combination of these.

Direct execution inside the engine VM must be treated as trusted scripting until isolation, exposed libraries and execution limits have been established. It does not inherit Wasm capabilities or fuel limits. [Luau's embedding guidance](https://luau.org/sandbox/) also distinguishes language-level safety from host API access and isolation between scripts; it assumes compiler-produced bytecode. CRML's offline bytecode parser is an inspection tool, not a security verifier for executable mod input.

See [reviewed engine paths](engine-paths.md) for callback and event dispatch, and [Lua bytecode tools](binlua.md) for offline inspection and reconstruction.
