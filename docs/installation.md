---
title: Install the mod loader
description: Download and install CONTROL Resonant Mod Loader by copying the release files into your game folder. No compiling or developer tools needed.
---

# Installation

## Download a ready-to-use package

**To install CRML, download `crml-runtime-<version>-windows-x64.zip`.** This is the ready-to-use loader for players. Install your chosen mods separately using their included instructions.

Open [GitHub Releases](https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases), choose a release that supports your game version and your mods, then expand **Assets**. `<version>` stands for the release number in the filename.

| Download | Who it is for |
| --- | --- |
| **`crml-runtime-<version>-windows-x64.zip`** | **Players installing CRML to use mods** |
| `crml-sdk-<version>-windows-x64.zip` | Mod authors who need development tools and examples |
| GitHub's **Source code** archives | Contributors building CRML itself |

**You do not need the SDK, Source code, Python or a compiler to play.** If a mod specifies a CRML version, use that version. The runtime download contains no gameplay mod or in-game panel; install your chosen mods separately.

## Copy the files and start the game

**Running CRML alongside another mod loader, including a native DLL mod loader, is not recommended.** Compatibility has not been verified or guaranteed. The combination may work, but loaders and their mods can interfere with shortcuts or gameplay, or cause crashes, even when they use different DLL filenames. Use one loader at a time; follow each loader's removal instructions when switching.

1. **Close CONTROL Resonant.**
2. Right-click the downloaded ZIP in Windows and choose **Extract All**.
3. Find the folder containing `CONTROLResonant.exe` using your launcher or storefront. In Steam, right-click **CONTROL Resonant**, then choose **Manage > Browse local files**.
4. Open the extracted download folder and **copy its contents into that game folder**. Select `xinput1_4.dll`, `crml` and the accompanying files. Copy the contents, rather than the outer folder named after the download. If an existing `xinput1_4.dll` belongs to another loader, stop here: CRML cannot share that filename with it.
5. **Start the game normally through your launcher.** CRML loads automatically; there is no separate loader application to open. Alpha 4.1 asks before starting mods on an unrecognized executable; read the prompt before choosing whether to continue.

After installing the runtime, the files should be arranged like this:

```text
CONTROLResonant.exe       <- the existing game executable
xinput1_4.dll             <- from the download
crml/
  crml_runtime.dll
  wasmtime.dll
  mods/
```

Do not place the files in Documents, the save folder, or an extra subfolder inside the game directory. The game executable stays where it is. The ZIP also includes a text README with installation and removal instructions.

After the game starts, `crml/crml.log` records that CRML loaded. The runtime alone does not add a general mod menu or gameplay controls. Each mod provides its own features and instructions.

## Add more mods

Close the game and follow the mod's included instructions. A standard CRML Wasm mod has its own folder under `crml/mods/`, containing `mod.ini` and a `.wasm` file. If its download already contains a `crml` folder, merge that folder into the game's existing `crml` folder. Use the CRML version required by the mod. From Alpha 4.2, supported Wasm gameplay services start from mod manifests; separate movement, visibility and physics installation modes are unnecessary. Older releases can require additional enable files.

## Update a downloaded release

Close the game and back up your existing `xinput1_4.dll` and `crml` folder. Copy the new release's contents into the same game folder, merge the `crml` folders, and replace CRML's files when Windows asks. Keep other installed mod folders. Check that the new runtime version is supported by your installed mods before updating them.

If an existing `xinput1_4.dll` belongs to another loader, do not overwrite it with CRML. Two loaders using that filename cannot be installed together by copying both packages.

## Remove a downloaded release

Close the game and remove CRML's `xinput1_4.dll` from beside the executable. This stops the loader from starting. Remove `crml` too if you no longer need its mods, settings or logs. No original game files need to be restored.

To remove an individual mod, follow its removal instructions. A mod may include additional settings or enable files beside its folder in `crml/mods/`. Leave the runtime and other mods in place.

## If it does not load

Check the folder layout above first. After starting the game, `crml/crml.log` records loader activity. See [troubleshooting](troubleshooting.md) if no log appears or a mod does not start. The release's `compatibility.json` lists its executable profiles. Trying an unrecognized build does not guarantee gameplay support; a game update may require a new CRML release.

## Building from source

Contributors who want to compile CRML or use the developer installer should use [Building CRML](building.md). Those tools are not needed for release ZIPs.
