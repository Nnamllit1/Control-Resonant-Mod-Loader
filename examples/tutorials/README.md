# Native tutorials

A Wasm example requesting the game's tutorial hint, prompt and panel renderers. Requires
a runtime built with gameplay services enabled with the `tutorials` service available.

Build with `python tools/mod.py build examples/tutorials --output build/mods/tutorials`.
Copy the resulting package into `crml/mods/tutorials/` with the game closed,
then load a playable save.

- **F9:** display a hint for six seconds, followed by the native fade.
- **F10:** open a panel; use the game's Continue control to dismiss it.
- **PageDown:** show a six-second bottom prompt with native progress and the
  game's current `CLOSE_TUTORIAL` dismiss action. Use the on-screen binding or
  let the timer finish; it does not advance campaign tutorials.
- **End:** cancel this mod's outstanding request.

These are action slots in `mod.ini`; change them if another mod uses those keys.
One request may be outstanding per mod. Status messages are written to CRML's
session log. The example does not open anything automatically at startup.

The hint and panel request an existing game UI image through `crml_tutorial_present`.
F9 places it above the hint body; F10 places it below the panel body. Both use
80 percent of the content width and a two-vh gap. Change the versioned page
constants in `tutorials.c` to choose placement, alignment, width, height and
spacing. Image and text remain separate blocks.

The PageDown action submits a version-2 page with `NATIVE_DISMISS` and
`PROGRESS` options. It has no image. PageDown is only the example's trigger;
the prompt's dismissal binding is supplied by the game and may be different.
Hints and prompts share the dynamic toast surface, so the example waits for
the previous request to retire before opening another.

No game image is distributed with this example. Custom image-file loading is
not provided. A presentation ticket confirms tutorial state, not image decoding.

The offline host compiles and loads the example, but reports native tutorials
unavailable. Native appearance and input restoration require an in-game check.
See [the API contract](../../docs/mod-tutorials.md).
