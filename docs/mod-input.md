---
description: Rebind Wasm actions, inspect held keys and conflicts, and handle focus and keyboard availability in CRML mods.
---

# Action bindings and input state

Development builds after Alpha 4.3 let Wasm mods change their own action bindings
while running. Declare `input.actions` and
`min_runtime=0.1.0-alpha.4.3.dev.0` when importing `input_bind` or `input_read`.
Existing `input_actions()` calls use the updated bindings too.

Each mod has 16 slots, initialized from `action.0` through `action.15` in
`mod.ini`. These are held-key observations, not engine action names or an event
queue. Reading a key does not consume another mod's press. Bindings do not
inject input, change the game's control settings, or suppress its normal input.

## Rebinding

`input_bind(slot, name, length)` copies a case-sensitive key token. The length
excludes a NUL terminator. Supported tokens are `A` through `Z`, `0` through `9`
(the number row), `F1` through `F12`, `Insert`, `Home`, `End`, `PageUp`, `PageDown`,
`Space`, `Ctrl`, `Shift`, `Up`, `Down`, `Left`, `Right`, and `None`. `None`
unbinds a slot. Escape and system-key combinations are not bindable. Tokens
identify virtual keys, not physical scan codes, text characters or mouse buttons.

| Result | Meaning |
| --- | --- |
| `1` | Binding changed immediately |
| `0` | Already bound to that token |
| `-1` | Binding registry unavailable for this mod |
| `-3` | Invalid slot or unsupported token; existing binding retained |
| `-5` | Revision exhausted; existing binding retained |

Slots are 0–15. Length must be 1–11 bytes; invalid lengths and guest memory
ranges trap before access. Rebinding has a separate limit of 16 calls per
invocation, including rejected and unchanged requests. This allows all 16
slots to be configured at initialization without spending gameplay commands.
Updating a binding
does not change another mod, remove a native shortcut or alter an active movement
lease's fixed suppression keys.

Bindings reset to the manifest when the package next loads. Use [mod storage](mod-storage.md)
for persistent preferences and [settings](mod-settings.md) for configuration UI;
the guest chooses its schema and when to apply a saved key. There is no automatic
key-capture widget or implicit persistence.

## Copied state

`input_read(&state, sizeof(state))` requires `crml_input_state` from `crml.h`.
It consumes one observation call. `1` means a copied binding snapshot, including
when no keyboard provider exists; `-1` means the registry is unavailable and the
output is zeroed. Incorrect size, missing memory and out-of-bounds ranges trap.
Unaligned output is supported. No pointers or native key codes cross this API.
The `input.actions` availability bit describes the keyboard provider; it can be
clear while the permission-gated binding imports remain usable for configuration.

The 232-byte version-1 layout uses little-endian values:

| Offset | Field | Meaning |
| --- | --- | --- |
| 0, 4 | `size`, `version` | `232`, `1` |
| 8 | `flags` | Provider/context flags below |
| 12 | `held` | Currently held action slots |
| 16 | `bound` | Slots with a non-`None` key |
| 20 | `duplicate` | Slots sharing a key within this mod |
| 24 | `shared` | Slots sharing a key with another attached CRML mod |
| 28 | `host_shortcut` | Slots using a potential native shortcut |
| 32 | `binding_revision` | `uint64_t`, initially 1; increases on local changes |
| 40 | `names[16][12]` | NUL-terminated key tokens, with zero padding |

Masks use bits 0–15. Bindings and conflict masks describe the registry at query
time. Later-loading mods are not visible before attachment. Failed initialization,
faults and unload remove a mod's entries; explicit `release()` keeps its bindings.
The local revision does not change when another mod loads, rebinds or unloads,
so do not use it to cache the `shared` mask. None of these values identify a save,
player or world.

| Flag | Meaning |
| --- | --- |
| `CRML_INPUT_AVAILABLE` | Installed keyboard observation service |
| `CRML_INPUT_CONTEXT_KNOWN` | Provider reports focus, freshness and emergency state |
| `CRML_INPUT_FOCUSED` | Foreground window belongs to the game process |
| `CRML_INPUT_FRESH` | Keyboard sample is within the provider's freshness window |
| `CRML_INPUT_EMERGENCY` | Escape held in the current keyboard sample |

The native provider uses a 500 ms freshness window. If availability is absent,
or known context reports missing focus, stale input or Escape, `held` is zero.
A provider without context reporting leaves `CONTEXT_KNOWN` clear; unset focus
bits then mean unknown, not confirmed focus loss. In development builds after
Alpha 4.4, native `input_read` copies held keys, Escape and freshness from one
keyboard publication. Cache contention or a foreground-window change during
the copy makes the sample unusable. A masked zero is not a confirmed release:
require usable context before resetting a hotkey latch. This is an OS input
observation, not an engine-frame transaction; focus may change after it returns.
Separate imports such as `input_motion` and `input_read` remain independent
observations and must not be combined to infer that a masked key was released.

These flags do **not** identify gameplay, menus, text entry, loading, controller
input or the UI's input owner. A focused Options menu is still focused. Mods must
not infer permission to suppress controls or change gameplay from focus alone.

## Conflicts and emergency controls

Shared bindings are advisory, not exclusive grants: both mods can observe a key.
The registry cannot inspect bindings owned by the game or native third-party
mods. It does not resolve conflicts automatically.

`host_shortcut` conservatively flags F6 and F7 (legacy noclip/visibility poll
controls), F11 (native physics cancellation) and Insert (runtime panels). It does
not claim that each associated service is active. Binding a slot to `None` cannot
disable these native controls. Choose a different key when that behavior would
conflict with the mod.

Escape and focus-loss recovery remain native policy. The movement bridge still
suppresses its fixed WASD, Space, Ctrl and Shift controls while its lease is active.
Rebinding a movement mod does not remap those suppressed controls. A generalized
guest suppression API is not available in this build.

## Avoid synthetic press edges

Treat the first usable sample after startup, a local rebind, focus loss, stale
input or Escape as a new baseline. Otherwise a held key can look like a new press.
For example, this helper only emits edges while context is known and usable:

```c
static uint64_t revision;
static uint32_t previous;
static int primed;

static uint32_t pressed_actions(void) {
    crml_input_state state;
    const uint32_t required = CRML_INPUT_AVAILABLE | CRML_INPUT_CONTEXT_KNOWN |
                              CRML_INPUT_FOCUSED | CRML_INPUT_FRESH;
    if (crml_input_read(&state, sizeof(state)) != 1 ||
        (state.flags & required) != required ||
        (state.flags & CRML_INPUT_EMERGENCY)) {
        primed = 0;
        return 0;
    }
    uint32_t pressed = primed && revision == state.binding_revision
        ? state.held & ~previous : 0;
    previous = state.held;
    revision = state.binding_revision;
    primed = 1;
    return pressed;
}
```

Very brief presses can still be missed between worker callbacks. This API does
not turn polling into a lossless input-event stream.
