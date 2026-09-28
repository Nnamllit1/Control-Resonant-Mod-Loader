CONTROL Resonant Mod Loader (CRML)
Experimental preview - Windows x64

Install - no compiling or developer tools needed
1. Close CONTROL Resonant.
2. Right-click the downloaded ZIP in Windows and choose Extract All.
3. In your Steam Library, right-click CONTROL Resonant, then choose
   Manage > Browse local files. Find the folder containing CONTROLResonant.exe.
4. Open the extracted ZIP folder. Copy its contents into that game folder.
   Copy the contents, not the outer folder named after the download.
5. Start CONTROL Resonant through Steam as usual, or double-click
   CONTROLResonant.exe in that folder. Keep Steam running.

The files should look like this after copying:

  CONTROLResonant.exe       (already there - the game)
  xinput1_4.dll             (from this download)
  crml/
    crml_runtime.dll
    wasmtime.dll
    mods/

Do not put these files in Documents, your save folder, or another folder inside
the game folder. CRML loads automatically when the game starts; there is no
separate CRML application to open. Individual mods provide gameplay features
and any menus or controls they need.

This package includes the runtime. Add compatible mods using the instructions
supplied with each mod. Mod folders belong in crml/mods.
The SDK archive is for mod authors; its tools and examples do not need to be
copied into the game directory.

Updating an existing CRML installation
Close the game and back up xinput1_4.dll and the crml folder. Copy the new runtime
contents into the same game folder, merge the crml folders and replace the CRML
files when Windows asks. Keep your other mod folders. Use matching runtime and
mod releases. A manually copied installation does not need the Python installer.

If an existing xinput1_4.dll belongs to another loader, do not replace it with
CRML. Two loaders using that same filename cannot be installed together this way.

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

Installation guide: https://crml.nnamllit.de/installation/
Documentation: https://crml.nnamllit.de/
Source and releases: https://github.com/Nnamllit1/Control-Resonant-Mod-Loader
