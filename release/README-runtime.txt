CONTROL Resonant Mod Loader (CRML)
Experimental preview - Windows x64

Install - no compiling or developer tools needed
1. Close CONTROL Resonant.
2. Right-click the downloaded ZIP in Windows and choose Extract All.
3. Find the folder containing CONTROLResonant.exe using your launcher.
   In Steam: right-click CONTROL Resonant > Manage > Browse local files.
4. Open the extracted ZIP folder. Copy its contents into that game folder.
   Copy the contents, not the outer folder named after the download.
5. Start CONTROL Resonant through your normal storefront or launcher.

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
files when Windows asks. Keep your other mod folders. Existing ABI 1 mods remain
compatible; mods using new imports need the runtime version specified by their
author. A manually copied installation does not need the Python installer.

If an existing xinput1_4.dll belongs to another loader, do not replace it with
CRML. Two loaders using that same filename cannot be installed together this way.

Compatibility
Running CRML alongside another mod loader, including a native DLL mod loader,
is not recommended. Compatibility has not been verified or guaranteed. It may
work, but shortcuts or gameplay can break, and crashes are possible even when
the loaders use different DLL filenames. Use one loader at a time.

An unrecognized executable opens a CRML Windows prompt before mods start.
Use Check for CRML updates on GitHub to open the releases page. The prompt stays
open and mods remain disabled until you choose to continue. Close the game before
installing a newer runtime. Reinstalling the same version does not add support
for a changed game build; a newer compatible release may not yet be available.
Choose Continue to try it or Leave CRML disabled to play without CRML mods.
The checkbox remembers either choice for that exact executable. Remove
crml/compatibility-choice.txt with the game closed to reset it.
Hook signatures are still checked; approval does not guarantee compatibility.
Mods use a bounded Wasm API; arbitrary native mod DLLs are not supported.
Direct engine Lua mods are not part of this release.

Removal
Close the game and remove this package's xinput1_4.dll to stop loading CRML.
Remove the crml folder only if you no longer need any installed mods or logs.
No original game files are replaced.

Troubleshooting
Check crml/crml.log after starting the game. The previous three runtime sessions
are kept as crml/crml.1.log through crml/crml.3.log (newest first).
If the runtime cannot load, install
Microsoft's current Visual C++ x64 Redistributable from Microsoft's website.
Include the CRML version and game build when reporting a problem; review logs
before sharing them.

Installation guide: https://crml.nnamllit.de/installation/
Documentation: https://crml.nnamllit.de/
Source and releases: https://github.com/Nnamllit1/Control-Resonant-Mod-Loader
