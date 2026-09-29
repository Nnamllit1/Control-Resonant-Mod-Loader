---
title: Engine Lua integration
description: CONTROL Resonant engine Lua loading, protected execution, script environments, and the distinction from sandboxed Wasm mods.
---

# Engine Lua integration

CRML's engine Lua integration is experimental. Source-enabled development builds can compile and load trusted controllers from `crml/lua-mods/`. This mode is not included in player releases. The existing [Wasm API](api.md) remains the supported release interface.

Self-authored Luau 0.650 bytecode has executed in the game's VM, including arithmetic, a contained runtime error, successful execution afterward and stack restoration. Private script environments, persistent custom-event delivery, explicit listener removal and engine-owned listener cleanup have passed in gameplay for the embedded examples. File-based source loading uses the same native executor. The counter example supports live source replacement, recovery from a callback error, explicit unload and re-enable, and controller recreation across a save reload.

## Source controllers in development builds

Engine Lua is **trusted code, not a sandbox**. It inherits the game's scripting bindings and can change engine state. Enable only source packages you trust. Wasm modules retain their separate capability checks and execution budgets.

Build and install the source-enabled runtime with the game closed:

```powershell
.\build.bat -LuaSource -Test
$game = Read-Host 'Game installation folder'
python tools/install.py "$game" --movement-wasm --lua-source --apply
```

Add `--update` when CRML is already installed. The source option currently uses movement mode's authenticated engine hook; it does not install a movement mod. Do not combine it with the observer or physics modes. The installer records ownership of `crml/engine-lua.enabled` and includes the compiler license. It never installs source packages implicitly.

A source package has this layout:

```text
crml/
  engine-lua.enabled
  lua-mods/
    my-mod/
      main.luau
```

Package identifiers contain lowercase ASCII letters, digits, underscores or hyphens, with at most 64 characters. Copy `examples/lua-counter/main.luau` into `crml/lua-mods/lua-counter/main.luau` for a minimal controller. It updates private state without changing visible gameplay. The watcher checks source files approximately every 500 ms; compilation runs on the worker thread and execution runs only on eligible engine updates.

The entry point returns a function:

```lua
local stopped = false
return function(shutdown)
    if shutdown then
        stopped = true
        -- Release resources owned by this controller.
        return
    end
    if stopped then return end
    -- Update; acquire resources here after the host has retained the controller.
end
```

Construction must not acquire engine resources. Ordinary updates receive no arguments and need no return value; shutdown receives one truthy argument. `_ENV` is private to the controller and `self` is its selected, existing script owner, **not necessarily the player**. The host requests updates at intervals of at least 16 ms, subject to that owner's engine updates. An owner or world transition can retire the controller and allow a new owner; shutdown is not called on an owner the engine has already destroyed. Scripts must distinguish engine-owned resource cleanup from restoration of their own gameplay changes. CRML does not automatically undo arbitrary script side effects.

A controller recreated after a save reload starts with fresh local state and a new private environment, even when its source has not changed. Source controllers do not provide save persistence. Do not assume that every save reload destroys the selected owner or closes the Lua VM.

Editing `main.luau` queues a replacement after successful compilation. The old controller shuts down before its replacement is constructed. A compile error preserves the old controller. Adding an empty `disabled` file beside the entry point, or removing the entry point, queues unload. Removing the global `engine-lua.enabled` marker queues unload of all source packages; restoring it allows discovery again within a session that started with source loading enabled. Enabling that mode for the first time requires restarting the game. An unreadable marker preserves the previous state. Installer-managed markers must be restored unchanged before a later installer update checks their receipt, or removed through `--update --disable-lua-source` while the game is closed.

A callback error stops updates for that source revision and schedules cleanup. Correcting the file can start a new revision after successful cleanup; the failing revision is not retried automatically. Aggregate `failures` is cumulative for the process, so a recovered package can be `active` with `status: 0` while that count still includes an earlier error.

