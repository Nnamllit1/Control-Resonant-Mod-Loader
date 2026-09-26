# Observe-only engine validation

The engine observer records selected engine phases and player/resource identities so contributors can investigate update timing and object lifetime. It is the first validation stage following the [static engine atlas](engine-atlas.md). A successful capture supplies evidence for review; it does not establish a safe modification API.

Observation mode suspends all Wasm mods and the existing noclip, visibility, input-filter, and fall-recovery features. It installs version-gated native hooks that forward the selected engine calls and copy diagnostic records. It does not intentionally change gameplay state or call property setters. Hooks still introduce overhead, and live compatibility must be established for the recorded build.

## Build and enable

Build on Windows with the dependencies described in [installation](installation.md):

```powershell
.\build.bat -EngineObserver -Test
```

Close the game, then preview and apply an update to an existing loader installation. Replace the example installation path as needed:

```powershell
python tools/install.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant" --update --engine-observer
python tools/install.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant" --update --engine-observer --apply
```

For a fresh installation, omit `--update`. The installer checks the supported executable, package feature metadata, existing ownership receipt, and whether the game is running. The observer option cannot be combined with other experimental mode selections. Existing mods and settings are preserved but remain suspended while observation mode is enabled.

Start the game normally through Steam. `crml/crml.log` should report **Observe-only engine validation active** and the capture filename. The observer uses automatic logging; it has no in-game panel or hotkey requirement. If initialization fails, that log identifies the failed fingerprint, hook, or file operation, and mods remain suspended for that session.

## Short capture sequence

Use an ordinary playable save and complete this sequence within a few minutes:

1. Load the save, remain still for about 10 seconds, then walk and turn the camera for about 20 seconds.
2. Open the pause menu for about 5 seconds, resume, and move again.
3. Reload the same save using the normal game menu. After loading, walk and turn the camera for another 20 seconds.
4. Exit the game normally. Keep the new `crml/engine-observer-<pid>-<start>.jsonl` capture and `crml/crml.log`.

When reporting a capture, include whether loading, movement, pause/resume, reload, and exit succeeded, and mention any unusual slowdown or crash. Approximate action times help correlate the trace. There is no need to change physics settings or travel out of bounds in this test.

Each session uses a new filename. Recording stops at **10 minutes or 64 MiB**, whichever occurs first. Completed records are flushed every worker poll, approximately every 100 ms. A process exit can leave a partial final line or omit the terminal marker; the analyzer reports this explicitly. Capture files accumulate between sessions, so retain or remove them deliberately.

## Analyze a capture

```powershell
python tools/analyze_engine_observer.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant\crml\engine-observer-1234-5678.jsonl" --output .local/engine/observation-report.json
```

The report includes observed thread IDs, paired phase durations, player identity transitions, resource identity replacements, and physics-wrapper correlations. `ready_for_manual_review` means the selected phases, player sampling, and some resource data were present without reported producer drops. It is **not** a gameplay compatibility result, proof of safe mutation, or confirmation that a reload occurred. Read the notes and compare the trace with the performed actions.

`incomplete` identifies missing coverage, reported losses, or a failed capture. Preserve that evidence: missing physics completion, for example, could reflect a different simulation path rather than a broken logger. Do not silently reinterpret it as a successful normal-physics validation.

## Restore ordinary startup

With the game closed:

```powershell
python tools/install.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant" --update --disable-engine-observer --apply
```

This removes the owned observer marker transactionally and updates the receipt. Logs remain available. The next launch resumes ordinary loader startup, including previously installed mods and experimental settings. Removing the loader through its normal uninstaller also removes the owned marker while preserving captures.

## Observed boundaries

These are executable RVAs for the fingerprint in `compatibility.json`; they are not portable engine APIs.

