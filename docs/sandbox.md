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
| In-game log | Approximately 4 MiB per session, overwritten on next startup |

Threads, memory64, multiple memories, and Wasm GC are disabled. Failed imports, invalid signatures, traps, and exhausted fuel reject or disable that mod. Fuel and memory limits apply before the Wasm start function executes.

## What this boundary means

Guest instructions cannot normally dereference game pointers or call Windows APIs. The trusted native host and Wasmtime still run inside the game process. Engine vulnerabilities, native bugs, allocation failures, and process crashes are not contained in a separate operating-system sandbox.

Fuel limits guest execution, not wall-clock time spent compiling a module or running a host function. File-size and package-count limits reduce exposure but do not provide a hard total compiler-memory or compile-time budget. A future hardened mode may compile or execute in a separate process.

Package directories and module/manifest files may not be reparse points. The package tree must not be modified concurrently with loading; path checks do not defend against another native process racing filesystem operations.

Keep the runtime updated and audit every added host function. Adding unrestricted memory reads/writes, native calls, script evaluation, or DLL loading would defeat the intended mod boundary.
