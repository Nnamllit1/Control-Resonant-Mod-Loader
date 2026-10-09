---
description: Learn how CRML isolates WebAssembly mods with Wasmtime, memory limits, execution budgets, and capability-based host access.
---

# Sandbox limits

Each mod runs in its own Wasmtime store. The runtime exposes only explicitly linked functions; WASI is never linked. Native DLLs, serialized compiled modules, and text-format modules are rejected at the package boundary.

| Resource | Host ceiling |
| --- | --- |
| Mod directories | 32 |
| Manifest | 8 KiB |
| Binary Wasm file | 4 MiB |
| Linear memory | 16 MiB per mod |
| Instances / memories / tables | 1 each per mod |
| Table elements | 4,096 |
| Wasm stack | 256 KiB |
| Fuel | 100,000 per start/version/lifecycle invocation |
| Log calls / bytes | 32 / 16 KiB per invocation |
| Gameplay observations (development builds) | 8 per invocation, shared across input and state queries |
| Gameplay commands | 8 per invocation, shared across mutations and explicit cleanup |
| Guest session log | 64 KiB per mod, including prefixes; later output suppressed |
| Host diagnostic log | Separate 1 MiB allowance, inaccessible to guest logging |
| Mod storage (development builds) | 64 KiB committed + one pending record per mod; 16 calls per invocation; one accepted write per second |
| Typed settings (development builds) | 32 definitions per mod; 64 calls per invocation; copied strings and validated numeric values |
| Live action rebinding (development builds) | 16 slots per mod; 16 binding calls per invocation; named keys only |

The separate observation/command and session-log allowances apply to development builds after Alpha 4.3. Released runtimes through Alpha 4.3 share eight gameplay observations/commands; see the [import classification](api.md#capability-availability-and-cleanup). Alpha 4.3 uses a shared approximately 4 MiB sink. Development builds retain the current log and up to three previous sessions; older releases overwrite the log on startup. Guest session suppression does not trap a mod; exceeding the per-invocation call or byte budget does.

Threads, memory64, multiple memories, and Wasm GC are disabled. Failed imports, invalid signatures, traps, and exhausted fuel reject or disable that mod. Fuel and memory limits apply before the Wasm start function executes.

## What this boundary means

Guest instructions cannot normally dereference game pointers or call Windows APIs. The trusted native host and Wasmtime still run inside the game process. Engine vulnerabilities, native bugs, allocation failures, and process crashes are not contained in a separate operating-system sandbox.

Fuel limits guest execution, not wall-clock time spent compiling a module or running a host function. File-size and package-count limits reduce exposure but do not provide a hard total compiler-memory or compile-time budget.

Package directories and module/manifest files may not be reparse points. The package tree must not be modified concurrently with loading; path checks do not defend against another native process racing filesystem operations.

Keep the runtime updated and audit every added host function. Adding unrestricted memory reads/writes, native calls, script evaluation, or DLL loading would defeat the intended mod boundary.

The optional [storage capability](mod-storage.md) exposes one mod-owned record,
not a filesystem. It is installation-scoped and survives uninstall/reinstall with
the same manifest ID. IDs are namespaces, not authenticated publisher identities.

[Feedback](mod-feedback.md) copies at most 240 bytes per message and permits one
active message per mod, four globally, with a one-second publish interval and a
ten-second maximum lifetime. Its imports have a separate 16-call callback budget.
Guests cannot choose markup, renderer selectors, executable code or another mod's
receipt. Feedback uses no native input handlers and never changes focus.
