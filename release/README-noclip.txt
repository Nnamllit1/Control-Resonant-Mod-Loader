CRML - Noclip & Free Flight
Experimental preview - keyboard controls

Fly around the level, pass through geometry, and look at places the normal camera
doesn't show you. This moves the player; it is not a detached free camera.

Install
1. Install the matching CRML runtime from GitHub, unless using the bundled archive.
2. Close the game. Copy this package's crml folder into the game installation
   folder, beside CONTROLResonant.exe. For the bundled archive, also copy
   xinput1_4.dll there. Start through Steam as usual.
3. Load a playable save. Press F6 to start flying.

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
