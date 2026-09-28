<img src="docs/assets/logo.svg" width="80" height="80" alt="CRML logo">

# CONTROL Resonant Mod Loader

A Windows x64 mod framework for **CONTROL Resonant**, with a native loader and sandboxed WebAssembly mods.

[Documentation and Northlight engine research](https://crml.nnamllit.de/) cover installation, mod development, asset formats, scripting, ECS entities, rendering, and physics. The research is also available to native mod and tool developers independently of the loader. For supported gameplay operations, building on CRML gives your mod a shared runtime, a bounded API and readable examples to start from. [Create your first mod](docs/developing.md).

**Experimental developer preview.** Includes an XInput proxy, a sandboxed hello mod, and an opt-in noclip prototype with a Wasm example and status overlay. Compatibility is limited to the fingerprinted game build. See the [noclip guide](docs/gameplay.md) for controls and known limitations.

The isolated [Wasm physics service](docs/api.md#experimental-prop-damping) supports nearby-prop selection and temporary linear damping through owner-scoped handles. Its [example](docs/physics-trial.md#wasm-damping-example) uses F7/F8; it runs separately from the native F9/F10 trial and other gameplay hooks.

The [movement example](docs/movement.md) implements controls and camera-relative velocity in Wasm using the experimental `player.motion` API. Its separate mode leaves fall/reset behavior under game control.

## Download and install

No compiling or developer tools needed. Open [Releases](https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases) and expand **Assets**:

- **Mod loader:** download `crml-runtime-<version>-windows-x64.zip`.
- **Noclip & Free Flight, including the loader:** download `crml-noclip-bundle-<version>-windows-x64.zip`.

Close the game, extract the ZIP, then copy **the extracted folder's contents** beside `CONTROLResonant.exe`. Find that folder through Steam: right-click the game, then **Manage > Browse local files**. Start through Steam as usual, or double-click `CONTROLResonant.exe` with Steam running. With the flight bundle, load a save and press **F6**.

The **SDK** and **Source code** downloads are for developers. See [installation, updates and removal](docs/installation.md) for the folder layout and full instructions.

## Build from source

Requires Python 3.10+, Visual Studio 2022/2026 with Desktop development with C++, and CMake 3.24+ (the Visual Studio component is supported).

```powershell
.\build.bat -Test
.\dist\crml\crml_host.exe .\dist\crml\mods 2
```

The build downloads the official Wasmtime 49.0.0 C API archive and checks its pinned SHA-256. It produces `dist/xinput1_4.dll`, the runtime, developer tools, and a working hello-world mod. No native mod DLLs, WASI, filesystem APIs, network APIs, or arbitrary game-memory APIs are exposed to guests.

## Project layout

```text
loader/     XInput proxy and Windows startup boundary
runtime/    Wasmtime host, package discovery, and lifecycle
sdk/        Versioned guest API
mods/       Local mod packages (ignored by Git)
examples/   Maintained mod sources
tools/      Standalone host, WAT compiler, inspection and installation
docs/       Player, mod-author, and contributor documentation
tests/      Sandbox and proxy integration tests
```

The organization and documentation style follow Hammer Addons. The mod execution model is different: each Wasm mod receives a separate store with bounded memory and execution fuel.

Read the [installation guide](docs/installation.md), [create a mod](docs/developing.md), or review the [architecture](docs/architecture.md) and [noclip guide](docs/gameplay.md).

```powershell
python -m venv .venv-docs
.\.venv-docs\Scripts\python.exe -m pip install -r requirements-docs.txt
.\.venv-docs\Scripts\python.exe -m mkdocs serve
```

This is an independent community project, not affiliated with Remedy Entertainment. CONTROL Resonant and associated marks belong to their respective owners.
