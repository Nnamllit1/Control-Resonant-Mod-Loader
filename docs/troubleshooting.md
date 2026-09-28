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

1. Use Steam's **Manage > Browse local files** to check that `xinput1_4.dll` is directly beside `CONTROLResonant.exe`.
2. Check that `crml/crml_runtime.dll` and `crml/wasmtime.dll` are present. An extra folder named after the ZIP should not sit between these files and the game.
3. Confirm that you installed `crml-runtime-<version>-windows-x64.zip`, not only a mod or the SDK.
4. Confirm that you extracted the ZIP before copying its contents, then start the game normally through Steam.

If the layout is correct and no log appears, [report the problem](#report-a-problem) with the release filename and any Windows error message.

## An existing proxy blocks installation

Another loader may also use a file named `xinput1_4.dll`. CRML cannot share that filename with it. Follow the other loader's removal instructions before installing CRML; do not rename its DLL or overwrite it to try to combine the two.

## A game update changes the fingerprint

CRML checks the game version before enabling gameplay features. If a game update is not supported, install a CRML release that explicitly supports it. Changing `compatibility.json` does not make an unsupported version work.

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
