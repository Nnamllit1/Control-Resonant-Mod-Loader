---
title: Wasm mod API and package format
description: Reference for CONTROL Resonant Wasm mod manifests, lifecycle functions, host imports, capabilities, and runtime limits.
---

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
| `capabilities` | Empty/omitted, or a comma-separated list of `log`, `input.buttons`, `player.noclip`, `player.visibility`, and `physics.damping`; duplicates and unknown requests are rejected |

Logging is available when requested. The experimental noclip import also requires native build support, a matching game fingerprint, and installation opt-in; declaring the capability cannot bypass those gates.

## Guest exports

| Export | Wasm signature | Required |
| --- | --- | --- |
| `crml_abi_version` | `() -> i32`, returns `1` | Yes |
| `crml_init` | `() -> ()` | Yes |
| `crml_tick` | `(f32 elapsed_seconds) -> ()` | No |
| `crml_shutdown` | `() -> ()` | No |
| `memory` | wasm32 linear memory | For logging |

All callbacks run serially on a runtime worker. In-game ticks are approximately 100 ms apart, or 10 ms in the isolated physics service, and elapsed time is capped at one second. They are not render callbacks or a game-thread scheduling guarantee.

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

See [the visibility example](visibility.md) for installation and controls.

## Experimental prop damping

Requires `physics.damping` and the isolated `--physics-wasm` installation mode. This service supports one nearby eligible prop and one mod owner at a time. It reuses the native [physics trial's selection and property checks](physics-trial.md), including player/attachment exclusions, full body generations, executable/backend fingerprints, native accessor checks and readback. It does not enable noclip or visibility hooks.

| Import in `crml_v1` | Wasm signature | Result |
| --- | --- | --- |
| `physics_select` | `() -> i32` | Queue a nearest-prop search within two world units |
| `physics_target` | `() -> i64` | Current owner-scoped selection token, or zero |
| `physics_apply` | `(i64 token, f32 damping, i32 duration_ms) -> i32` | Queue temporary linear damping |
| `physics_status` | `() -> i32` | Current operation state |
| `physics_restore` | `() -> i32` | Request restoration/cancellation and release ownership |

Command results are `0` accepted (or already idle for restore), `-1` unavailable, `-2` busy or another owner, and `-3` invalid/stale target or arguments. Acceptance is not execution: the engine callback consumes requests. Requests older than 500 ms are not applied. The guest must wait for selection to complete before obtaining a token and applying a value. A token is neither an engine address nor an entity/body ID; passing another mod's token does not grant access.

`physics_status` returns `0` idle, `1` queued, `2` searching, `3` selected, `4` active, `5` restoring, `6` finished, `7` retired, `8` conflicting game change, or `9` refused. It can return `-1` unavailable or `-2` busy. Finished includes a value already equal to the request; it does not by itself prove a write occurred. Terminal status is transient and returns to idle when the selection is cleared. A failed search also returns to idle, with its reason shown in the panel and diagnostic log.

The import accepts finite damping from **0 through 8**, for **1 through 5,000 ms**. Nonfinite/out-of-range values or exceeding the shared eight-call gameplay budget trap the mod. Native code independently checks these limits. Unused selection expires after 15 seconds; completing a trial consumes its token. A new application requires a fresh selection. Damping affects linear velocity decay, not friction, mass or angular damping.

Focus loss, Escape, F11, the worker/session deadline, mod failure, shutdown and runtime destruction request cleanup. Cleanup is serviced on the physics callback, including after the guest has stopped. A game-written conflicting value is preserved; retired bodies are not written through stale handles. Temporary unavailability keeps restoration pending. The service retains the diagnostic session's ten-minute/four-MiB limits; restart the game to begin a new session after a limit is reached.

See the [Wasm damping example](physics-trial.md#wasm-damping-example) for setup and expected behavior.
