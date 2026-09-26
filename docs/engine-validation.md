---
title: Engine observer diagnostics
description: Capture CONTROL Resonant update phases, body identities, and ECS associations with read-only diagnostics to investigate runtime safety.
---

# Engine observer diagnostics

The engine observer records selected engine phases and player/resource identities so contributors can investigate update timing and object lifetime. Use it alongside the [engine atlas](engine-atlas.md) to interpret native update phases. The observer has no modification API.

Observation mode suspends all Wasm mods and the existing noclip, visibility, input-filter, and fall-recovery features. It installs version-gated native hooks that forward the selected engine calls and copy diagnostic records. It does not intentionally change gameplay state or call property setters. Hooks introduce overhead, and capture durations include that overhead.

## Build and enable

Build on Windows with the dependencies described in [installation](installation.md):

```powershell
.\build.bat -EngineObserver -Test
```

Close the game, then preview and apply an update to an existing loader installation. Enter your own installation directory when prompted:

```powershell
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
python tools/install.py "$gameDir" --update --engine-observer
python tools/install.py "$gameDir" --update --engine-observer --apply
```

For a fresh installation, omit `--update`. The installer checks the supported executable, package feature metadata, existing ownership receipt, and whether the game is running. The observer option cannot be combined with other experimental mode selections. Existing mods and settings are preserved but remain suspended while observation mode is enabled.

Start the game normally through Steam. `crml/crml.log` should report **Observe-only engine validation active** and the capture filename. The observer uses automatic logging; it has no in-game panel or hotkey requirement. If initialization fails, that log identifies the failed fingerprint, hook, or file operation, and mods remain suspended for that session.

## Short capture sequence

Use an ordinary playable save and complete this sequence within a few minutes:

1. Load the save, remain still for about 10 seconds, then walk and turn the camera for about 20 seconds. For schema-2 body observation, include an area with loose movable props; nudge an ordinary prop if convenient.
2. Open the pause menu for about 5 seconds, resume, and move again.
3. Reload the same save using the normal game menu. After loading, walk and turn the camera for another 20 seconds.
4. Exit the game normally. Keep the new `crml/engine-observer-<pid>-<start>.jsonl` capture and `crml/crml.log`.

When reporting a capture, include whether loading, movement, pause/resume, reload, and exit succeeded, and mention any unusual slowdown or crash. Approximate action times help correlate the trace. There is no need to change physics settings or travel out of bounds in this test.

Each session uses a new filename. Recording stops at **10 minutes or 64 MiB**, whichever occurs first. Completed records are flushed every worker poll, approximately every 100 ms. A process exit can leave a partial final line or omit the terminal marker; the analyzer reports this explicitly. Capture files accumulate between sessions, so retain or remove them deliberately.

## Analyze a capture

```powershell
python tools/analyze_engine_observer.py "$gameDir\crml\engine-observer-1234-5678.jsonl" --output .local/engine/observation-report.json
```

The report includes observed thread IDs, paired phase durations, player identity transitions, resource identity replacements, and physics-wrapper correlations. Resource observations group IDs, raw state words and reference-count ranges by owner/component/object identity. Replacements within one owner are counted separately from changes across different player handles. `ready_for_manual_review` means the selected phases, player sampling, and some resource data were present without reported producer drops. It is **not** a gameplay compatibility result, proof of safe mutation, or confirmation that a reload occurred. Read the notes and compare the trace with the performed actions.

`physics_windows` groups each wrapper's timestamped observations between consecutive submission entries. It classifies only windows containing exactly one matched entry/return pair for submission, completion and wait, with distinct timestamps. Missing/extra boundaries, timestamp ties and mismatched spans remain unclassified. These windows are not engine frame IDs: multiple substeps and missing submissions can make grouping ambiguous. Same-thread completion inside a wait interval is evidence against treating that interval as idle, not proof of exclusive ownership. `duration_seconds` covers the observed event timestamp range; `last_stats_elapsed_seconds` separately reports elapsed recording time at the last statistics record.

`incomplete` identifies missing coverage, reported losses, or a failed capture. Preserve that evidence: missing physics completion, for example, could reflect a different simulation path rather than a broken logger. Do not silently reinterpret it as a successful normal-physics validation.

## Restore ordinary startup

With the game closed:

