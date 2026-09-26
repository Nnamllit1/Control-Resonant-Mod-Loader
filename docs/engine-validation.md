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

1. Load the save, remain still for about 10 seconds, then walk and turn the camera for about 20 seconds. For schema-2 body observation, include an area with loose movable props; nudge an ordinary prop if convenient.
2. Open the pause menu for about 5 seconds, resume, and move again.
3. Reload the same save using the normal game menu. After loading, walk and turn the camera for another 20 seconds.
4. Exit the game normally. Keep the new `crml/engine-observer-<pid>-<start>.jsonl` capture and `crml/crml.log`.

When reporting a capture, include whether loading, movement, pause/resume, reload, and exit succeeded, and mention any unusual slowdown or crash. Approximate action times help correlate the trace. There is no need to change physics settings or travel out of bounds in this test.

Each session uses a new filename. Recording stops at **10 minutes or 64 MiB**, whichever occurs first. Completed records are flushed every worker poll, approximately every 100 ms. A process exit can leave a partial final line or omit the terminal marker; the analyzer reports this explicitly. Capture files accumulate between sessions, so retain or remove them deliberately.

## Analyze a capture

```powershell
python tools/analyze_engine_observer.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant\crml\engine-observer-1234-5678.jsonl" --output .local/engine/observation-report.json
```

The report includes observed thread IDs, paired phase durations, player identity transitions, resource identity replacements, and physics-wrapper correlations. Resource observations group IDs, raw state words and reference-count ranges by owner/component/object identity. Replacements within one owner are counted separately from changes across different player handles. `ready_for_manual_review` means the selected phases, player sampling, and some resource data were present without reported producer drops. It is **not** a gameplay compatibility result, proof of safe mutation, or confirmation that a reload occurred. Read the notes and compare the trace with the performed actions.

`physics_windows` groups each wrapper's timestamped observations between consecutive submission entries. It classifies only windows containing exactly one matched entry/return pair for submission, completion and wait, with distinct timestamps. Missing/extra boundaries, timestamp ties and mismatched spans remain unclassified. These windows are not engine frame IDs: multiple substeps and missing submissions can make grouping ambiguous. Same-thread completion inside a wait interval is evidence against treating that interval as idle, not proof of exclusive ownership. `duration_seconds` covers the observed event timestamp range; `last_stats_elapsed_seconds` separately reports elapsed recording time at the last statistics record.

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
| `post_physics` (schema 2) | `0x2ce5310` | Scene-owner post-processing entry/normal return; bounded body sampling after the original returns |

The simulation wrapper entry is used instead of the outer submission helper's AVX-leading entry because the bundled hook decoder must be able to relocate the overwritten instructions. All eight prefixes are checked before any hook is enabled in the current build. Unit tests exercise trampoline creation using copied prefixes; this does not replace live ABI and behavior validation. The original schema-1 capture used seven hooks, without post-processing/body observation.

Physics completion publishes its flag inside the original function. The observer never dereferences the completion task after that function returns. Completion-task return and wait-helper return are different events; neither alone establishes an exclusive mutation window. The immediate TGS simulation path is outside these normal-wrapper probes.

Player samples are collected at most ten times per second on observed player controller calls. They retain the entity handle and copy resource information from the known `MeshResource`, `CollisionResource`, and `Physics` component representations. Resource snapshots copy ID, reference-count word, and raw state word without retaining or releasing a resource. Access violations reject the read; successful reads still do not establish a synchronized snapshot or ownership.

No engine pointer is dereferenced by the logging worker. Addresses become session-local identity tokens on the producer thread. Tokens can be reused if an address is reused, and resource snapshots can miss intermediate transitions. Missing samples, changed identities, or a reference count are not destructor observations. The capture therefore supports lifetime investigation but does not prove complete resource destruction or generation safety across reloads.

## Capture format and loss handling

Schemas 1, 2 and 3 use JSON Lines with a header, events, periodic statistics, and a terminal record when the worker stops recording. The analyzer accepts all three versions. All 64-bit object/entity/value/detail identities use decimal strings; `span` and clock/sequence counters are integer fields.

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

## Next validation gate

