CRML developer SDK

Extract outside the game folder. Open PowerShell in this extracted directory.
Python 3.11+ is required for the author commands.

1. Get the pinned C compiler (first use only, approximately 543 MB download):
   python tools/mod.py toolchain
   Or pass --clang <path-to-clang.exe> to build with an existing wasm32 compiler.

2. Create and build a mod:
   python tools/mod.py new source/my-mod --id my-mod
   python tools/mod.py build source/my-mod --output mods/my-mod

3. Check it and run standalone ticks:
   python tools/mod.py check mods/my-mod
   python tools/mod.py test mods/my-mod --ticks 20

Edit source/my-mod/main.c and rebuild to change behavior. Maintained examples
also build with tools/mod.py build examples/<name> --output mods/<name>.
C is the normal example build input. WAT files remain low-level reference
fixtures; editing a WAT file does not change a normal C build.

check executes module initialization and shutdown inside the sandbox; test also
runs ticks. This standalone host has no game, keyboard or engine services.
It checks loading and unavailable-service handling, not gameplay behavior.
