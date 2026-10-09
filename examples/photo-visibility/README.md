# Photo visibility

Hide the player's root mesh with **F8** for screenshots. By default, hold F8;
release it to return visibility to the engine. In **Options > Mods > Photo
visibility**, choose activation mode **0 = hold** or **1 = toggle**, disable the
feature, or turn off its notices. In toggle mode, press F8 again to release it.
This is a Wasm guest using the existing visibility, input, settings, storage and
feedback services. It needs no mod-specific native code.

From the repository or SDK root:

```powershell
python tools/mod.py build examples/photo-visibility --output build/mods/photo-visibility
python tools/mod.py check build/mods/photo-visibility
```

The included deterministic scenarios use a mods directory containing only this
package (so unrelated guests cannot change the expected trace):

```powershell
python tools/mod.py build examples/photo-visibility --output build/photo-sim/mods/photo-visibility
python tools/mod.py simulate build/photo-sim/mods examples/photo-visibility/hold-recovery.json
python tools/mod.py simulate build/photo-sim/mods examples/photo-visibility/settings-toggle.json
python tools/mod.py simulate build/photo-sim/mods examples/photo-visibility/lease-refusal.json
python tools/mod.py simulate build/photo-sim/mods examples/photo-visibility/long-stall.json
```

Install a development runtime at least `0.1.0-alpha.4.4.dev.0` with its experimental
gameplay services and native UI panel. Follow the [visibility installation
guide](../../docs/visibility.md) for runtime build and installation options; copy
the built `photo-visibility` package into the game's `crml/mods` directory with the
game closed. The manifest requests `player.visibility` directly; a legacy
`visibility.enabled` marker is not required by the current runtime. The installer's
`--experimental-visibility` option also installs the older F7 example, which is
separate from this package. Do not hold that example's key while using this one.
Native UI and feedback need a supported renderer and the bundled panel; missing
services produce diagnostics, not an alternative UI.

Action slot 0 is the photo key. Change `action.0=F8` in the package's `mod.ini` to
another supported [named key](../../docs/mod-input.md), then reload the package.
There is no key-capture control in this example. Shared-key warnings are advisory;
the key is not consumed, and game bindings or third-party native mods cannot all be
detected. A lease owned by another mod refuses the request and reports a short
notice. Release the key and press again to retry; it does not retry automatically
while held.

Escape, focus loss, stale/unavailable input, an unavailable or older-than-500-ms
player snapshot, a player generation change, a binding revision change, disabling
the mod, or changing its activation mode cancels the request. Starting or resuming
requires an observed key release before a fresh press. Input must report known,
focused, fresh context. Focus alone does not distinguish menus or gameplay: a
focused Options screen may still observe F8. Very short presses between worker
callbacks can be missed.

The guest checks `visibility_read` before renewing its hide lease. If a long
callback gap lets that lease expire, both hold and toggle modes require a key
release followed by a fresh press. The initial notice says that hiding was
requested; a later notice reports the adapter's hidden-mesh observation when
available. Evidence is scoped to the current lease and is not a render-completion
or restoration guarantee. Older native providers without this observation API
cannot keep this example's request active.

Preferences are installation-wide for the `photo-visibility` ID, not associated
with a save, campaign or player. Only enabled/mode/notifications are persisted;
active hiding, bindings, generations and setting handles are never saved. The
guest uses an eight-byte `PVIS`, schema-1 record, validates every field and
coalesces edits before writing. Accepted asynchronous writes are polled for
completion. Failures retry up to three attempts with a delay; another setting edit
allows another attempt. Storage unavailability leaves preferences temporary for
that load. Empty, malformed, unreadable or unknown-schema records are preserved,
and saving is disabled until **Reset preferences** is explicitly activated in
Options. That action restores enabled/hold/notices defaults, replaces even an
unknown or damaged saved record, and resets its own switch. Back up the mod's
record in `crml/data` with the game closed before resetting if it might contain
preferences from a newer version. Unload
attempts one final save when possible, but abrupt termination or an outstanding
write can lose the latest edits. See [storage](../../docs/mod-storage.md).

This changes root-mesh presentation only: it does not provide gameplay
invisibility, hide all equipment/effects, move the camera, or disable the HUD.
`visibility_set(1)` acknowledges a 500-ms lease request, **not a confirmed rendered
result**. The guest renews that lease while active. Release asks the engine to
resume control on its next eligible mesh update; a paused engine can delay this.
Feedback acceptance likewise does not prove a notice was displayed. Warnings also
go to the runtime log even when notices are off.

Retry, debounce and notice deadlines use the host monotonic `clock_ms` snapshot.
Long callback stalls expire old deadlines; notices and settings first observed
after the stall receive their full lifetime or debounce from that observation.
The long-stall scenario checks key and setting recovery; storage and feedback
deadlines require a host harness because the simulator has no persistent store
or feedback publication trace.

For contributors: each tick makes three gameplay observations and at most three
gameplay commands, with separate bounded settings/storage/feedback calls. Host
simulation can check hold/toggle, lease refusal, stale input, focus/Escape,
generation changes, settings edits and rearming without running the game. The
simulator has no persistent store; storage durability needs a standalone host
restart check. These checks do not establish live rendered visibility, native
Options compatibility or gameplay behavior.

Separate gameplay checks on game `0.564.478.0` with runtime
`0.1.0-alpha.4.4.dev.0` exercised hold/toggle visibility, focus recovery, native
settings, visible replacement after save reload, and preference retention across
a game restart. These checks cover this mod's workflow; they do not establish
compatibility with every other mod or engine lifecycle transition.
