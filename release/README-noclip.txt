CRML - Noclip & Free Flight
Experimental preview - keyboard controls

Fly around the level, pass through geometry, and look at places the normal camera
doesn't show you. This moves the player; it is not a detached free camera.

Install
No compiling or developer tools needed. For a first installation, download the
crml-noclip-bundle ZIP: it includes both the mod and the required runtime.
The smaller crml-noclip ZIP needs the matching crml-runtime ZIP installed first.

1. Close the game. Right-click the downloaded ZIP and choose Extract All.
2. In your Steam Library, right-click CONTROL Resonant, then choose
   Manage > Browse local files. Find CONTROLResonant.exe.
3. Copy the extracted folder's contents into that game folder, not the outer
   download folder. Merge the crml folder with an existing CRML installation.
   The bundle also puts xinput1_4.dll beside CONTROLResonant.exe.
4. Start the game through Steam as usual, or double-click CONTROLResonant.exe
   with Steam running. Load a playable save and press F6 to start flying.

After installation, crml/mods/movement/movement.wasm and
crml/movement-wasm.enabled should be inside the game folder. The folder is named
movement because that is the included Noclip & Free Flight mod's package name.
For runtime updates and removal, see README-CRML.txt in the runtime or bundle.

Use this with ordinary CRML startup, not the separate physics/observer trial modes.
If upgrading a development installation, remove its conflicting diagnostic markers
first. Do not install the legacy noclip example alongside this movement example.

Controls
F6             Flight on/off (starts off)
W A S D        Move along the camera's horizontal heading
Space / Ctrl   Up / down
Shift          Fly faster
Insert         Hide/show the panel; flight continues while it is hidden
Esc            Stop flight
Alt-Tab        Also stops flight when the game loses focus

Return above solid ground before switching flight off. Avoid saving outside the
playable area. Teleports, loading and player replacement can stop flight; press F6
again after normal gameplay resumes. Camera collision may still move the view
around geometry. Some recovery/transition behaviors remain controlled by the game.

The panel's visibility is remembered for the current game session. Hiding it does
not disable the mod. The UI is part of CRML's runtime; movement controls live in
the included Wasm mod. Controller flight bindings are not included.

Removal
Close the game. Remove crml/movement-wasm.enabled and crml/mods/movement.
Other CRML mods remain installed. To stop CRML entirely, remove its xinput1_4.dll.

Compatible game fingerprints are listed in compatibility.json. Unsupported game
updates need a matching CRML release; do not bypass the compatibility check.

Built with CRML. Write your own mods: https://crml.nnamllit.de/developing/
Source and releases: https://github.com/Nnamllit1/Control-Resonant-Mod-Loader
