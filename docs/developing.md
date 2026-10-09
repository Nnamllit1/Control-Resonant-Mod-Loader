---
title: Create a Wasm mod
description: Create a CONTROL Resonant WebAssembly mod with a manifest, lifecycle exports, host logging, and explicit capabilities.
---

# Your first mod

## Get the SDK

This guide targets the **Alpha 5 development source checkpoint**. Its new author
CLI and expanded APIs require a matching runtime and SDK built from
[source](building.md); they are not part of the published Alpha 4.5 downloads.
The commands below require Python 3.11+.

For a published version, download `crml-sdk-<version>-windows-x64.zip` from
[GitHub Releases](https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases),
extract it outside the game directory, and follow that archive's documentation.
Those prebuilt packages do not require compiling the native runtime. Older
archives provide WAT tools rather than `tools/mod.py`.

To change the loader or runtime itself, use [Building from source](building.md). The author command finds tools in either an extracted SDK or a source checkout.

Install the pinned [WebAssembly SDK compiler](https://github.com/WebAssembly/wasi-sdk/releases/tag/wasi-sdk-27) once:

```powershell
python tools/mod.py toolchain
```

The checksum-verified download is approximately 543 MB. Only the compiler, linker and builtin headers are extracted; CRML builds freestanding wasm32 modules without a WASI sysroot or runtime. To use an existing wasm32-capable Clang and `wasm-ld`, pass `--clang <path>` to `build` or set `CRML_CLANG`.

## Run the hello example

Compile the C source and run it:

```powershell
python tools/mod.py build examples/hello --output mods/hello
python tools/mod.py check mods/hello
python tools/mod.py test mods/hello --ticks 2
```

The greeting followed by `Active: 1; failures: 0` confirms the example ran. `check` executes module start, initialization and shutdown; `test` also runs the requested number of ticks. These commands use the actual Wasmtime sandbox. They do not start the game, read the keyboard or provide engine services; successful loading does not prove gameplay behavior.

Use [scenario testing](mod-testing.md) to supply deterministic input, snapshots,
failures and competing owners with `tools/mod.py simulate`. Scenarios test guest
policy through the sandbox without requiring a running game.

## Create a package

Create a source folder from the template and build into a separate package directory:

```powershell
python tools/mod.py new source/my-mod --id my-mod
python tools/mod.py build source/my-mod --output mods/my-mod
python tools/mod.py test mods/my-mod --ticks 10
```

Edit `source/my-mod/main.c` and its [manifest](api.md#manifest), then rebuild. The build command compiles all `.c` files in that source directory, exports the required lifecycle functions and copies the manifest. Package IDs must be unique across loaded mods. Remove a package directory from the mods root to disable it; restart the host or game to reload packages.

To run a mod in-game, [install the matching runtime](installation.md), close the game and copy your package folder into `crml/mods/`. Alpha 4.2 prepares supported gameplay services from mod manifests. Older runtimes can require a particular mode; follow the version requirements in the API and example documentation. Do not copy the SDK's tools into the game directory.

For configurable shortcuts, start with `examples/input-actions` in the Alpha 4.2 SDK. Its manifest declares named controls, and the Wasm source decides what a press does. It needs no noclip or physics mode. Query [capability availability](api.md#capability-availability-and-cleanup) before offering a feature, check operation results, and use `crml_release()` when cancelling the mod's native leases. Availability does not mean the game currently has a valid player or target.

## C and C++ guests

The guest header is `sdk/include/crml.h`. The supported build command targets freestanding C11. For custom C/C++ tooling, export the ABI and lifecycle functions explicitly; ordinary C symbols are not automatically Wasm exports. A minimal manual C command is:

```powershell
clang --target=wasm32-unknown-unknown -O2 -nostdlib -Isdk/include examples/hello/hello.c -Wl,--no-entry -Wl,--export-memory -Wl,--export=crml_abi_version -Wl,--export=crml_init -Wl,--export-if-defined=crml_tick -Wl,--export-if-defined=crml_shutdown -Wl,--initial-memory=131072 -Wl,--max-memory=16777216 -o mods/hello/hello.wasm
```

C is the normal source for maintained examples. Their WAT files remain low-level reference fixtures and do not feed a normal build. Do not target WASI, add native dependencies, or assume a C runtime is available. Any Wasm-producing language may be used if it implements the documented core-Wasm ABI and imports only allowed host functions.

## Where mod behavior belongs

`examples/startup-skip` uses `ui.read` and `ui.activate` to continue past supported startup notices, and `ui.presentation` to hide the engine's named splash element temporarily while required initialization finishes. It also uses `media.read` and `media.skip` to select the boot video by its logical resource name, with explicit handling of the adapter's mapped-name provenance. Its guest chooses targets, lease duration and retry policy. See the [UI contract](api.md#startup-ui-state-and-actions) and [media contract](api.md#media-observation-and-skipping) for supported operations and cleanup behavior. These interfaces do not yet enumerate every screen or control arbitrary cinematics. Mod settings use the separate [typed settings service](mod-settings.md).

Two larger guest packages demonstrate those services together in the development SDK:

| Package | Behavior | Shared author facilities |
| --- | --- | --- |
| `examples/startup-preferences` | Per-screen startup choices and reviewed boot-movie skipping; observes command receipts before deciding whether to retry | Typed settings, versioned persistence, error feedback, explicit cleanup |
| `examples/photo-visibility` | Hold or toggle a configurable key to request hiding the player's root mesh for screenshots | Typed settings, versioned persistence, feedback, copied input/player state, explicit cleanup |

Each package includes C source, a manifest, installation instructions and executable
JSON scenarios. Build it with `tools/mod.py build` and run its scenarios with
`tools/mod.py simulate`; use a separate mods directory for each package's scenario.
All preferences and gameplay decisions live in the guest. Neither package adds
a native enable-feature helper. Use Startup Preferences instead of the simpler
Startup Skip package to avoid competing requests.

These packages target development imports after Alpha 4.3. Automated checks cover
Wasm execution, preference restart and failure behavior, and scripted service
outcomes. Live rendering and menu compatibility require separate game checks.
Photo Visibility is a mesh-visibility tool, not a camera or complete photo mode;
startup dispatch receipts do not prove that a transition completed.

For state-driven behavior, `examples/state-watch` reads player position and selected-camera state and implements its own displacement and camera-change detection in Wasm. `player.read` and `camera.read` do not grant movement or camera writes. The physics example also reads its selected body's damping and speed through `physics_read`; it checks observed damping separately from queue acceptance. Use these copied values to drive your own conditions and combine them with the supported write operations.

Keep controls, timing, state and combinations of API calls in the mod's example/package directory. The visibility example chooses its own hide button; the physics-damping example detects button presses, requests selection, obtains its token and supplies the damping value and duration. Both include WAT and C source beside `mod.ini`. Edit and compile the source you intend to run; rebuilding the native runtime is unnecessary for guest behavior changes.

The trusted runtime provides engine operations and enforces their boundaries: it resolves live objects, schedules access on the researched engine phase, checks values and identities, and performs cleanup when a mod stops. A helper that contains an entire gameplay feature, such as the legacy `noclip_poll`, is not the pattern for new composable APIs. Prefer explicit requests such as `visibility_set` and `physics_apply`, with mod policy in the guest.

The [movement example](movement.md) also keeps camera-relative axes, speed selection and toggling in its guest. Its runtime request is a bounded world-space velocity; it does not delegate the control scheme to `noclip_poll`.

For new C code, `crml.h` includes [descriptive helpers](sdk-contracts.md#descriptive-c-helpers)
such as `crml_player_flight_set_velocity`, `crml_prop_set_linear_damping` and
`crml_startup_read_screen`. They use existing ABI imports and permissions while
making the implemented scope visible at the call site. Rebuilding an example
does not expand its native capabilities.

When a required operation is missing, follow
[Extend the native bridge](extending-runtime.md). It covers what to include in a
capability proposal, where adapters belong, and the evidence needed before a
guest API can promise a new engine operation.

## Lifecycle

The host validates signatures, checks ABI version 1, and calls `crml_init`. Optional ticks receive elapsed seconds. A trap disables the offending mod and releases its store. Healthy mods continue.

The standalone host calls optional shutdown callbacks on exit. Abrupt termination of the game does not guarantee shutdown; mods must not depend on it for durable writes or restoring game state.

## Share your mod

Include the required CRML version and a link to the [runtime installation guide](installation.md) in your mod's description. A short "Built with CRML" credit helps players find the runtime and other authors discover the SDK. Credits are appreciated as a [community courtesy](engine-research.md#community-courtesy), not an additional license condition.
