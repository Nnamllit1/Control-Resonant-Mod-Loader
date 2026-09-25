# CONTROL Resonant Mod Loader

A Windows x64 mod framework for **CONTROL Resonant**, with a native loader and sandboxed WebAssembly mods.

**Experimental developer preview.** The XInput proxy and sandboxed hello mod have loaded in the recorded game build. An opt-in noclip prototype includes a Wasm example and status overlay; wall and ceiling traversal have been reported in gameplay. Floor descent, camera-heading controls, input isolation, boundary-reset suppression, and restoration still need verification. Controller input and clean shutdown also need dedicated validation. See the [noclip test guide](docs/gameplay.md).

## Build and try it

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

Read the [installation guide](docs/installation.md), [create a mod](docs/developing.md), or review the [architecture](docs/architecture.md) and [noclip milestone](docs/gameplay.md).

```powershell
python -m venv .venv-docs
.\.venv-docs\Scripts\python.exe -m pip install -r requirements-docs.txt
.\.venv-docs\Scripts\python.exe -m mkdocs serve
```

This is an independent community project, not affiliated with Remedy Entertainment. CONTROL Resonant and associated marks belong to their respective owners.