```powershell
python tools/install.py "$gameDir" --update --disable-engine-observer --apply
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
| `post_physics` (schema 2) | `0x2ce5310` | Scene-owner post-processing entry/normal return; bounded body sampling after the original returns |

The simulation wrapper entry is used instead of the outer submission helper's AVX-leading entry because the bundled hook decoder must be able to relocate the overwritten instructions. All eight prefixes are checked before any hook is enabled in the current build. Unit tests exercise trampoline creation using copied prefixes; this does not replace live ABI and behavior validation. The original schema-1 capture used seven hooks, without post-processing/body observation.

Physics completion publishes its flag inside the original function. The observer never dereferences the completion task after that function returns. Completion-task return and wait-helper return are different events; neither alone establishes an exclusive mutation window. The immediate TGS simulation path is outside these normal-wrapper probes.

Player samples are collected at most ten times per second on observed player controller calls. They retain the entity handle and copy resource information from the known `MeshResource`, `CollisionResource`, and `Physics` component representations. Resource snapshots copy ID, reference-count word, and raw state word without retaining or releasing a resource. Access violations reject the read; successful reads still do not establish a synchronized snapshot or ownership.

No engine pointer is dereferenced by the logging worker. Addresses become session-local identity tokens on the producer thread. Tokens can be reused if an address is reused, and resource snapshots can miss intermediate transitions. Missing samples, changed identities, or a reference count are not destructor observations. The capture therefore supports lifetime investigation but does not prove complete resource destruction or generation safety across reloads.

## Capture format and loss handling

Captures use UTF-8 JSON Lines with LF record terminators. The analyzer also accepts older Windows captures with CRLF terminators and applies the capture-size limit after normalizing those terminators.

Schemas 1 through 4 use JSON Lines with a header, events, periodic statistics, and a terminal record when the worker stops recording. The analyzer accepts all four versions. All 64-bit object/entity/value/detail identities use decimal strings; `span` and clock/sequence counters are integer fields.

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

## Guarded body observation: schema 2

Schema 2 additionally gates the loaded `PhysX_64.dll` against SHA-256 `ec53e67b700e67a5420a2c951340efd9a8ed9d086da539eed6ee88368fe0ce51` before enabling any hooks. A missing or mismatched backend refuses the observer with mods still suspended. The header records this second fingerprint.

After the original post-processing call returns, at most once per 100 ms across the observed scenes, the hook scans up to 128 body slots and copies at most eight accepted dynamic-body observations. A rotating cursor continues after the last inspected slot; this is bounded sampling, not a complete inventory or fair enumeration across multiple scenes. Tables larger than the diagnostic one-million-slot bound are rejected.

The sampler checks handle-table and actor-table bounds, full handle identity, the known dynamic-body vtable/type word, and the actor's encoded handle. It reads normal linear/angular damping at actor+0xc8/+0xcc or the tagged alternate representation described in [physics dynamics](physics-dynamics.md#damping-sleeping-and-contactsolver-properties). Tag zero is rejected when the alternate property representation is required. Non-finite or negative values are rejected. A final pointer/count/identity/representation recheck detects some concurrent changes; access violations reject the snapshot. None of these checks establishes atomicity, ownership, or an exclusive mutation window.

| Record | Fields |
| --- | --- |
| `body` | `span`: enclosing post-processing observation; `object`: actor token; `entity`: full native body handle, **not an ECS entity**; `detail`: scene-owner token; `value`: IEEE-754 float bits, linear damping in the low word and angular damping in the high word; `flags`: 1 for normal storage, 3 for tagged alternate storage |
| `body_scan` | `span`: enclosing post-processing observation; `object`: scene-owner token; `entity`: number of accepted snapshots attempted for publication; `value`: inspected slots; `detail`: sampled handle-table count; `flags`: rejection-category bitmask |

Rejection bits 1–10 are `arguments`, `bounds`, `generation`, `missing_actor`, `actor_type`, `actor_identity`, `representation`, `scalar`, `changed`, and `memory`. A bit reports that a scan encountered the reason, not its occurrence count. Zero slots can mean an empty, unreadable or rejected table. Publication losses can make accepted-attempt counts differ from recorded body events.

The analyzer's `body_probe` groups samples by scene/full handle/actor token, reports damping ranges, observed slot replacements, and scans containing each rejection category. `observed` means samples exist; `ownership_or_mutation_verified` remains false. A schema-2 capture with no accepted body samples is `incomplete`. Unobserved recycling, world/address reuse and missing lifecycle markers still limit interpretation. The existing gameplay sequence is sufficient for this diagnostic stage; it does not require a property-edit hotkey. Post-processing durations include the sampler's overhead, so they are not directly comparable to uninstrumented timings.

## Body-to-entity observation: schema 3

Schema 3 retains the eight hooks and backend fingerprint gate. During the existing player sample, at most once per 100 ms, it obtains the scene owner from the world supplied by that movement callback. It scans up to 64 slots and attempts publication of at most four accepted associations. This additional rotating scan is separate from the post-processing damping scan. It does not retain world or actor pointers for another callback, and it adds no property getter/setter calls.

Each candidate first passes the dynamic-body snapshot checks. Association lookup then bounds the reverse-pair, scene-record and instance-association tables; checks the occupied scene slot; round-trips the instance's local full body handle; and resolves scene record `+0x30` through the same world's ECS generation and location tables. The entity's `Physics` component must point back to that scene slot. A final reread checks both ends and the intermediate pointers. Missing, changed or unreadable relationships are rejected. These checks support observation only: concurrent reuse and ownership are not solved by rereading fields.

| Record | Fields |
| --- | --- |
| `entity_body` | `span`: world token, **not an invocation span**; `object`: scene-owner token; `entity`: ECS handle; `value`: full native body handle; `detail`: actor token; `flags`: 1 |
| `entity_scan` | `span`: world token; `object`: scene-owner token or zero when lookup fails; `entity`: accepted publication attempts; `value`: inspected slots; `detail`: sampled body-table count; `flags`: rejection mask |

The scan retains body-rejection bits 1–10. Association-rejection bits 17–25 are `arguments`, `world`, `bounds`, `association`, `scene`, `instance`, `entity`, `changed`, and `memory`. Bits 0 and 16 are reserved and unset. Counts describe bounded samples, not the total population or a complete error count. Global lookup is limited to 65,536 entries; body-related table capacities retain the one-million-slot diagnostic limit.

The analyzer reports `entity_probe` associations grouped by world, scene, body handle, actor token and ECS handle. A schema-3 capture with none is incomplete; an older capture reports this probe as `not_recorded`. A recorded association is not proof that an object is disposable, selectable, or safe to modify. The same gameplay test sequence above collects these observations automatically. Movement timings include this sampler's overhead.

## Native damping readback: schema 4

Schema 4 adds a bounded native-getter comparison after each accepted body-to-entity sample, up to four comparisons per movement sampling pass. It uses the reviewed linear and angular damping getters in the fingerprinted PhysX module. Before calling them, the probe revalidates the body snapshot and requires virtual slots `+0x130` and `+0x140` to match the two exact expected targets. A changed body, representation or damping snapshot rejects the comparison. It rechecks after the first call before attempting the second, then checks again after the second call. Access violations and non-finite or negative returns are rejected.

The getter results are compared with the stable before/after snapshot. Agreement is readback evidence, not proof of thread ownership or permission to write. The two scalars are not an atomic transaction, and identity/value rereads cannot rule out ABA reuse. The probe performs no property setters and does not expose accessors to Wasm.

| Record | Fields |
| --- | --- |
| `body_accessor` | `span`: enclosing movement invocation; `object`: actor token; `entity`: full native body handle; `detail`: scene-owner token; `value`: getter float bits, linear low word and angular high word; `flags`: 1 = agreement, 4 = mismatch, plus bit 1 for alternate storage |
| `accessor_scan` | `span`: movement invocation; `object`: scene-owner token; `entity`: readable comparisons attempted for publication, including mismatches; `value`: attempted getter comparisons; `detail`: zero; `flags`: rejection mask |

Rejection bits 1–7 are `arguments`, `snapshot`, `slot`, `scalar`, `changed`, `mismatch`, and `memory`. Mismatch records preserve the finite getter values; other failures publish no value record. Publication losses can remove either a result or a scan summary. The analyzer flags a mismatch seen in either source and treats missing agreement or any observed mismatch as incomplete. Older schemas report `accessor_probe` as `not_recorded`; `agreement_observed` always retains `ownership_or_mutation_verified: false`.

The capture procedure collects these comparisons automatically. Getter agreement does not establish scheduler exclusion, property-write behavior, restoration, or reload cleanup.

## Scheduler and lifetime requirements

Polling helper `0x2cdbbf0` calls `0x3271510`, which selects work through `0x3272a70` and executes it through `0x3272ad0` before returning. The executor invokes a virtual callback, processes dependency counters, and has recursive execution branches. The trace establishes a scheduler execution path, not every concrete callback target or the complete task graph.

Simulation tail-calls `0x2ce5310`, which forwards to post-processing implementation `0x2cdbdb0`. That implementation also polls through `0x3271510`. Neither wait entry nor entry into post-processing is therefore an established exclusive modification window. The [scheduler reference map](research/physics-scheduler-map.json) supplies independently checkable static transfers:

```powershell
python tools/verify_engine_map.py "$gameDir\CONTROLResonant.exe" docs/research/physics-scheduler-map.json
```

Treat persistent resource IDs, native object pointers, full entity handles, and world identities as separate concepts. Equal resource IDs do not prove that the same resource object survived a reload. A gap in observations does not prove destruction or identify a reload without a corresponding lifecycle marker. Capture loss limits all absence and ordering conclusions.

## Handling diagnostic reports

Keep raw captures, individual play-session results and investigation notes in the ignored `.local/` directory. Public documentation describes reproducible procedures, evidence requirements and supported behavior. Review any report before sharing it, and remove installation paths, user or machine names, session identifiers and save-specific details.
