# List browser

This Wasm example publishes a native Mods-page list with search, paging, selection
and adjustable text size. It uses 40 local sample words; it does not inspect or
change game equipment, dialogue or saves.

Requires runtime `0.1.0-alpha.4.4.dev.1` or later. Build with
`python tools/mod.py build examples/list-browser --output build/mods/list-browser` and install the resulting mod
folder beneath `crml/mods/`.

In **Options → Mods → List browser**, edit **Search** and apply it to filter all
entries. Search is case-insensitive for ASCII sample words. Change **List text size**
to scale the list independently of settings controls. Click an entry to select it,
or use the list's **Previous entries** and **Next entries** rows. Disabled page
controls cannot enqueue actions. Selection is temporary and does not persist.

All filtering, paging and selection logic is in `list-browser.c`. The host knows
only a copied page with stable row IDs and revision-checked actions. The example
consumes at most four actions per tick and rejects actions from obsolete views.
It retries unavailable/rate-limited publication and clears its page at shutdown.

The native engine's text entry and list interaction require in-game qualification;
offline Wasm/browser tests do not establish those behaviors inside Cohtml.