| Trace name | RVA | What is observed |
| --- | --- | --- |
| `movement` | `0x1b98950` | Entry/normal return for validated player controller calls; other calls are counted without per-call records |
| `command_flush` | `0x1af20b0` | Shared command-flush dispatcher; does not distinguish fixed-update, stream-in and stream-out callers |
| `script_fixed` | `0x19bbda0` | Fixed script-update dispatcher, not every script callback or custom event |
| `renderer_sync` | `0x18d22c0` | Renderer synchronization dispatcher; not GPU completion |
| `physics_begin` | `0x2cdba00` | Normal simulation wrapper submission, preserving its floating-point timestep argument |
| `physics_wait` | `0x2ce5380` | Scene-owner wait helper; wrapper identity sampled from owner `+0x240` |
| `physics_complete` | `0x2cdb200` | Completion-task run entry/normal return; wrapper identity derived from embedded task offset `+0x148` |

The simulation wrapper entry is used instead of the outer submission helper's AVX-leading entry because the bundled hook decoder must be able to relocate the overwritten instructions. All seven prefixes are checked before any hook is enabled. Unit tests exercise trampoline creation using copied prefixes; this does not replace live ABI and behavior validation.

Physics completion publishes its flag inside the original function. The observer never dereferences the completion task after that function returns. Completion-task return and wait-helper return are different events; neither alone establishes an exclusive mutation window. The immediate TGS simulation path is outside these normal-wrapper probes.

Player samples are collected at most ten times per second on observed player controller calls. They retain the entity handle and copy resource information from the known `MeshResource`, `CollisionResource`, and `Physics` component representations. Resource snapshots copy ID, reference-count word, and raw state word without retaining or releasing a resource. Access violations reject the read; successful reads still do not establish a synchronized snapshot or ownership.

No engine pointer is dereferenced by the logging worker. Addresses become session-local identity tokens on the producer thread. Tokens can be reused if an address is reused, and resource snapshots can miss intermediate transitions. Missing samples, changed identities, or a reference count are not destructor observations. The capture therefore supports lifetime investigation but does not prove complete resource destruction or generation safety across reloads.

## Capture format and loss handling

Schema 1 uses JSON Lines with a header, events, periodic statistics, and a terminal record when the worker stops recording. All 64-bit object/entity/value/detail identities use decimal strings; `span` and clock/sequence counters are integer fields.

| Field | Interpretation |
| --- | --- |
| `sequence` | Successful publication order into the capture buffer, not a total order of concurrent engine execution |
| `qpc` | Performance-counter timestamp sampled on the calling thread; scale using header `qpc_frequency` |
| `thread` | Windows thread ID of the observation |
| Phase `edge` | 1 = entry, 2 = normal return; exceptions/unwinding are not converted to successful return records |
| Phase `span` | Unique invocation token pairing that phase's entry and return |
| Phase `object` | World/wrapper identity where traced, or zero if unavailable/not applicable |
| Player `object`/`entity` | World identity and full sampled entity handle; `value` is its sampled row |
| Resource `span`/`entity`/`value` | World identity, sampled entity handle, component hash |
| Resource `flags` | Bit 0: component found; bit 1: resource fields readable; bit 2: state/refcount record instead of ID record |
| Resource `object`/`detail` | Resource token plus resource ID, or `(refcount << 32) \| raw_state` when bit 2 is set |

The fixed 8,192-record buffer uses nonblocking producer admission. Contention or capacity exhaustion increments a drop counter; hooks do not wait for disk or allocate capture storage. The worker drains bounded batches. Limits and failures disable recording while pinned hooks remain pass-through until process exit. Capture boundaries can split an invocation or leave queued records unwritten; terminal markers and analyzer notes must be considered alongside paired records.

## Next validation gate

Review the capture before adding any mutation. Establish which phases run on which threads, how normal physics submission/completion/wait relate, whether identities change or disappear across the performed reload, and where observations have gaps. Follow unresolved ownership or scheduling questions with targeted research or additional observation. Only then select one reversible physics-property operation on a disposable object and test effect, restoration and teardown before exposing it through Wasm.
