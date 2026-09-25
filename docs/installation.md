# Installation

## Build the developer preview

Use Windows x64, Python 3.10 or newer, and Visual Studio 2022/2026 with the **Desktop development with C++** workload and CMake tools.

```powershell
.\build.bat -Test
.\dist\crml\crml_host.exe .\dist\crml\mods 2
```

The hello mod prints its greeting, followed by `Active: 1; failures: 0`. The host calls shutdown before exiting. This verifies the sandbox without starting the game.

## Inspect an installation

```powershell
python tools/inspect-game.py "C:\Games\CONTROL Resonant\CONTROLResonant.exe"
python tools/install.py "C:\Games\CONTROL Resonant"
```

Replace `C:\Games\CONTROL Resonant` with your installation path. The installer defaults to a read-only preview. It checks the executable against `compatibility.json` and refuses any existing `xinput1_4.dll` or `crml/` directory. The recorded profile has passed a live bootstrap check; it does not yet have a gameplay bridge.

## Experimental game bootstrap

Close the game before installing. After reviewing the preview, use `--apply` to copy the proxy, runtime, and hello package. Nothing edits the game's executable or replaces a shipped DLL.

```powershell
python tools/install.py "C:\Games\CONTROL Resonant" --apply
```

Start the game normally. If the loader is reached, `crml/crml.log` beside the executable records the hello greeting. Controller polling remains forwarded to Windows. No gameplay changes are made by the example.

An absent log means startup has not been established; it is not evidence that a gameplay mod ran. See [troubleshooting](troubleshooting.md).

## Remove the loader

With the game closed, preview and then apply removal:

```powershell
python tools/install.py "C:\Games\CONTROL Resonant" --uninstall
python tools/install.py "C:\Games\CONTROL Resonant" --uninstall --apply
```

Removal checks the installation receipt and hashes first. Modified files are preserved by refusing the operation; extra mod files and logs are never removed. Empty directories may remain.

## Update an existing installation

With the game closed, run `python tools/install.py "C:\Games\CONTROL Resonant" --update` to preview, then add `--apply`. Updates verify every owned file, stage replacements, and roll back replaced files if publishing fails. Additional mods, logs, and settings are preserved. Modified owned files are refused rather than overwritten.

The [experimental noclip guide](gameplay.md) describes the separate build and installation flags needed for gameplay testing.