`crml/lua-mods.jsonl` contains aggregate `lua_source` records, per-package `lua_package` records and compile/discovery events. `active_revision` and `desired_revision` distinguish the current controller from a queued edit. `calls` counts successful updates in the current controller; `status` and `line` retain the last native or script error until a new initialization. `released` counts explicit registry releases and `reclaimed` counts roots retired by observed VM closure. The file is bounded to 4 MiB per process; reaching the limit stops logging, not source execution. Ending a movement diagnostic capture does not stop source mods.

Normal gameplay is unchanged by the counter example. Its package should become `active`, `calls` should increase, and the aggregate failure count should remain zero. Disabling it should increase `released` and remove its active reference after an eligible owner update. These counters describe lifecycle operations; they do not establish that an arbitrary script restored every property or released every engine-owned resource.

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

The engine's fixed script update makes a protected call at `0x1a0aada` with one argument, zero results and error handler index 1. The one-shot probe uses the return from that exact call after success. It shares the existing boundary-guard hook, runs synchronously on the engine thread, and does not retain a VM, world or entity pointer afterward. The separate persistent diagnostic below retains a registry reference with explicit lifecycle ownership. A suspended VM, non-root call frame, missing context, insufficient stack space or installed protected-error debugger callback prevents execution.

The arithmetic/error/recovery test confirms this loading and protected-execution path for the embedded compiler output. Other ABI interpretations still come from instruction and data-flow inspection. Encoded-reference checks and native mock tests do not establish behavior of additional engine operations.

### Native execution layer

`runtime/lua_executor.h` defines the internal synchronous executor in `crml::engine::lua`. The persistent diagnostic uses this executor; script selection, expected counters and deliberate-error recognition remain in `lua_probe.cpp` and `lua_session.cpp`. `lua_vm.h` and `lua_vm.cpp` hold the shared engine ABI, frame checks, owner-generation check and bounded error classification.

The executor accepts a borrowed bytecode span and chunk label, an authenticated engine context, an action, and operation-liveness callbacks supplied by its lifecycle host. It creates a private environment and retains the constructor's returned closure. Ordinary invocation requests no return values. Shutdown calls that closure with one truthy argument, then releases the host's registry reference; raw release skips the Lua callback. Numeric returns and rooted constructor arguments are optional facilities used by the diagnostic, not requirements for ordinary controllers.

The caller owns scheduling, thread affinity, serialization with engine teardown and reference retirement. A successful VM call is insufficient on its own: the caller must also check stack restoration and whether the operation was invalidated. An initialization result received after owner retirement or VM closure must not become an active controller. After an interrupted shutdown, release can be deferred to a fresh eligible frame; once native release has been attempted, an ambiguous outcome must never be retried against a potentially recycled reference. Construction must defer engine resource acquisition until the retained controller's first invocation.

