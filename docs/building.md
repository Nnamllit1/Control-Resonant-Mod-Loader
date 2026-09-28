---
title: Build CRML from source
description: Build and test the CRML runtime, install a development build, and use the receipt-based installer for updates and removal.
---

# Build from source

This guide is for contributors changing the native loader or runtime. To play with mods, use the [ready-to-use release packages](installation.md). To write a Wasm mod, start with the [SDK guide](developing.md).

## Requirements

Use Windows x64, Python 3.10 or newer, Visual Studio 2022/2026 with the **Desktop development with C++** workload, and CMake 3.24 or newer. The Visual Studio CMake component is supported.

From a source checkout, build and run the standalone example:

```powershell
.\build.bat -Test
.\dist\crml\crml_host.exe .\dist\crml\mods 2
```

The build downloads the pinned Wasmtime C API dependency and checks its SHA-256. Output goes to `dist/`. The hello mod prints a greeting followed by `Active: 1; failures: 0`. This exercises the sandbox without starting the game.

The default build supplies the runtime and hello mod. Use the documented options for the [Wasm movement example](movement.md), [physics example](physics-trial.md#wasm-damping-example) or [engine diagnostics](engine-validation.md). Development output can contain diagnostic markers; use the installer to select a mode rather than copying the entire `dist` tree into the game.

## Inspect an installation

```powershell
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
python tools/inspect-game.py "$gameDir\CONTROLResonant.exe"
python tools/install.py "$gameDir"
```

The installer defaults to a read-only preview. It checks the executable against `compatibility.json` and refuses an existing `xinput1_4.dll` or `crml/` directory for a fresh installation.

## Install a development build

Close the game, review the preview, then apply it:

```powershell
python tools/install.py "$gameDir" --apply
```

This installs the proxy, runtime and hello package, and records ownership in an installation receipt. It does not modify the game executable or replace a shipped DLL. Start normally through Steam and check `crml/crml.log` for the hello greeting. No visible gameplay change is expected from hello.

## Update an existing installation

For an installation created by this installer:

```powershell
python tools/install.py "$gameDir" --update
python tools/install.py "$gameDir" --update --apply
```

Close the game before applying. Updates verify owned files, stage replacements and roll back replacements if copying fails. Extra mods, logs and settings are preserved. Modified owned files are refused rather than overwritten.

Manually copied release ZIPs do not create an installer receipt. Use the [manual update instructions](installation.md#update-a-downloaded-release) for those installations. Copying release files over an installer-managed build can invalidate its recorded hashes.

## Remove the loader

With the game closed, preview and apply removal:

```powershell
python tools/install.py "$gameDir" --uninstall
python tools/install.py "$gameDir" --uninstall --apply
```

Removal checks the receipt and owned-file hashes first. Modified files cause refusal; additional mod files and logs are not removed. Empty directories may remain. For a manually copied release, use the [player removal guide](installation.md#remove-a-downloaded-release).

## Package a release

Follow [Building releases](releases.md) for tested ZIPs and the publishing workflow. For website changes, see [Maintaining the website](documentation.md).
