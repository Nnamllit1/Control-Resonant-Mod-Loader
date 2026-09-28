CONTROL Resonant Mod Loader (CRML)
Experimental preview - Windows x64

Install
1. Close CONTROL Resonant.
2. Open the game's installation folder through Steam: Manage > Browse local files.
3. Copy xinput1_4.dll and the crml folder beside CONTROLResonant.exe.
4. Start the game normally through Steam.

This package includes the runtime and a small hello mod. Install the separate
Noclip & Free Flight package to enable flight. The SDK archive is for mod authors;
its tools and examples do not need to be copied into the game directory.

If xinput1_4.dll or a crml folder already exists, do not overwrite it blindly.
Keep a backup of your existing CRML installation and custom mods. Do not combine
this proxy with another mod loader that uses the same DLL filename.

Compatibility
The gameplay bridge checks the exact executable fingerprint in compatibility.json.
Unknown game builds refuse gameplay support. A game update may require a new CRML
release. Mods use a bounded Wasm API; arbitrary native mod DLLs are not supported.
Direct engine Lua mods are not part of this release.

Removal
Close the game and remove this package's xinput1_4.dll to stop loading CRML.
Remove the crml folder only if you no longer need any installed mods or logs.
No original game files are replaced.

Troubleshooting
Check crml/crml.log after starting the game. If the runtime cannot load, install
Microsoft's current Visual C++ x64 Redistributable from Microsoft's website.
Include the CRML version and game build when reporting a problem; review logs
before sharing them.

Documentation: https://crml.nnamllit.de/
Source and releases: https://github.com/Nnamllit1/Control-Resonant-Mod-Loader
