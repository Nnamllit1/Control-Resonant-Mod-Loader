---
description: Bounded dynamic lists and row activation events in the native Mods page.
---

# Dynamic lists

Declare the `lists` permission and require runtime `0.1.0-alpha.4.4.dev.1` or
later. `CRML_CAP_LISTS` is bit 21. This service publishes one copied page per
loaded mod inside **Options → Mods**. It supplies plain-text rows and activation
events; filtering, pagination, content meaning and actions remain guest policy.
Availability means the service and renderer are enabled, not that a player has
opened or seen the page. Native focus and appearance require game qualification.

Use [text settings](mod-settings.md) for a search query and a numeric setting for
font preference. Read those values in the guest, filter its own data and publish
the resulting rows and `font_scale`. Search does not query game assets. A list
does not capture dialogue, enumerate legal equipment or execute a gameplay action.

## Publish a page

Zero-initialize the structures from `crml_lists.h`, included by `crml.h`:

```c
crml_list_page page = {
    .version = CRML_LIST_VERSION,
    .size = sizeof(crml_list_page),
    .row_count = 1,
    .font_scale = 1.0f,
    .title = "Choices",
    .rows = {{ .id = 42, .flags = CRML_LIST_ROW_ENABLED, .label = "First item" }}
};
int64_t revision = crml_list_publish(&page, sizeof(page));
```

An empty title or active row label is invalid. The 9,840-byte page contains a 96-byte title and
at most 32 rows; zero rows is a valid empty result. Each 304-byte row contains:

| Field | Contract |
| --- | --- |
| `id` | Nonzero guest-defined `uint64_t`, unique within this page; preserve it across reordering when it represents the same item |
| `flags` | `CRML_LIST_ROW_ENABLED` permits activation; `CRML_LIST_ROW_SELECTED` marks the guest's selected row; other bits invalid |
| `reserved` | Zero |
| `label[96]` | Required, at most 95 UTF-8 bytes plus NUL |
| `detail[192]` | Optional, at most 191 UTF-8 bytes plus NUL |

At most one row may be selected, including a disabled row. Selection is a guest
annotation, not an instruction to apply that choice. All text is single-line
UTF-8 without C0/C1 controls or U+2028/U+2029; text is rendered literally.
`font_scale` must be finite and between 0.75 and 1.5 inclusive. It affects list
text, not all game UI. Inactive rows and bytes after a string's first NUL have no
effect on content identity. Set `version=1` and the exact `size` from the header.

`list_publish` atomically copies the complete page and returns a positive signed
64-bit host revision on admission. Identical effective content keeps its revision;
changed text, order, flags, IDs or font scale get a new revision. Hide followed by
republish also gets a new revision. Revisions are transient and must not be saved
as content IDs. Store the mapping from an accepted revision to the guest model
needed to interpret an eventual activation.

| Result | Meaning |
| --- | --- |
| Positive | Copied page admitted; not presentation acknowledgement |
| `-1` | Service/renderer unavailable or owner absent |
| `-2` | Reserved capacity outcome; owner registration is currently bounded separately |
| `-3` | Invalid descriptor or content |
| `-4` | Owner published less than 100 host-monotonic ms ago, or clock moved backward |
| `-5` | Revision space exhausted |

At most 32 loaded owners can register. Every accepted publish, including an
identical page, consumes the owner's 100 ms rate interval. Invalid/rejected
publishes leave the previous page and accepted events intact. Hide does not reset
the rate limit. Publish and hide share the eight gameplay-command calls allowed
per guest invocation. Invalid guest memory or structure size traps before copying;
an unexpected service failure can also trap.

## Read activations

`crml_list_next(&event, sizeof(event))` destructively removes the oldest accepted
event. It uses the separate eight-observation allowance. The 32-byte event has
`version=1`, `flags=0`, a monotonically increasing `sequence`, the activated
`revision`, and `row_id`. A return of `1` copies that event, `0` means none, and
`-1` means unavailable. Non-success results clear the entire output; a memory or
layout validation trap does not promise an output write.

The host checks the current owner, revision and enabled row under one lock when
accepting an activation. Its native transport rejects stale page requests and
request replays. Each owner has a FIFO of at most 16 accepted actions. A full
queue rejects further activations instead of overwriting accepted ones. Drain it
within the observation allowance; do not assume only one click can arrive between
ticks. Sequence values may have gaps from other owners; they are not timestamps.

Replacing a published list **preserves accepted events**, including events for
the earlier revision. Validate each event against the guest's retained model and
current policy before acting. The user may have activated a row just before a
search or availability update. An activation is not authoritative equipment
eligibility, combat state, campaign identity or an engine mutation receipt.

Destructive reads are not a durable acknowledged message protocol: an event read
before a guest failure is not replayed. Two separately accepted clicks on the same
row are two events. Implement any desired guest debounce explicitly. Future native
mutation APIs need their own execution-time validation and idempotency contracts.

## Visibility and cleanup

`crml_list_hide()` returns `1` when it clears a page or pending actions, `0` when
already clear, and `-1` for an unknown owner. It clears host state immediately;
native pixels change on a subsequent renderer update. Options close/reopen does
not itself discard the guest's published data or already accepted events. Old
document requests are rejected by the page nonce checks.

`release()`, guest failure, unload and renderer disable clear the owner's list
and pending actions as applicable. Re-enabling the renderer does not revive old
pages. Lists have no timed lease and are not a gameplay overlay. The guest must
publish again after an explicit clear. Lists are not automatically persisted.

The service, guest-memory boundary and authored browser fixture can be tested
offline. These checks do not establish native renderer focus, input ownership,
gamepad behavior or successful gameplay actions. Full transcript retention and
campaign-aware storage remain separate missing capabilities.
