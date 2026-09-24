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
| `capabilities` | Empty/omitted, or exactly `log`; other requests are rejected |

There is currently one grantable capability. The host's policy allows logging when requested; declaring an arbitrary capability does not grant it.

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

No gameplay or UI imports are available yet. The [gameplay milestone](gameplay.md) describes the intended next bridge without reserving an unverified ABI.
