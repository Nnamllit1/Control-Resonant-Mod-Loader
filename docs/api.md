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
