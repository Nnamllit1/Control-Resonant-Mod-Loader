# Input actions

A Wasm example for CRML Alpha 4.2 or later. It queries capability availability and writes one log line per press of two configured actions. It does not require a gameplay-mode marker or manipulate the player.

Build `input-actions.c` with `python tools/mod.py build examples/input-actions --output mods/input-actions` from the SDK or repository root. Place the built folder in `crml/mods/input-actions/`. The WAT file remains a low-level reference fixture.

With the game focused, F10 writes `Input action 0 pressed` and F12 writes `Input action 1 pressed` to `crml/crml.log`. Holding a key does not repeat the line. There is no on-screen panel or notification sound. The standalone host logs `Input actions unavailable` and returns zero input; it does not read desktop keys.

To rebind, close the game and change `action.0` or `action.1` in `mod.ini`, then restart. Set an action to `None` to disable it. Actions are local to each mod; reading one does not consume another mod's input. Shared keys can still trigger both mods, so choose bindings that suit the installed mods.

Development builds also support live guest rebinding and copied conflict/context information through `input_bind` and `input_read`. See [Action bindings and input state](../../docs/mod-input.md). This basic example retains the older import path for release compatibility.

Input is a held-state snapshot. The example derives press edges in guest code and ignores its initial held state. Focus loss and Escape return zero. Returning to the game with a key held may produce a new edge; mods with persistent gameplay toggles should apply their own cancellation and rearming policy.
