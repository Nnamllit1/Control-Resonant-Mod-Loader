# Getting started

## Run the supplied example

```powershell
.\build.bat -Test
.\dist\crml\crml_host.exe .\dist\crml\mods 2
```

`examples/hello/hello.wat` is readable WebAssembly. The build converts it to a binary module with the bundled `crml_wat` tool. The runtime accepts binary `.wasm` packages only.

## Create a package

Create `mods/my-mod/` and add a [manifest](api.md#manifest). Compile your module with the supplied tool:

```powershell
.\dist\crml\crml_wat.exe examples/hello/hello.wat mods/my-mod/hello.wasm
.\dist\crml\crml_host.exe mods 10
```

Change the package ID to `my-mod`. IDs must be unique across the loaded packages. Remove a package directory from the mods root to disable it; restart the host or game to reload packages.

## C and C++ guests

The guest header is `sdk/include/crml.h`; `examples/hello/hello.c` shows the same greeting in C. With a Clang distribution that includes the wasm32 backend and wasm linker:

```powershell
clang --target=wasm32 -O2 -nostdlib -Isdk/include examples/hello/hello.c -Wl,--no-entry -Wl,--export-memory -Wl,--initial-memory=131072 -Wl,--max-memory=16777216 -o mods/my-mod/hello.wasm
```

The C example is an alternative source; the normal build uses WAT and does not install Clang. Do not target WASI, add native dependencies, or assume a C runtime is available. Any Wasm-producing language may be used if it implements the documented core-Wasm ABI and imports only allowed host functions.

## Lifecycle

The host validates signatures, checks ABI version 1, and calls `crml_init`. Optional ticks receive elapsed seconds. A trap disables the offending mod and releases its store. Healthy mods continue.

The standalone host calls optional shutdown callbacks on exit. Abrupt termination of the game does not guarantee shutdown; mods must not depend on it for durable writes or restoring game state.
