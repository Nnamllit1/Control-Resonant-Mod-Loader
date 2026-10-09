# Dialogue observation research

The [fingerprinted map](research/dialogue-observation-map.json) traces a real
subtitle producer through timed text selection, UI publication, and audio timing.
It does not enable a dialogue SDK capability. Completion versus interruption,
presentation acknowledgement, and campaign identity are still unresolved.

An opt-in, ten-minute native diagnostic now observes changes to the selected
timed segment on the reviewed October hotfix executable. It copies the source
allocation counter, shared playback key, segment index, and the selected
formatted line's byte length and hash after the native worker returns. It does
not log dialogue text. Records use `Capability dialogue: ` JSON lines with
schema 1 and `sample` or `totals` type; status and drop counts distinguish
validated selections, invalid reads, capture overflow, and native unwinds.
This evidence is still only a subtitle selection, not proof that a line was
shown, heard, or completed. The observer does not create a guest API.

Verify the native trace without running the game:

```powershell
python tools/verify_dialogue_observation.py '<game directory>/CONTROLResonant.exe'
```

An optional `--ui <local stock UI HTML>` checks the existing presentation model
consumers. No game script or dialogue content is distributed with this research.
The verifier checks the executable fingerprint, native call edges, exact field
access instructions and registered system declarations. These checks detect build
drift; they do not prove runtime ordering or the meaning of an unknown field.

## Established source path

`coregame::global::ActiveSubtitles` contains a vector of 0x70-byte records.
`heron::ui_subtitles::system` gathers records, applies option and forced-line
filters, updates `SubtitleState`, and publishes changed values into the UI fact
dictionary. It gathers from two active-subtitle environments, so sampling only
one environment cannot claim complete coverage.

Each source record has an atomic allocation counter at `+0x60`. The UI instead
matches its 0x58-byte records using source `+0x4c`, a key also consumed by shared
audio playback timing. These are different identities. The counter is process
local and 32-bit; its wrap and reload behavior are not qualified. Neither field
is a campaign ID or a persistent conversation ID.

The selected text is a **current timed segment**: the worker selects a 0x128-byte
row using its lower and upper time bounds, stores the segment index at UI record
`+0x50`, and formats only that row into the line string at `+0x30`. One source can
therefore produce several distinct lines. A future observation key needs the
source allocation and segment plus a verified context epoch. String comparison
or UI slot comparison cannot distinguish repeated identical lines.

The speaker path uses the active source's localization key. The stock UI applies
translation to that key and can hide speaker names. The native worker can omit
the speaker key when names are disabled and the line is not forced. Copying only
the visible model cannot promise a correct speaker in every setting.

## Completion and visibility are separate evidence

Source `+0x54` is current source-relative time and `+0x50` is duration. A fixed-time
path advances current time, while another path obtains timing from shared dialogue
playback and an optional audio playing ID. The audio path can set current time to
duration when timing disappears. The expiry path then removes records whose
current time reaches duration. Thus expiry does **not** establish normal completion;
it can collapse timing loss into the same removal path. Subtitle filtering and
UI compaction can also remove a displayed row without ending its audio.

The stock UI has three subtitle slots. It consumes the line, speaker, forced flag
and narrative graph type, with a separate HUD visibility predicate. A graph type
is a category, not a conversation group identity. Writing a model value does not
establish that a visible renderer presented it. Audio playing state does not prove
audibility. Subtitles disabled must not silently turn all loaded resource text
into a transcript. Only an observed eligible occurrence may supply its current
text; unselected timed rows and future dialogue must remain inaccessible.

## Diagnostic scope and remaining validation

The diagnostic checks the exact worker prologue and its one mapped caller before
installing a hook. It copies bounded fields while the caller still owns both
records, then retains only scalar metadata. It stops admitting observations
after ten minutes or shutdown, reports pending spans and drops, and leaves the
native call and its exceptions unchanged. Its changed-result samples are not a
continuous transcript: other active-subtitle environments, hidden model values,
and samples lost at the bound remain possible.

Further qualification must correlate allocation creation, published slots,
actual visibility and the separate audio timing transition. Capture end causes
from the actual successful-end and stop/cancel producers; mark missing evidence
unknown rather than guessing incomplete/complete.

Use at most 64 source records per sample, 64 events per batch, 4096 UTF-8 bytes per
field, and 4096 admitted changed-result samples. The native event ring holds 256
copied metadata records and reports drops; it never claims a complete transcript.
These are diagnostic limits, not a guest ABI or transcript retention policy.
Independent guest cursors, copied payloads, loss reporting, semantic
simulation time, and campaign/save lineage remain required before SDK delivery.

Qualify repeated identical lines, overlapping speakers, multi-segment sources,
normal endings, cancellation, hidden HUD, subtitles and speaker names disabled,
forced subtitles, pause, scene change, reload, and overflow. A full Notebook also
needs durable campaign-scoped storage and a searchable native history panel.
