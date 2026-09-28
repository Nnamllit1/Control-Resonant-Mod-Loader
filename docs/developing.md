---
title: Create a Wasm mod
description: Create a CONTROL Resonant WebAssembly mod with a manifest, lifecycle exports, host logging, and explicit capabilities.
---

# Your first mod

## Get the SDK

Download `crml-sdk-<version>-windows-x64.zip` from [GitHub Releases](https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases) and extract it into a working folder outside the game directory. It includes headers, example sources, a WAT compiler and a standalone host. Open PowerShell in that extracted folder for the commands below. You do not need to compile the native runtime to run this example.

To change the loader or runtime itself, use [Building from source](building.md). A source build puts the tools under `dist/crml/`; substitute that path for `tools/` in this guide.

## Run the hello example

Create a working package, copy its manifest, compile the supplied WAT source and run it:

```powershell
New-Item -ItemType Directory -Path mods/hello
Copy-Item examples/hello/mod.ini mods/hello/mod.ini
.\tools\crml_wat.exe examples/hello/hello.wat mods/hello/hello.wasm
.\tools\crml_host.exe mods 2
```

The greeting followed by `Active: 1; failures: 0` confirms the example ran. `examples/hello/hello.wat` is readable WebAssembly; the runtime accepts compiled `.wasm` packages. The standalone host does not start or control the game.

## Create a package

Create `mods/my-mod/` and add a [manifest](api.md#manifest). Compile your module with the supplied tool:

```powershell
.\tools\crml_wat.exe examples/hello/hello.wat mods/my-mod/hello.wasm
.\tools\crml_host.exe mods 10
```

Change the package ID to `my-mod`. IDs must be unique across the loaded packages. Remove a package directory from the mods root to disable it; restart the host or game to reload packages.

To run a mod in-game, [install the matching runtime](installation.md), close the game and copy your package folder into `crml/mods/`. Gameplay examples can require a particular runtime mode; follow their API and example documentation. Do not copy the SDK's tools into the game directory.

## C and C++ guests

The guest header is `sdk/include/crml.h`; `examples/hello/hello.c` shows the same greeting in C. With a Clang distribution that includes the wasm32 backend and wasm linker:

```powershell
clang --target=wasm32 -O2 -nostdlib -Isdk/include examples/hello/hello.c -Wl,--no-entry -Wl,--export-memory -Wl,--initial-memory=131072 -Wl,--max-memory=16777216 -o mods/my-mod/hello.wasm
```

The C example is an alternative source; the normal build uses WAT and does not install Clang. Do not target WASI, add native dependencies, or assume a C runtime is available. Any Wasm-producing language may be used if it implements the documented core-Wasm ABI and imports only allowed host functions.

## Where mod behavior belongs

Keep controls, timing, state and combinations of API calls in the mod's example/package directory. The visibility example chooses its own hide button; the physics-damping example detects button presses, requests selection, obtains its token and supplies the damping value and duration. Both include WAT and C source beside `mod.ini`. Edit and compile the source you intend to run; rebuilding the native runtime is unnecessary for guest behavior changes.

The trusted runtime provides engine operations and enforces their boundaries: it resolves live objects, schedules access on the researched engine phase, checks values and identities, and performs cleanup when a mod stops. A helper that contains an entire gameplay feature, such as the legacy `noclip_poll`, is not the pattern for new composable APIs. Prefer explicit requests such as `visibility_set` and `physics_apply`, with mod policy in the guest.

The [movement example](movement.md) also keeps camera-relative axes, speed selection and toggling in its guest. Its runtime request is a bounded world-space velocity; it does not delegate the control scheme to `noclip_poll`.

## Lifecycle

The host validates signatures, checks ABI version 1, and calls `crml_init`. Optional ticks receive elapsed seconds. A trap disables the offending mod and releases its store. Healthy mods continue.

The standalone host calls optional shutdown callbacks on exit. Abrupt termination of the game does not guarantee shutdown; mods must not depend on it for durable writes or restoring game state.