This executor performs no file discovery, compilation or mod installation and is not a public Lua package API. Source compilation is available through the [offline tool](binlua.md#compile-self-authored-source) and the native source preparation layer below. Header and size checks reject unsuitable input but do not make bytecode trustworthy. The executor requires trusted compiler output and inherits the engine's permissions. Native tests exercise caller-selected programs, callbacks without numeric returns, errors and synchronous teardown; the mock loader does not prove arbitrary bytecode compatibility with the game.

### Internal controller lifecycle

`runtime/lua_controller.h` supplies a native host for already-compiled, trusted controllers. It owns up to 16 controllers with a combined 64 MiB of active and queued bytecode, at most 16 MiB per program. Submission transfers the program into host-owned storage and queues initialization; it makes no VM calls. The host requires eligible engine-thread contexts from the fingerprinted update hook. The source service connects discovery to this host only in explicitly enabled development builds.

Each controller stays with its selected owner and VM. Its configured interval limits invocation frequency; the owner must also supply an eligible update. A replacement keeps the old source revision alive through shutdown and reference release before constructing the new revision. An initialization or callback error stops automatic retries of that revision. Failed shutdown blocks replacement until engine owner cleanup, since releasing the host's reference alone does not remove resources retained by the engine. Owner retirement uses raw reference release instead of calling a stale shutdown closure, and VM closure retires references without touching the destroyed VM.

`runtime/lua_dispatch.h` provides the common gate, operation invalidation and teardown notifications used by both the controller host and the diagnostic session. Native stack-restoration failure, an unknown teardown identity or an unexpected C++ exception disables further CRML Lua execution for the process. Normal contained Lua errors do not disable unrelated controllers. Native exceptions propagate after gate cleanup. These boundaries cannot make arbitrary engine bindings transactional or resolve cross-thread engine task dependencies.

Shutdown must drain through eligible updates or VM closure before the host is destroyed. Its destructor disconnects notifications and never attempts Lua execution on the worker thread. Tests exercise the host's replacement and retirement rules, shared-gate concurrency, and the host-to-executor path against a mock engine ABI.

### Native source preparation

Development builds using `-LuaSource` link the pinned Luau 0.650 compiler and AST utilities. Compilation uses optimization level 0 and debug level 2, matching the embedded examples. The build verifies the source archive and includes its MIT license. This path does not require a Python installation or external compiler executable, and it does not link a second Lua VM. Native tests compare all seven embedded examples byte-for-byte against the linked compiler's output.

`runtime/lua_packages.h` provides worker-thread discovery and compilation for an explicitly supplied directory. Each package uses a lowercase ASCII identifier and a `main.luau` entry point. A `disabled` marker queues its unload. Sources must be UTF-8, with an optional BOM, and at most 1 MiB each. The scanner accepts up to 16 package folders and bounds directory enumeration. It rejects reparse-point package paths and never loads precompiled files as executable input. These checks are not a filesystem or scripting sandbox: engine Lua packages remain trusted code.

Polling compiles changed source snapshots and queues the resulting controllers. An unchanged syntax error is not repeatedly compiled. Invalid edits and uncertain reads preserve the previous accepted revision; source removal and explicit disabling queue unload. A second read rejects an edit that changed during compilation. Compile diagnostics contain a fixed category and source line, without copying source text or filesystem paths into events. Controller replacement still waits for the native lifecycle host to drain the old revision.

`runtime/lua_source.cpp` owns the process-pinned host, worker polling and bounded logs. Startup requires the source opt-in, the supported executable and successful VM/lifetime-hook setup. Source mode bypasses the embedded diagnostic sequence. The worker never saves a VM pointer or executes Lua. Normal release packaging rejects `-LuaSource` builds. Native tests exercise compilation, file changes, hook routing, opt-out and lifecycle operations against a mock engine provider, plus startup refusal in an unsupported process. Gameplay coverage for the file-based `lua-counter` includes initialization, updates, live replacement, preservation after a compile error, callback-error cleanup, corrected-source recovery, unload through the package's `disabled` marker, and reference release followed by reinitialization across a save reload. This resource-free example does not establish cleanup of arbitrary engine resources or reference reclamation during VM closure.

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

This establishes a static cleanup path from script removal to event references and environment ownership. A gameplay capture observed selected-owner cleanup followed by new owner updates without a VM-close call during the recording window. Cleanup ran on threads distinct from the sampled update thread. Reload handling therefore cannot rely solely on VM destruction or assume that teardown runs on the callback thread. This does not establish every transition or process-exit path. A persistent integration must distinguish its own retained references from engine-owned listener references, retire each reference once, and invalidate ownership before VM replacement. Pointer equality and an old reference number are insufficient proof that a callback is still valid.

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

The event script attempts to remove its own listener even if sending raises an error. It writes only to its private environment and local state. This one-shot sequence keeps no registry references and runs at most once per process. The engine retains a function while the listener is registered; removal releases that reference. Temporary closures then become eligible for ordinary VM garbage collection. This bounded register/send/remove operation is not general mod unload support.

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

Normal builds omit `-LuaProbe` and do not arm this test. The embedded probe does not require a compiler at runtime; `-LuaSource` builds separately include the compiler described above. The embedded bytecode can be reproduced from the checked-in sources using a separately built [Luau 0.650 compiler](https://github.com/luau-lang/luau/tree/0.650):

```powershell
$compiler = Read-Host 'Path to Luau 0.650 luau-compile executable'
python tools/compile_lua_probe.py --compiler "$compiler" --check
```

Omit `--check` to regenerate `runtime/lua_probe_bytecode.h`. The header contains only CRML-authored scripts, compiled at optimization level 0 with debug level 2. Matching a bytecode header alone does not establish compatibility with the engine VM.

The native CTest suite covers stack cleanup, owner generation checks and hook routing. `python tests/test_lua_probe_source.py --vm <luau-executable>` additionally exercises the event script against a standalone mock host, including dispatch failure and cleanup. Those mocks do not substitute for the engine's event dispatch and deferred removal behavior.

## Script and VM lifetime diagnostics

The recovered script-state destructor at RVA `0x19b0190` closes its VM before releasing its context. The close entry at `0x2c3be80` obtains the main thread from the shared global state, closes its upvalues and enters state destruction at `0x2c3c1a0`. That path releases GC objects, string storage, call frames, the stack and finally the VM allocation. Registry references cannot be used after this boundary. These are static contracts for the fingerprint in [the execution map](research/lua-execution-map.json); they do not establish which save or menu transitions destroy a VM.

With `-LuaProbe`, CRML also observes the close entry and entity-script cleanup at `0x19c70e0`. Both hooks forward their original arguments unchanged. The observer invokes no Lua functions, retains no registry references and changes no engine state. These hooks also notify the persistent diagnostic described below; the one-shot script probe still runs as described above.

The observer tracks up to 16 shared Lua global states and one selected script owner per observed world. It emits anonymous, process-local epochs for the VM, world and owner; they are diagnostic identities, not API handles. Closing a VM retires its identity before forwarding to the engine. Matching owner cleanup retires the selected owner; a subsequent eligible update can select another. World address changes also retire the selected owner. Post-call records use copied metadata and never read freed state. Reuse of a world address without an observed teardown cannot independently prove world continuity.

Records are written to `crml/fall-recovery.jsonl`:

| Record or field | Meaning |
| --- | --- |
| `lua_lifetime` | Observer state and cumulative counters |
| `enabled` | Both fingerprint-gated lifetime hooks were installed and recording is enabled |
| `incomplete`, `missed`, `read_failures` | Loss, contention, capacity exhaustion or unreadable state; any such loss stops further epoch assignment |
| `cleanup_calls`, `close_calls` | Calls observed while recording, including owners or VMs outside the selected sample |
| `lua_lifetime_event` | Ordered event with sequence, timestamp, thread ID and anonymous epochs |
| `vm_observed`, `world_observed`, `owner_observed` | First observation of an identity; not a creation hook |
| `world_changed` | A tracked VM supplied a different world address |
| `owner_cleanup_begin`, `owner_cleanup_end` | The selected owner's script cleanup was entered and returned |
| `vm_close_begin`, `vm_close_end` | A tracked VM's destruction was entered and returned |
| `update`, `updates` | Sparse samples and cumulative eligible script-call count; multiple calls can occur within a frame |
| `observer_stop` | Recording stopped; this does not mean the VM was destroyed |

Each batch holds at most 256 events. The worker drains the batch before writing, without file I/O on engine threads. The enclosing movement recorder limits capture to approximately ten minutes. Hook trampolines remain pinned and forward normally after recording stops. Process termination may end recording before a final close event is flushed; a missing exit event is not proof that destruction did not run. Lifetime observations alone do not establish reference cleanup; combine them with the session and listener-reference records below.

To capture reload ordering:

1. Install a diagnostic build made with `-LuaProbe` in movement mode and launch normally.
2. Load a playable save and wait for ten seconds; movement is unnecessary.
3. Reload the same save and wait ten seconds after gameplay returns.
4. Return to the main menu, wait ten seconds, then load the save again and wait another ten seconds.
5. Exit the game and retain `crml/fall-recovery.jsonl` before launching again.

No diagnostic hotkey is required. Movement, saving, loading and menu transitions should behave normally; no visible effect is expected. Interpret a capture only when lifetime recording was enabled and `incomplete` remained false. Compare observed cleanup and close events with subsequent update epochs; a save reload is not assumed to destroy the entire VM. Native tests cover forwarding, identity reuse, freed-state handling, bounded recording and passive stopped hooks. Engine transition ordering still requires a gameplay capture.

## Persistent callback diagnostic

The arithmetic examples, `examples/lua-probe/persistent.luau` and `persistent_error.luau`, return closures that increment counters in private environments. The host retains a closure in the VM registry and invokes it on later eligible updates, at least 250 ms apart. A gameplay capture confirmed persistent state, explicit release, recovery after a deliberate callback error, and two owner-retirement/reinitialization transitions with no unexpected failures. These results cover the bounded self-authored callbacks, not arbitrary Lua mods or every teardown path.

The event example, `examples/lua-probe/persistent_events.luau`, constructs a controller without creating engine resources. After the host retains the controller, its first invocation registers one privately named custom-event listener that counts and checks deliveries. Later updates check that each payload is delivered exactly once while the listener is active. It never replaces the game's update callback or changes gameplay properties.

Gameplay evidence confirms persistent delivery, explicit removal, recovery from a controller error, and removal of the listener by owner cleanup. The listener ledger balanced four retained references with four releases: two explicit removals and two owner removals, with no lost observations or unexpected failures. A fresh controller and listener initialized after the first owner retirement. The final controller root remained retired pending another eligible update; this capture does not prove final reclamation of that root or destruction of its VM. Close calls for unrelated VMs are not evidence of the selected VM's destruction.

After the one-shot sequence succeeds, the diagnostic runs these stages:

1. Initialize a listener and controller, obtain results 1, 2 and 3, then request script shutdown and release the controller reference. Shutdown removes the listener and sends again to verify that delivery has stopped.
2. Initialize another pair, obtain 1 and 2, contain the controller's deliberate nil-call error on the third invocation, then run shutdown and release the controller. The expected error must match the self-authored chunk and generated source-line marker; an unrelated error status is a failure.
3. Initialize a listener that deliberately fails during the third delivery. Before raising the error it discards its local handle; the engine's dispatch-error path owns removal. The controller checks that a fourth event on a later update is not delivered, then shuts down without reusing the retired handle. This stage requires both the behavioral check and an observed engine dispatch-error release to establish cleanup.
4. Retain a controller, then deliberately fail its first initialization immediately after listener registration. Protected cleanup removes the listener and checks that another event is not delivered. A later update retries initialization with a new listener; two deliveries must succeed before shutdown removes it. Rollback and recovery are counted separately.
5. Initialize a fresh counter and keep invoking it until owner cleanup, world replacement, VM destruction or recording shutdown. After owner retirement and reference cleanup, a later eligible owner can start a fresh counter. At most eight sessions are initialized per process.

Construction and resource acquisition are separate so a failed native retain has no listener to abandon. Explicit removal clears the local handle before calling the engine; an ambiguous removal error prevents retries with a potentially recycled handle. Shutdown before the first invocation creates no listener, and a stopped controller cannot register again. These rules cover the example's own listener. They do not roll back arbitrary property writes, spawned entities or effects created before an engine binding returns a usable handle.

All allocating operations, invocation and registry access run inside the recovered native error barrier. The original stack and frame are checked and restored after each operation. A reference is fetched only from its recorded global state. A failed or ambiguous release is never retried, because the engine can recycle the reference number.

An ownership gate prevents another thread's observed script cleanup or VM destruction from overlapping a bounded diagnostic operation. Other-thread teardown waits for that operation, retires matching ownership and marks cleanup in progress, then enters the original engine routine without holding the gate. Updates skip while teardown is active. A revision copied before the original script call rejects a stale update even if cleanup completed before its return and the entity generation still matches.

Cleanup that reenters on the callback thread reuses the gate already held by that thread. Matching owner cleanup invalidates the current operation; nested updates cannot start another callback. The provider stops further script operations, restores the stack only while the VM remains live, and defers release of its separate controller root to a fresh eligible frame. An interrupted shutdown is not invoked twice. VM closure invalidates the operation before destruction: returned references are not published afterward, and CRML performs no subsequent stack restoration or registry access on that VM. An unreadable teardown identity halts the session. Reference release is marked as attempted before entering the native unref routine, so an ambiguous failure cannot cause a second release of a recycled slot.

The selected script owner remains fixed between diagnostic stages, including the interval after one controller is released and before the next is constructed. Unrelated script updates cannot select a new owner during that interval. Observed owner cleanup, world replacement or VM closure permits a new selection; cleanup is tracked even when no controller reference is held. This preserves owner context without assuming every script updates continuously.

Teardown notifications perform no Lua calls. Owner retirement queues release of CRML's controller reference for a subsequent eligible update in the same VM; the old callback is not invoked again. The engine owns the listener reference and performs its removal through its own cleanup routine. CRML does not call script shutdown with a retired listener handle. An explicit unload waits for the still-live owner; owner cleanup supersedes a pending explicit unload. If the VM closes first, the diagnostic discards its reference number before destruction and leaves reclamation to the VM. On recording shutdown, new initialization and invocation stop; pending cleanup can still drain through the pinned update hook. If no eligible update follows, reclamation waits for engine cleanup or VM destruction. The host retains identity values for comparison, but never saves a VM pointer to dereference from a worker thread.

Native tests exercise same-thread cleanup during construction, invocation, shutdown and reference release, including closed or reused VM memory and unrelated-owner notifications. These tests cover CRML's handling of notifications; they do not establish that the engine can safely close a VM from inside its own interpreter stack. The diagnostic uses the shared dispatch gate described above. The embedded gameplay captures do not establish arbitrary Lua file loading, comprehensive hot reload, rollback of arbitrary engine side effects or sandboxing. A script that blocks or runs indefinitely is outside these guarantees.

`lua_session` records accompany the lifetime records in `crml/fall-recovery.jsonl`:

| Field | Meaning |
| --- | --- |
| `armed` | Cleanup hooks and registry entry checks allowed this diagnostic to start |
| `schema`, `event_mode` | Schema 4 adds initialization rollback and retry; schema 3 added listener-error checks, schema 2 persistent events, and schema 1 the arithmetic sequence |
| `sessions`, `initialized` | Attempted and successful session initializations |
| `invocations`, `current_calls` | Total callback invocations and calls in the current session |
| `explicit_unloads`, `expected_errors` | Completed first-stage unload requests and contained second-stage errors |
| `listener_error_checks` | Completed third-stage delivery checks; confirm engine removal separately with `lua_listener_refs.error_removals` |
| `initialization_rollbacks`, `initialization_recoveries` | Fourth-stage cleanup after the deliberate initialization error, and successful delivery after retry; confirm removals with the listener ledger |
| `active_reference`, `retired` | A reference remains owned, and whether invocation has been retired pending release |
| `released`, `vm_reclaimed` | References explicitly released, or handed to VM destruction without another Lua call |
| `owner_retirements`, `world_retirements` | Ownership retirement triggered by script cleanup or changed world identity |
| `failures`, `halted` | Unexpected results or unsafe state; a halted session performs no further registry access |
| `rejected` | Unsuitable execution frames; no operation was attempted |
| `eligible_updates`, `owner_waits`, `vm_waits`, `revision_waits`, `teardown_waits` | Coordinator entries and skipped entries for a different owner, a different VM, a stale cleanup revision, or active teardown; skips do not establish a script failure or a completed check |
| `interrupted_calls` | Returned operations invalidated by matching owner cleanup, VM closure or an unreadable teardown identity; their callback result is not accepted as a successful invocation |
| `reentrant_cleanups`, `reentrant_closes` | Teardown notifications received on the thread currently executing a CRML operation, including unrelated identities that do not invalidate it |
| `last_status` | Last VM/host status, including protected error 2 or diagnostic errors -301 through -306 |
| `failure_status`, `failure_line`, `failure_value` | First unexpected operation result, retained across later cleanup; no arbitrary error text is logged |
| `callback_thread`, `cleanup_thread` | Last execution and matching owner-retirement threads |
| `teardown_depth`, `stop_requested` | Active cleanup nesting and a request to cease execution |

Errors -301 through -307 mean: registry value is not a closure, callback result is not numeric, shared counter already exists, private metatable installation failed, initialization returned no closure, retaining the closure returned no positive reference, or shutdown returned no numeric result. Script result -401 means delivery continued after removal; -402 means delivery count, sender or payload differed from expectations; -403 means the deliberate listener-error branch did not execute; -404 means unexpected initialization failure; -405 means removal failed or was ambiguous; -406 rejects invocation after shutdown or failed cleanup. Result -410 is an expected rollback marker only on the fourth stage's first invocation; elsewhere it is a failure.

Error -308 records an unexpected C++ exception from the native provider. The coordinator releases its gate, halts further operations and rethrows the original exception; it does not turn that exception into a successful Lua error result.

An additional read-only observer identifies listener references created at the engine's event-registration call site by the exact self-authored chunk label. It watches registry release without invoking Lua, changing arguments, or retaining Lua values. The ledger has 16 slots and logs counters rather than addresses or reference numbers. A release is classified only after the original function returns; an interrupted release remains pending. Reused reference numbers and closed VM identities are handled separately.

`lua_listener_refs` reports `retained`, `released`, `active`, `explicit_removals`, `owner_removals`, `error_removals`, `other_removals`, `vm_reclaimed`, `pending`, `lost` and `read_failures`. The removal categories use the mapped call sites for explicit removal, owner cleanup and dispatch-error cleanup. `vm_reclaimed` means ownership was handed to VM destruction, not an observed individual unref. Unexpected categories, lost observations or read failures prevent a complete cleanup conclusion.

Use the reload sequence above when investigating owner transitions. For a schema-4 initial sequence, load a playable save and wait ten seconds before exiting normally. Expected results are at least five initialized controllers, one explicit unload, one contained controller error, one listener-error check, one initialization rollback, one initialization recovery, and zero unexpected failures. The listener ledger should show four explicit removals and one dispatch-error removal, followed by successful delivery to the final listener. Before an owner transition, six retained listener references balance five releases and one active listener. Both rollback counters and the ledger are needed to establish cleanup; a counter alone is insufficient.

Reload cleanup should increase both the session's owner retirements and the listener observer's owner removals, followed by a fresh session if an eligible owner resumes. An initialized ordinary session owns one controller reference and one listener reference; between construction and its first invocation, or after rollback or a listener error, the controller temporarily remains active without a listener. No visible effect, movement or hotkey is required for the Lua checks themselves.

The schema-3 sequence has passed in the game: the observer recorded two explicit removals and one dispatch-error removal, followed by an active replacement listener with successful event delivery. No unexpected failures, lost observations or pending releases were recorded at completion. This covers the deliberately failing embedded listener; it does not establish rollback of arbitrary initialization side effects or general Lua mod isolation.

The complete schema-4 sequence has passed in gameplay with owner affinity enabled: five controllers initialized, four controller references released, one initialization rollback and one successful recovery. The final persistent controller completed 497 invocations with zero unexpected failures or rejected operations. Six listener references balanced five releases and one active listener; the releases comprised four explicit removals and one dispatch-error removal, with no lost observations, pending releases or read failures. This establishes failed-initialization cleanup, retry and subsequent delivery for the example's own listener. No owner cleanup or VM-close notification occurred in that recording window, so it does not establish final reference reclamation on process exit or repeat the separate owner-transition evidence above.

Standalone Luau execution covers successful retry with mocked engine bindings, failed registration, cleanup errors before and after removal, continued delivery after removal, failed retry, shutdown before registration, and repeated shutdown. Native-provider tests separately check stack restoration, constructor arguments and reference ownership.

## Lua and Wasm mod interfaces

Lua can provide convenient access to engine bindings for properties, events and entity operations. Its eventual scope depends on the recovered binding contracts and lifecycle rules, not on the size or complexity of a script. Complex weapon systems may need Lua callbacks, Wasm logic, native bridge operations, resource integration or a combination of these.

Direct execution inside the engine VM must be treated as trusted scripting until isolation, exposed libraries and execution limits have been established. It does not inherit Wasm capabilities or fuel limits. [Luau's embedding guidance](https://luau.org/sandbox/) also distinguishes language-level safety from host API access and isolation between scripts; it assumes compiler-produced bytecode. CRML's offline bytecode parser is an inspection tool, not a security verifier for executable mod input.

See [reviewed engine paths](engine-paths.md) for callback and event dispatch, and [Lua bytecode tools](binlua.md) for offline inspection and reconstruction.
