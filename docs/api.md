# Package format and API

## Manifest

`mod.ini` is a strict UTF-8/ASCII-compatible `key=value` file without sections. Blank lines and lines beginning with `#` are ignored. Duplicate and unknown fields are errors.

```ini
id=hello
abi=1
module=hello.wasm
capabilities=log
```

| Field | Contract |
| --- | --- |
| `id` | Required, 1–64 lowercase letters, digits, `_` or `-`; unique within the host |
| `abi` | Required, exactly `1` |
| `module` | Required, local `.wasm` filename; no directories or absolute paths |
| `capabilities` | Empty/omitted, or a comma-separated list of `log` and `player.noclip`; duplicates and unknown requests are rejected |

Logging is available when requested. The experimental noclip import also requires native build support, a matching game fingerprint, and installation opt-in; declaring the capability cannot bypass those gates.

## Guest exports

| Export | Wasm signature | Required |
| --- | --- | --- |
| `crml_abi_version` | `() -> i32`, returns `1` | Yes |
| `crml_init` | `() -> ()` | Yes |
| `crml_tick` | `(f32 elapsed_seconds) -> ()` | No |
| `crml_shutdown` | `() -> ()` | No |
| `memory` | wasm32 linear memory | For logging |

All callbacks run serially on a runtime worker. In-game ticks are approximately 100 ms apart and elapsed time is capped at one second. They are not render callbacks or a game-thread scheduling guarantee.

## Host imports

`crml_v1.log(level: i32, offset: i32, length: i32) -> ()`

Requires `log`. `offset` and `length` describe guest memory, never a native address. Level must be 0–3 (debug/info/warning/error); the current plain-text sink uses the same format for each level. Text should be UTF-8. Control characters are replaced with spaces to keep log entries on one line. Invalid byte sequences are not transcoded.

One call accepts at most 4,096 bytes. Each lifecycle invocation accepts at most 32 calls and 16,384 bytes. Invalid ranges and exhausted logging budgets trap the guest.

### Experimental noclip

`crml_v1.noclip_poll(speed: f32) -> i32`

Requires `player.noclip`. Call on each worker heartbeat to keep the mod's native movement lease alive and service the fixed F6 keyboard toggle. Finite speeds from 0.25 through 20 world units per second are accepted; Shift multiplies movement speed by three. More than eight calls per lifecycle invocation, nonfinite values, and out-of-range speeds trap the guest.

Returns `1` when enabled, `0` when off, `-1` when unavailable, or `-2` when another mod owns the override. The standalone host returns `-1`. Native cleanup releases ownership on load failure, traps, shutdown, and runtime destruction, independently of a guest shutdown export. Guests cannot provide addresses, native callbacks, arbitrary key codes, or entity IDs.

The host handles movement keys and the status panel; this is not a general UI or keyboard API. While the lease is active, it consumes the fixed noclip keyboard controls, suppresses player fall/boundary recovery and fall monitoring, and neutralizes script-visible fall values. Once a flight position is acquired, same-player/world teleports restore that target instead of cancelling flight. This also affects intentional travel; disable noclip first. These overrides end when the lease is released or expires. See the [test guide and limitations](gameplay.md).

## Bounded button input

`crml_v1.input_buttons() -> i32` requires `input.buttons`. Bit 0 is F7, bit 1 is F8; other bits are zero. Returns zero while the game lacks focus, Escape is held, or no input service exists. No arbitrary key codes or text input are exposed. Polling runs on the worker heartbeat, so a press shorter than that interval can be missed.

## Experimental player visibility

`crml_v1.visibility_set(hidden: i32) -> i32` requires `player.visibility` and a native build installed with `--experimental-visibility`. Only 0 and 1 are accepted: 1 renews a 500 ms hide lease; 0 releases it. The **guest decides** when to request hiding, using button input, a timer, or other guest logic. It does not need the input capability if it does not read buttons.

The native runtime validates the request, enforces focus/Escape cancellation and lease expiry, then submits the renderer command on the engine's mesh update phase. It releases ownership on mod failure and shutdown. The host accepts at most eight calls per lifecycle invocation shared across input and gameplay imports.

Returns `1` for a renewed lease, `0` for release, `-1` when unavailable, and `-2` when another mod owns the lease. This is request status, not confirmation of a rendered result. The bridge targets only a fresh, generation-checked player entity in the mesh visibility query. Guests cannot supply a pointer, render handle, entity ID, or renderer opcode. Normal engine visibility resumes on the next eligible update after release or cancellation.

The legacy `visibility_poll()` import remains available for older mods and combines native F7 polling with a visibility lease. New mods should use `visibility_set()` instead.

See [the visibility example](visibility.md). The native renderer operation has been reported working in gameplay; the revised guest-controlled input path still needs live verification.
