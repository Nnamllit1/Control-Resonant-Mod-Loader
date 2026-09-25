# Troubleshooting

## A mod is rejected or disabled

Run `crml_host` against the package directory and read its output. Common causes are a wrong ABI, unknown capability, missing import, invalid callback signature, memory limit, or exhausted fuel. The host exits with status 1 for mod failures and status 2 for host/usage failures.

A tick trap disables only that mod. Remove the package and restart to test the remaining mods. There is no native-DLL fallback.

## No in-game log

Check that `xinput1_4.dll` is beside the executable and that `crml/crml_runtime.dll`, `crml/wasmtime.dll`, and `crml/mods/` exist. The startup route is experimental and only runs when ordinal 2 is called. Missing runtime dependencies or a game build that bypasses this import can prevent startup.

Use the standalone host to distinguish package failures from bootstrap failures. The Windows debug output contains brief loader errors if the trusted runtime cannot load.

## An existing proxy blocks installation

The installer never overwrites another mod's proxy. Resolve ownership of the existing files first. Proxy chaining is not implemented.

## A game update changes the fingerprint

The installer refuses an unrecognized executable. Inspect the new import table and validate it before adding a profile. A new hash by itself is not compatibility testing.

## Noclip still jumps or resets at a boundary

Confirm the panel shows ON and the first line of `crml/movement-probe.jsonl` reports schema 7. If the log reports an unavailable input or fall-recovery hook, noclip stays disabled. Close the game and install the latest experimental build before retrying.

The log includes `input_consumed`, `fall_checks_skipped`, and `boundary_targets_skipped` counters. Report both logs and whether the panel changed to OFF when the problem occurred. A boundary reset after disabling noclip is expected; normal recovery resumes at the current position. Scripted transitions and deaths may still interrupt flight.

If resetting stops but white fog still appears, include `last_stop`, `active_recoveries_skipped`, `fall_camera_overrides`, and `fall_camera_clear_requests` along with the fall-monitor counters. A relocation can cancel noclip between the one-second samples; `last_stop` preserves that reason and the coordinates. The remaining visual effect has not yet been confirmed fixed in gameplay.

If the player is still moved during flight, include `teleport_restores` and `last_override`. Teleport-flagged movement of the same player/world now retains the flight target. A world/player change, an unflagged large displacement, or an expired lease still cancels the override. Disable noclip before intentional teleports or fast travel.

## Shutdown and logs

Close the game before changing DLLs. The bootstrap pins its modules for the process lifetime; unloading them while the worker is running is unsupported. The log is overwritten on each startup and stops growing near its session limit.
