---
title: Engine Lua integration
description: CONTROL Resonant engine Lua loading, protected execution, script environments, and the distinction from sandboxed Wasm mods.
---

# Engine Lua integration

CRML's engine Lua integration is experimental. The bytecode loader and protected execution path have been traced, and a fixed smoke test is available in developer builds. Loading arbitrary `.lua` mod packages, callback ownership, hot reload and unloading are not supported yet. The existing [Wasm API](api.md) remains the supported mod interface.

## Execution path

The [reference map](research/lua-execution-map.json) records call sites for the executable fingerprint `2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0`. These are executable RVAs; they are internal interfaces, not stable exports.

| Routine | RVA | Recovered calling convention on Windows x64 |
| --- | --- | --- |
| Bytecode loader | `0x2c27b70` | `int(state, chunkLabel, bytes, byteCount, environmentIndex)` |
| Resource load wrapper | `0x3229280` | Boolean result; receives state, bytes, count, label, error destination, environment index |
| Protected Lua call | `0x2c4fc90` | `int(state, argumentCount, resultCount, errorHandlerIndex)` |
| Internal protected callback | `0x2c434f0` | `int(state, callback, userData, savedTopOffset, errorHandlerOffset)` |
| Stack top setter | `0x2c4d040` | `void(state, index)`; positive indices are relative to the current frame base |

The loader receives bytecode **without** the resource envelope. An environment index of zero selects the VM's globals table at state `+0x58`; a nonzero index follows the VM's index-resolution path. A successful load pushes a closure; it does not execute the script. The resource wrapper extracts and removes a load error on failure. Its separate error destination is an engine object and must not be replaced with an arbitrary C string buffer.

The protected callback saves call-frame position and native-call depth. Its error path closes pending upvalues, places an error object at the saved stack position, and restores the frame base. Stack positions are offsets because VM allocation can relocate the stack. The callback receives `(state, userData)`. This barrier covers loading as well as execution in the CRML probe; protecting only the later Lua call would leave loader allocation outside that barrier.

The engine's fixed script update makes a protected call at `0x1a0aada` with one argument, zero results and error handler index 1. The probe uses the return from that exact call after success. It shares the existing boundary-guard hook, runs synchronously on the engine thread, and does not retain a VM, world or entity pointer afterward. A suspended VM, non-root call frame, missing context, insufficient stack space or installed protected-error debugger callback prevents execution.

These ABI interpretations come from instruction and data-flow inspection. Encoded-reference checks and native mock tests do not establish in-game execution compatibility.

## Developer smoke test

The self-authored scripts live in `examples/lua-probe/`:

1. `arithmetic.luau` uses a small local table and returns **42**.
2. `error.luau` deliberately calls a nil value. The expected protected-call status is **2**.
3. The arithmetic script runs again and must return **42**.
4. `bindings.luau` returns a bit mask identifying selected function names in the default environment. It does not invoke those functions.

The native harness restores the stack after every step and compares the existing stack values and frame identity. Any failed step stops the sequence. It keeps no registry references, registers no callbacks, writes no globals and runs at most once per process. Removing its stack references makes the temporary closures eligible for ordinary VM garbage collection; this is not evidence of mod callback teardown or general unload support.

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
| `passed` | All four stages produced their expected results and restored the stack |
| `steps` | Load, call and outer protection statuses, numeric result and restoration result |
| `thread` | The engine thread that executed the test |

The binding mask uses bits 1, 2, 4, 8, 16, 32 and 64 for `nl_update_callback`, `nl_add_event_handler`, `nl_send_custom_event`, `nl_copy_world_transform`, `spawn_bundle_with_instigator`, `npc_spawn` and `npc_despawn`, respectively. A set bit proves only that the default environment exposes a function under that name. A clear bit does not establish that the operation is unavailable in entity-specific environments. Argument schemas, context requirements and behavior remain separate questions.

Normal builds omit `-LuaProbe` and do not arm this test. The compiler is not a runtime dependency. The embedded bytecode can be reproduced from the checked-in sources using a separately built [Luau 0.650 compiler](https://github.com/luau-lang/luau/tree/0.650):

```powershell
$compiler = Read-Host 'Path to Luau 0.650 luau-compile executable'
python tools/compile_lua_probe.py --compiler "$compiler" --check
```

Omit `--check` to regenerate `runtime/lua_probe_bytecode.h`. The header contains only CRML-authored scripts, compiled at optimization level 0 with debug level 2. Matching a bytecode header alone does not establish compatibility with the engine VM.

## Lua and Wasm mod interfaces

Lua can provide convenient access to engine bindings for properties, events and entity operations. Its eventual scope depends on the recovered binding contracts and lifecycle rules, not on the size or complexity of a script. Complex weapon systems may need Lua callbacks, Wasm logic, native bridge operations, resource integration or a combination of these.

Direct execution inside the engine VM must be treated as trusted scripting until isolation, exposed libraries and execution limits have been established. It does not inherit Wasm capabilities or fuel limits. [Luau's embedding guidance](https://luau.org/sandbox/) also distinguishes language-level safety from host API access and isolation between scripts; it assumes compiler-produced bytecode. CRML's offline bytecode parser is an inspection tool, not a security verifier for executable mod input.

See [reviewed engine paths](engine-paths.md) for callback and event dispatch, and [Lua bytecode tools](binlua.md) for offline inspection and reconstruction.
