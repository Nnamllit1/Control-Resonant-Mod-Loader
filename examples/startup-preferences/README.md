# Startup Preferences

A sandboxed mod with separate controls for the photosensitivity warning, save
warning, interaction prompt and reviewed boot movie. Configure it in **Options >
Mods > Startup Preferences** on a supported renderer. Preferences belong to this
installation and mod ID, not a campaign or save slot.

Build and validate from the repository or extracted SDK:

```powershell
python tools/mod.py build examples/startup-preferences --output build/mods/startup-preferences
python tools/mod.py check build/mods/startup-preferences
```

Install the resulting `startup-preferences` folder in `crml/mods`. Use this mod
instead of `startup-skip`; enabling both creates competing startup requests.
The package requires a development runtime exposing its declared imports
(`min_runtime=0.1.0-alpha.4.3.dev.0`). A runtime that lacks the imports rejects it.
Service availability is checked separately: missing media, presentation, storage
or feedback support does not disable the other features. Settings registration
failure disables startup actions. Availability of the settings registry does not
guarantee that the native Mods page can render on a particular game build.

| Setting | Default | Behavior |
| --- | --- | --- |
| Enabled | On | Apply the selected startup actions. |
| Skip photosensitivity warning | On | Request its advertised Continue action. Disable to read the warning. |
| Skip save warning | On | Request Continue; does not change saving. |
| Skip interaction prompt | On | Request Continue at the startup prompt. |
| Skip boot movie | On | Request skipping the known boot asset after two seconds of reported playback. |
| Hide selected startup screens | Off | Briefly hide the native `splash` class while waiting for selected Continue actions. |
| Show preference errors | On | At most one error message per load, deferred until the main menu. |
| Reset saved preferences | Off | A momentary action: restore defaults and explicitly replace the saved record. |

Legal notices, EULA, privacy, calibration, first-time setup and the main menu are
never selected for automatic Continue or hiding. There are no startup-success
notifications. Errors also go to the runtime log. Disabling a preference prevents
future requests; an already delivered action cannot be undone.

## Saved preferences

The guest stores a fixed 12-byte version-1 record: `SPRF`, little-endian schema
`1`, little-endian length `12`, and a little-endian seven-bit preference mask.
Bits follow the first seven settings in the table. The reset action, registry
handles, revisions, screen generations and delivery receipts are never persisted.

A missing record uses defaults without writing on every startup. Invalid,
empty, oversized, unknown-schema and unreadable records are preserved; edits
remain session-only until **Reset saved preferences** explicitly permits a
replacement. Reset also requires an available storage service. This protects
data written by a newer mod version from silent downgrade overwrite.

Edits are coalesced for 1.25 seconds on the host monotonic clock. At most one write is
pending. The guest checks asynchronous completion before treating its submitted
snapshot as committed; edits made while it is pending are saved afterward.
Rate-limited submissions wait for another interval. Other failures are retried
up to three times per edit/reset, retaining the previous committed record.
Unavailable storage keeps preferences usable for the current session. Error
feedback has at most three publish attempts and never claims that queue
acceptance proves presentation.

Unloading immediately after an edit can lose an unsubmitted change. Accepted
writes are handled by the host's storage worker; the mod never blocks shutdown
waiting for them. Removing the package does not remove its saved preferences.
See [mod persistence](../../docs/mod-storage.md) for installation data lifetime
and filesystem limitations.

## Action delivery and observed visits

Continue requires the advertised action and 0.1 seconds with an unchanged UI
generation. Attempts are separated by at least 0.5 seconds, with at most three
per observed visit. A different observed screen starts a new visit; readiness
generation changes and unavailable observations do not reset the attempt budget.
The SDK cannot identify a same-screen revisit that happens entirely between
observations, so this mod conservatively retains the previous visit's limit.

`ui_action_submit` returns a receipt. The guest retries rejected transient
submissions, or receipts explicitly marked skipped, expired before delivery or
cancelled before delivery. Queued, delivered and unknown-outcome receipts are
observed for up to ten seconds without resubmitting. Dispatched actions, thrown
triggers, lost receipts and unresolved timeouts end automatic attempts for that
visit. A returned engine event trigger is **not confirmation that the screen
transition completed**. The next UI observation remains authoritative.

Optional hiding requests 750 ms leases renewed every 250 ms, for no more than
five seconds on the host monotonic clock per visit. It stops after the attempt budget is
exhausted or the receipt ends further attempts. Expiry, page/screen replacement
and owner cleanup also restore presentation. A queued lease does not prove that
the uniquely matched element exists. Hiding preserves native initialization,
layout and input behavior; it does not make startup instantaneous.

The movie rule matches only
`textures/videos/uiresources/splash/boot.tex`, accepting either path separator.
It requires active, skippable media with an engine or reviewed mapped asset name.
Mapped names are adapter metadata, not proof of general movie enumeration.
The older `media_skip` API has no receipt: after one accepted request the guest
does not retry that continuously observed boot asset. Up to three rejected
requests are allowed, at least 0.5 seconds apart. Only a successful inactive
snapshot or a valid engine/mapped name identifying a different asset resets that
budget. Missing, malformed or untrusted names preserve it. The current media
service reports inactivity as unavailable, so stopping and restarting the same
boot movie without observing another named asset does not reset the budget.
This deliberately favors avoiding duplicate requests over recovering a lost
media request. Arbitrary cutscenes are unaffected.

Timers use `crml_clock_ms()` sampled at callback entry, so stalled callbacks
count toward existing deadlines. A newly observed visit, generation, setting edit
or feedback opportunity starts its own timer at that observation; it does not
inherit time from before it existed. A receipt observed after its ten-second
window ends the visit even if its late status would normally permit a retry.

All selection, settings, record encoding, feedback and timing rules live in the
Wasm guest. Its busiest tick uses at most five gameplay observations and three
commands; settings, storage and feedback use their separate bounded allowances.
The host supplies generic services and enforces native readiness checks.

Build and simulator checks establish guest behavior and service contracts, not
actual game rendering, boot timing or live transitions. The enabled/disabled
startup workflow, native settings and preference retention across a game restart
have also been exercised on game `0.564.478.0` with runtime
`0.1.0-alpha.4.4.dev.0`. This does not establish every startup screen or movie
variant; see the [SDK compatibility matrix](../../docs/sdk-contracts.md).

## Repeatable guest checks

Use an isolated mods directory so other installed examples do not contribute
events to the expected trace:

```powershell
python tools/mod.py build examples/startup-preferences --output build/startup-check/mods/startup-preferences
python tools/mod.py simulate build/startup-check/mods examples/startup-preferences/dispatched-visit.json
python tools/mod.py simulate build/startup-check/mods examples/startup-preferences/unknown-no-replay.json
python tools/mod.py simulate build/startup-check/mods examples/startup-preferences/skipped-bounded.json
python tools/mod.py simulate build/startup-check/mods examples/startup-preferences/settings-behavior.json
python tools/mod.py simulate build/startup-check/mods examples/startup-preferences/movie-no-replay.json
python tools/mod.py simulate build/startup-check/mods examples/startup-preferences/long-stall.json
```

Supply `--host path/to/crml_host.exe` when the development host is outside the
tool's default search locations. These scenarios cover visit boundaries,
readiness changes, same-screen page reloads with an intervening unknown screen,
missing delivery outcomes, bounded skipped-command retries, settings edits and
an accepted media request with consumption withheld, missing asset names and
unavailable media intervals. The long-stall scenario verifies that old receipts
time out without replay and presentation hiding stops, while a newly observed
screen gets its full delay. The simulator has no
persistence service; it does not test saving preferences or disk recovery.
