---
description: Fix common CONTROL Resonant Mod Loader installation problems, version mismatches, and mod loading errors.
---

# Troubleshooting

Start with the [installation guide](installation.md) if you are unsure which download to use. Close the game before replacing or removing files.

## Which download do I need?

For playing with CRML mods, download **`crml-runtime-<version>-windows-x64.zip`**. Install your chosen mod separately, following its instructions. The SDK and Source code downloads are for development.

The runtime alone does not add a general mod menu or gameplay controls. Check `crml/crml.log` after starting the game to confirm startup, then follow your mod's instructions for using its features.

## No in-game log

After launching the game, look for `crml/crml.log` in the game folder. If it is absent:

1. Find the game folder through your launcher and check that `xinput1_4.dll` is directly beside `CONTROLResonant.exe`. In Steam, use **Manage > Browse local files**.
2. Check that `crml/crml_runtime.dll` and `crml/wasmtime.dll` are present. An extra folder named after the ZIP should not sit between these files and the game.
3. Confirm that you installed `crml-runtime-<version>-windows-x64.zip`, not only a mod or the SDK.
4. Confirm that you extracted the ZIP before copying its contents, then start the game normally through your usual storefront or launcher.

If the layout is correct and no log appears, [report the problem](#report-a-problem) with the release filename and any Windows error message.

## An existing proxy blocks installation

Another loader may also use a file named `xinput1_4.dll`. CRML cannot share that filename with it. Follow the other loader's removal instructions before installing CRML; do not rename its DLL or overwrite it to try to combine the two.

## Shortcuts or gameplay break with another loader installed

Running CRML alongside another mod loader, including a native DLL mod loader, is not recommended. Compatibility has not been verified or guaranteed. It may work normally, but the loaders or their mods can interfere with input and gameplay or cause crashes. Different proxy DLL filenames do not guarantee compatibility.

To check whether the combination is involved, close the game and move **CRML's** `xinput1_4.dll` out of the game folder, then launch again. This disables CRML and its mods. If the other mods' shortcuts work again, include both loaders' versions, the affected mods and hotkeys, and `crml/crml.log` in your report. Keep a copy of the log before another launch.

## A game update changes the fingerprint

From Alpha 4.1, an unrecognized executable opens a CRML-owned Windows prompt before any mods or gameplay hooks start. Choose **Continue with CRML** to try it, or **Leave CRML disabled** to play without CRML mods. The checkbox remembers either choice for that exact executable. Close the game and remove `crml/compatibility-choice.txt` to reset it; a changed executable requires a new choice. Changing `compatibility.json` does not grant approval or adjust engine offsets.

From Alpha 4.3, the prompt includes **Check for CRML updates on GitHub**. This opens the [releases page](https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases) without approving mods or closing the prompt. Check for a newer runtime after a game update, then close the game before installing it. Downloading the same CRML version again does not add support for a changed game build; if no compatible update is available, leave CRML disabled.

Approval does not establish compatibility. Changed or unreadable hook signatures still refuse the affected feature, and physics integrations retain their backend checks. Send `crml/crml.log` with the exact game version and storefront if a feature remains unavailable. A different edition or storefront alone does not tell us whether its engine layout matches.

Check each mod's stated CRML requirements when choosing a runtime version. A mod update may be needed after updating CRML.

## A mod is rejected or disabled

Open `crml/crml.log` in a text editor and look for the mod's name and error. Check that its folder contains both `mod.ini` and the `.wasm` file named by the manifest, and that the mod supports your CRML version.

Some mods require a gameplay mode that cannot run alongside another mod's mode. Follow the mod author's compatibility instructions. If one mod still fails, close the game, move that mod's folder out of `crml/mods`, and restart to check the others. Keep a copy of the log before restarting.

For errors in a mod you are writing, see the [mod development guide](developing.md).

## A mod loads, but its feature does not work

Check the mod's instructions for controls, supported game versions and any required settings. Some features are available only after loading a playable save. A successful loader startup does not by itself confirm that every mod feature is available.

If the problem affects one mod, report it to that mod's author with its version and any related log message. For a CRML startup or compatibility problem, [report it to CRML](#report-a-problem).

## Remove CRML to check a startup problem

Close the game and move CRML's `xinput1_4.dll` out of the game folder. Start the game again. This prevents CRML from loading and leaves the `crml` folder available for checking logs. See [removal instructions](installation.md#remove-a-downloaded-release) to uninstall it completely.

## Report a problem

Open a [GitHub issue](https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/issues) with:

- The CRML release or ZIP filename, your game version, and installed mods.
- What you did, what you expected, and what happened instead.
- Any error message or a screenshot showing the problem.
- `crml/crml.log` and any additional logs requested by the mod author.

Copy the logs **before starting the game again**: a new session replaces them. Review files and screenshots before sharing them and remove personal information.

Contributors investigating CRML itself can use the [building guide](building.md) and [architecture reference](architecture.md).
