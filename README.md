<img src="docs/assets/logo.svg" width="80" height="80" alt="CRML logo">

# CONTROL Resonant Mod Loader

A Windows x64 mod framework for **CONTROL Resonant**, with a native loader and sandboxed WebAssembly mods.

Install the ready-to-use runtime, add compatible mods, and start the game normally. CRML is experimental and supports the game build listed in each release's compatibility information.

## Download and install

No compiling or developer tools needed. Open [Releases](https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases), expand **Assets**, and download **`crml-runtime-<version>-windows-x64.zip`**. Replace `<version>` with the release number required by your mod.

Close the game, extract the ZIP, then copy **the extracted folder's contents** beside `CONTROLResonant.exe`. Find that folder through Steam: right-click the game, then **Manage > Browse local files**. Install your chosen mods using their instructions, then start through Steam as usual or double-click `CONTROLResonant.exe` with Steam running.

The **SDK** and **Source code** downloads are for developers. The runtime supplies the loader; individual mods supply gameplay features and controls. See [installation, updates and removal](docs/installation.md).

## Make mods and explore the engine

Use CRML's [SDK and examples](docs/developing.md) to build on a shared runtime and [bounded gameplay API](docs/api.md). The movement example keeps its controls in Wasm; other examples cover visibility and temporary physics damping. Available operations depend on the selected runtime mode.

[Public Northlight research](https://crml.nnamllit.de/engine-research/) covers assets, scripting, entities, rendering and physics. Native mod and tool developers can also use it independently of the loader.

If CRML fits your mod, we'd appreciate you building on the shared runtime. If you use the research independently, a credit to CRML contributors and a link to the relevant findings are welcome. See [Community courtesy](docs/engine-research.md#community-courtesy) for suggested credits; these are voluntary requests, not extra license terms.

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

See [Building from source](docs/building.md), [Architecture](docs/architecture.md), [Building releases](docs/releases.md) or [Maintaining the website](docs/documentation.md) to contribute.

This is an independent community project, not affiliated with Remedy Entertainment. CONTROL Resonant and associated marks belong to their respective owners.
