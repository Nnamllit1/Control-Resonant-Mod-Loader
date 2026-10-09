---
description: Save bounded mod-owned data through the Wasm SDK without filesystem access.
---

# Mod persistence

Development builds after Alpha 4.3 provide the experimental `storage` capability.
It stores one binary record of up to 64 KiB per manifest ID. Data belongs to the
installation and mod ID, **not a campaign, player, save slot or entity generation**.
Use this for preferences or explicitly installation-wide history. Campaign-aware
state needs an independently established campaign identity.

The inspected native save-container names include a slot-derived name. Reusing a
slot does not give it a new identity suitable for mod history. Do not infer
campaign scope from that name, map geometry, or player continuity. The
[save-context research](engine-paths.md#save-and-load) describes the available
evidence and its limits.

```ini
id=my-mod
abi=1
module=my-mod.wasm
min_runtime=0.1.0-alpha.4.3.dev.0
capabilities=log,storage
```

Imports are declared in `crml.h`. Older runtimes reject this capability. Check
`crml_capabilities() & CRML_CAP_STORAGE` before offering persistent features:
blocked directories or another process using the same store can make it unavailable.
Storage does not require gameplay hooks or a supported game fingerprint.

## Read and write contract

| Import | Wasm signature | Result |
| --- | --- | --- |
| `storage_read` | `(i32 output, i32 capacity) -> i32` | Committed byte count, including zero; negative error |
| `storage_write` | `(i32 bytes, i32 length) -> i32` | `0` accepted; negative error |
| `storage_status` | `() -> i32` | `0` no write requested, `1` pending, `2` committed, `-5` failed, `-1` unavailable |

| Error | Meaning |
| --- | --- |
| `-1` | Storage is unavailable |
| `-2` | No saved record (`storage_read` only) |
| `-3` | Output capacity is too small (`storage_read` only) |
| `-4` | A write is pending, or the one-second write interval has not elapsed |
| `-5` | I/O failure or invalid saved file; reads leave the output unchanged |

Invalid memory ranges and lengths over 65,536 bytes trap the mod before accessing
storage. These imports share a separate limit of 16 calls per guest invocation;
they do not consume the gameplay observation or command allowances. Capability queries still
use the observation allowance. Each mod may submit at most one write per wall-clock
second and have one pending write. Coalesce edits and save on meaningful changes.

The host copies write input before returning, then writes on a separate storage
worker. `0` means accepted, **not saved**. Poll `storage_status` on later ticks;
never spin waiting within a callback. Terminal status stays available until the
next accepted write. Rejected requests do not replace it. Reads during a pending
write return the previous committed record, or `-2` if none exists. Failed writes
preserve that record. Reads do not modify unused output bytes.

Writing zero bytes stores an empty record; it does not make the record absent.
Use a versioned guest encoding so empty, missing and incompatible data have clear
meanings. Validate every decoded field before using it. A corrupt record returns
`-5`; it is not silently reset. An explicit new write may replace it.

## Example: save a preference

This fragment assumes the mod already provides the required lifecycle exports.
It uses a fixed eight-byte encoding with a schema version and a Boolean. A real
mod should read and validate its saved record during initialization, report save
errors to the player, and retry rate-limited changes on a later tick.

```c
#include "crml.h"

static int saving;

static int save_enabled(int enabled) {
    uint32_t record[2] = {1, enabled ? 1u : 0u};
    int result = crml_storage_write(record, sizeof(record));
    if (result == 0) saving = 1;
    return result;
}

static void observe_save(void) {
    if (!saving) return;
    int result = crml_storage_status();
    if (result == 1) return;
    saving = 0;
    if (result == 2) crml_log(1, "Preference saved", 16);
    else crml_log(3, "Preference save failed", 22);
}
```

## Lifetime, files and testing

In the game, records live in `crml/data`. The standalone `crml_host` uses a `data`
directory beside the supplied mods directory. Use a temporary mods directory when
testing persistence. The deterministic scenario simulator currently supplies no
storage service; it reports the capability unavailable and returns `-1`.

Removing a mod does not remove its data. Reinstalling it with the same ID can read
the record; changing the ID creates a different namespace. IDs provide isolation
between loaded guests, not authenticated publisher identity. Installing a package
with another package's ID gives it that ID's data. Duplicate IDs cannot load
together. Do not store secrets here.

Accepted writes survive guest traps, unload and shutdown. Normal host destruction
drains the write queue. Abrupt process termination can lose uncommitted writes;
there is no shutdown-time durability guarantee after a crash. Successful completion
means the host flushed a staging file and replaced the committed entry. Filesystem
and hardware failures still apply; this is not a game-save transaction.

Each ID has one committed file and at most one staging file. An interrupted staging
file is safely replaced by the next write for that ID. The checksum detects accidental
corruption, not hostile modification. The store rejects linked directories and
linked record reads; guests receive no path-selection or arbitrary filesystem API.
One process owns a storage root at a time. Retained records from uninstalled mods
are not automatically pruned; the 32-ID runtime limit is not an installation-wide
disk quota. Back up or remove an individual mod's data only with the game closed.

Automated coverage exercises actual disk replacement and separate host-process
restarts. It does not establish campaign identity, settings UI behavior or
power-loss guarantees.