Review the capture before adding any mutation. Establish which phases run on which threads, how normal physics submission/completion/wait relate, whether identities change or disappear across the performed reload, and where observations have gaps. Follow unresolved ownership or scheduling questions with targeted research or additional observation. Only then select one reversible physics-property operation on a disposable object and test effect, restoration and teardown before exposing it through Wasm.

## Body-to-entity observation: schema 3

Schema 3 retains the eight hooks and backend fingerprint gate. During the existing player sample, at most once per 100 ms, it obtains the scene owner from the world supplied by that movement callback. It scans up to 64 slots and attempts publication of at most four accepted associations. This additional rotating scan is separate from the post-processing damping scan. It does not retain world or actor pointers for another callback, and it adds no property getter/setter calls.

Each candidate first passes the dynamic-body snapshot checks. Association lookup then bounds the reverse-pair, scene-record and instance-association tables; checks the occupied scene slot; round-trips the instance's local full body handle; and resolves scene record `+0x30` through the same world's ECS generation and location tables. The entity's `Physics` component must point back to that scene slot. A final reread checks both ends and the intermediate pointers. Missing, changed or unreadable relationships are rejected. These checks support observation only: concurrent reuse and ownership are not solved by rereading fields.

| Record | Fields |
| --- | --- |
| `entity_body` | `span`: world token, **not an invocation span**; `object`: scene-owner token; `entity`: ECS handle; `value`: full native body handle; `detail`: actor token; `flags`: 1 |
| `entity_scan` | `span`: world token; `object`: scene-owner token or zero when lookup fails; `entity`: accepted publication attempts; `value`: inspected slots; `detail`: sampled body-table count; `flags`: rejection mask |

The scan retains body-rejection bits 1–10. Association-rejection bits 17–25 are `arguments`, `world`, `bounds`, `association`, `scene`, `instance`, `entity`, `changed`, and `memory`. Bits 0 and 16 are reserved and unset. Counts describe bounded samples, not the total population or a complete error count. Global lookup is limited to 65,536 entries; body-related table capacities retain the one-million-slot diagnostic limit.

The analyzer reports `entity_probe` associations grouped by world, scene, body handle, actor token and ECS handle. A schema-3 capture with none is incomplete; an older capture reports this probe as `not_recorded`. A recorded association is not proof that an object is disposable, selectable, or safe to modify. The same gameplay test sequence above collects these observations automatically. Movement timings include this sampler's overhead.

## Reviewed body capture: 2026-09-26

The schema-2 capture with SHA-256 `4858b9de066d393d9539529cf19a46cfb8a372a4a62416fd99556c4132d251e8` contains all eight selected phases and 750 player snapshots. Events span 140.090 seconds; the last statistics record is at 154.875 seconds. There are two observed player identities and four resource-object/ID combinations. This capture was collected during a reported walk/reload/walk sequence, but the log contains no explicit reload markers.

The body sampler produced **4,605 accepted snapshots across 749 scans**, grouped into 4,288 scene/handle/actor identities. Of those samples, 1,378 use tagged alternate damping storage. The analyzer observed 139 changes of full handle or actor token at previously sampled scene slots. These demonstrate observed identity changes, not complete destruction coverage or exclusively generation changes. Actor-type rejection occurred in 402 scans and handle-table identity rejection in 36; other rejection categories were not recorded.

The capture remains incomplete: 61 reported dropped records, 59 unmatched wait entries, two orphan returns and no terminal marker. Of 2,771 physics windows, 2,708 had six distinct matched boundaries; completion was inside wait in all 2,708 (1,307 on the waiting thread and 1,401 on another thread). This supports the earlier scheduler observations without establishing exclusion for writes. Body-to-ECS associations, accessor agreement, property effects, restoration and teardown were not measured by this schema-2 capture.

## Reviewed capture: 2026-09-26

An observe-only capture for executable SHA-256 `2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0` contains all seven selected phases. The source capture SHA-256 is `d6e38d10aca4e308b9ccac4324ba0a6f85f162ccbe0b267c78f0afb793564703`. Raw session logs remain local; the table records the reproducible analyzer results without publishing game data or installation paths.

The last statistics record is at 206.500 seconds. Events span 188.539 seconds, with 636 player snapshots. The analyzer reports **incomplete**: 27 reported drops, 27 unmatched entries (26 physics waits and one command flush), and no terminal marker. Missing exit instrumentation prevents a shutdown or teardown validation claim. The log does not distinguish capacity loss from lock contention, and a missing final statistics record could conceal additional loss.

