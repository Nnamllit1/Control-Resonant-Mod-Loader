---
title: Install the mod loader
description: Download and install CONTROL Resonant Mod Loader by copying the release files into your game folder. No compiling or developer tools needed.
---

# Installation

## Download a ready-to-use package

You do not need Python, Visual Studio or a compiler to install a release.

Open [GitHub Releases](https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases), choose a release and expand **Assets**. Download a Windows x64 ZIP from this table. The version number replaces `<version>` in the filename.

| What you want | Download |
| --- | --- |
| The mod loader, ready for adding mods | `crml-runtime-<version>-windows-x64.zip` |
| Noclip & Free Flight with everything needed | `crml-noclip-bundle-<version>-windows-x64.zip` |
| Just Noclip & Free Flight, with the matching runtime already installed | `crml-noclip-<version>-windows-x64.zip` |

For playing, choose one of these packages. The **SDK** and GitHub's **Source code** downloads are for development. The runtime by itself includes a small hello mod; it does not enable flight or add a flight panel.

## Copy the files and start the game

1. **Close CONTROL Resonant.**
2. Right-click the downloaded ZIP in Windows and choose **Extract All**.
3. In your Steam Library, right-click **CONTROL Resonant**, then choose **Manage > Browse local files**. Find the folder containing `CONTROLResonant.exe`.
4. Open the extracted download folder and **copy its contents into that game folder**. Copy the contents, rather than the outer folder named after the download.
5. **Start the game normally through Steam**, or double-click `CONTROLResonant.exe` with Steam running. CRML loads automatically; there is no separate loader application to open.

After installing the runtime or bundle, the files should be arranged like this:

```text
CONTROLResonant.exe       <- the existing game executable
xinput1_4.dll             <- from the download
crml/
  crml_runtime.dll
  wasmtime.dll
  mods/
```

Do not place the files in Documents, the save folder, or an extra subfolder inside the game directory. The game executable stays where it is. The ZIP also includes a text README with installation and removal instructions.

For **Noclip & Free Flight**, load a playable save and press **F6**. Use **WASD** to move, **Space/Ctrl** to rise or descend, and **Shift** to fly faster. **Insert** hides or shows the panel. Return above solid ground before switching flight off. The installed mod folder is named `crml/mods/movement`; this is the release's flight mod.

## Add more mods

Follow the mod's included instructions. A standard CRML Wasm mod has its own folder under `crml/mods/`, containing `mod.ini` and a `.wasm` file. If its download already contains a `crml` folder, merge that folder into the game's existing `crml` folder. Some mods require a particular CRML version or gameplay mode.

## Update a downloaded release

Close the game and back up your existing `xinput1_4.dll` and `crml` folder. Copy the new release's contents into the same game folder, merge the `crml` folders, and replace CRML's files when Windows asks. Keep other installed mod folders. Use matching runtime and mod releases.

If an existing `xinput1_4.dll` belongs to another loader, do not overwrite it with CRML. Two loaders using that filename cannot be installed together by copying both packages.

## Remove a downloaded release

Close the game and remove CRML's `xinput1_4.dll` from beside the executable. This stops the loader from starting. Remove `crml` too if you no longer need its mods, settings or logs. No original game files need to be restored.

To remove only Noclip & Free Flight, delete `crml/movement-wasm.enabled` and `crml/mods/movement`. Leave the runtime and other mods in place.

## If it does not load

Check the folder layout above first. After starting the game, `crml/crml.log` records loader activity. See [troubleshooting](troubleshooting.md) if no log appears or a mod does not start. Gameplay support is limited to the game build listed in the release's `compatibility.json`; a game update may require a new CRML release.

## Build the developer preview

The remaining instructions are for building from source. They are not needed for the ZIP installation above.

Use Windows x64, Python 3.10 or newer, and Visual Studio 2022/2026 with the **Desktop development with C++** workload and CMake tools.

```powershell
.\build.bat -Test
.\dist\crml\crml_host.exe .\dist\crml\mods 2
```

The hello mod prints its greeting, followed by `Active: 1; failures: 0`. The host calls shutdown before exiting. This verifies the sandbox without starting the game.

## Inspect an installation

```powershell
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
python tools/inspect-game.py "$gameDir\CONTROLResonant.exe"
python tools/install.py "$gameDir"
```

Enter your installation directory when prompted; the examples use `$gameDir` for that location. The installer defaults to a read-only preview. It checks the executable against `compatibility.json` and refuses any existing `xinput1_4.dll` or `crml/` directory. The default source build installs the runtime and hello mod; gameplay modes require their documented build and installation options.

## Experimental game bootstrap

Close the game before installing. After reviewing the preview, use `--apply` to copy the proxy, runtime, and hello package. Nothing edits the game's executable or replaces a shipped DLL.

```powershell
python tools/install.py "$gameDir" --apply
```

Start the game normally. If the loader is reached, `crml/crml.log` beside the executable records the hello greeting. Controller polling remains forwarded to Windows. No gameplay changes are made by the example.

An absent log means startup has not been established; it is not evidence that a gameplay mod ran. See [troubleshooting](troubleshooting.md).

## Remove the loader

For an installation created by the Python installer, close the game, then preview and apply removal:

```powershell
python tools/install.py "$gameDir" --uninstall
python tools/install.py "$gameDir" --uninstall --apply
```

Removal checks the installation receipt and hashes first. Modified files are preserved by refusing the operation; extra mod files and logs are never removed. Empty directories may remain. Manually copied release ZIPs do not create this receipt; use the manual removal instructions above for those installations.

## Update an existing installation

For an installation created by the Python installer, close the game and run `python tools/install.py "$gameDir" --update` to preview, then add `--apply`. Updates verify every owned file, stage replacements, and roll back replaced files if publishing fails. Additional mods, logs, and settings are preserved. Modified owned files are refused rather than overwritten. For a manually copied release, use the download update instructions above.

The [experimental noclip guide](gameplay.md) describes the separate build and installation flags needed for gameplay testing.