| Phase | Matched entry/return pairs | Distinct observed threads |
| --- | ---: | ---: |
| Player movement | 2,350 | 31 |
| Command flush | 49,017 | 1 |
| Fixed script update | 4,942 | 1 |
| Renderer synchronization | 11,019 | 31 |
| Normal physics submission | 2,350 | 31 |
| Normal physics wait | 2,324 | 31 |
| Normal physics completion | 2,350 | 31 |

Command flush and fixed script update shared one observed thread and each covered two world tokens. Movement covered one of those worlds. This describes the sample; it does not prove that either dispatcher always owns a fixed thread or that every engine callback follows the same schedule.

### Physics intervals and scheduler work

The three physics probes shared one wrapper token. Of 2,350 submission windows, 2,311 pass strict six-boundary classification. Another 26 lack a wait-return record and 13 contain timestamp ties. All 2,311 classified windows contain the complete completion interval inside the wait interval: 1,105 on the same thread and 1,206 on a different thread. No classified window has wait return before completion return. That last observation is not a synchronization guarantee, especially with losses and completion publication occurring inside the original function.

Targeted static tracing explains why a waiting thread can execute work. Polling helper `0x2cdbbf0` calls `0x3271510`, which selects work through `0x3272a70` and executes it through `0x3272ad0` before returning. The executor invokes a virtual callback, processes dependency counters, and has recursive execution branches. The trace establishes a scheduler execution path, not every concrete callback target or the complete task graph.

Post-processing is also substantial work: simulation tail-calls `0x2ce5310`, which forwards to `0x2cdbdb0`. That implementation includes another polling loop calling `0x3271510`. Therefore neither wait entry nor entry into post-processing is an established exclusive modification window. The [scheduler reference map](research/physics-scheduler-map.json) supplies 11 independently checkable static transfers:

```powershell
python tools/verify_engine_map.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant\CONTROLResonant.exe" docs/research/physics-scheduler-map.json
```

### Player and resource identity changes

Two full player handles were sampled in the same world token, separated by a 13.085-second observation gap at the transition. The requested test included a save reload, but the capture has no menu/reload markers; the gap itself cannot identify that action or prove destruction.

| Component | Resource ID in both player identities | Copied reference-count word | Raw state word | Resource object token |
| --- | ---: | ---: | ---: | --- |
| MeshResource | 37,474 | 3 | 5 | Different for each player identity |
| CollisionResource | 37,473 | 2 | 5 | Different for each player identity |
| Physics | 37,473 | 2 | 5 | Shared with CollisionResource for that player identity |

The first player identity has 341 snapshots and the second 295. Each contains readable observations for all three components. Resource IDs remained equal across the identity change while pointer-derived tokens changed. Replacements *within* either owner were zero; that does not mean the resources survived the reload at the same address. Raw state 5 is left uninterpreted here, and the reference-count snapshots do not grant ownership.

### Next experiment requirements

The next focused probe should connect a disposable dynamic world object to its live body and scene, then establish the enclosing simulation phase and reload invalidation. The player resource pointer is not a selected dynamic prop, and the body resolver at `0x2d83f70` indexes scene storage using the low 32 bits of its argument without checking bounds or generation. Calling that resolver with a stale handle would not supply validation.

Use a scalar property such as linear damping as the first mutation candidate after those checks exist. Capture its original value, confirm an applied value through the owning accessor, demonstrate an observable change, restore it, and verify that reload/unload rejects old object identities. Pending requests must not hold raw native pointers across a scheduler callback or reload. This capture does not yet authorize a physics setter or a Wasm API; no additional broad catalog collection is needed to identify the remaining gaps.

The [game damping trace](physics-dynamics.md#game-damping-accessors-and-overrides) identifies getter/setter consumers and two setup paths that can reapply damping. The [body association trace](engine-internals.md#full-body-handles-reverse-association-and-retirement) identifies the scene record's ECS handle. These narrow the remaining validation work: confirm the association on a disposable prop, establish scheduler exclusion, compare the guarded snapshot with accessor readback, and exercise restoration and reload invalidation. Restoration must report an intervening property change instead of silently replacing it.
