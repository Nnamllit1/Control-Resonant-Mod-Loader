---
description: Show bounded Wasm-owned messages in the native renderer and check delivery receipts.
---

# Mod feedback

Development builds after Alpha 4.3 provide the experimental `feedback` capability.
It displays short, passive messages inside the game's existing Cohtml UI document,
using its tutorial-panel typography and border with a translucent dark background.
There is no separate desktop window or external graphics overlay. CRML creates
and owns the message elements in a separate notification lane; it does not insert
messages into the game's loot queue or overwrite its HUD notification slots.
The game supplies the presentation styles; CRML controls attribution and lifetime.
Messages use static panels without backdrop blur or retained animations.
Messages do not take focus, capture keys, add buttons or accept HTML. The native
resource hook requires a supported executable, renderer and UI document.
Browser and native service tests cover delivery and cleanup; they do not verify
the game's actual font rendering, appearance or coexistence with stock notifications.

```ini
id=my-mod
abi=1
module=my-mod.wasm
min_runtime=0.1.0-alpha.4.3.dev.0
capabilities=feedback
```

## Publish and observe

```c
int64_t ticket = crml_feedback_show("Preferences updated.", 20,
                                  CRML_FEEDBACK_SUCCESS, 2000);
if (ticket > 0) {
    /* Retain the ticket and inspect it on later ticks. */
    int status = crml_feedback_status((uint64_t)ticket);
}
```

`feedback_show(text, length, severity, duration_ms) -> i64` copies the text and
returns a positive receipt on acceptance. Acceptance means queued, not displayed.
The guest chooses the message, timing and retry behavior. The renderer adds the
mod's name and package ID so one mod cannot hide its identity behind another
mod's display name.

Text is 1–240 UTF-8 bytes without ASCII control characters or a NUL terminator in
the length. Severity is `CRML_FEEDBACK_INFO`, `SUCCESS`, `WARNING` or `ERROR`.
Duration is 1,000–10,000 milliseconds from acceptance, using the host's monotonic
clock. It does not pause with gameplay. At most one message per mod and four
messages globally can remain active. A mod may publish at most once per second;
dismissing a message does not reset this limit. The registry supports 32 loaded
owners. All three imports share a separate 16-call budget per guest callback.

| Publish result | Meaning |
| --- | --- |
| Positive | Owner-scoped receipt |
| `-1` | Service or renderer unavailable |
| `-2` | This mod already has an active message, or global capacity is full |
| `-3` | Invalid text, severity or duration |
| `-4` | Publish rate limit; retry on a later tick |
| `-5` | Internal failure or exhausted receipt space |

Invalid guest memory and lengths over 240 bytes trap the guest before service
access. Missing permission rejects imports at instantiation. Content/range errors
within the bounded buffer return `-3`. Capability availability reports an installed
renderer service, not a loaded UI page or confirmed presentation.

## Delivery receipts

`feedback_status(ticket) -> i32` returns:

| SDK constant | Value | Meaning |
| --- | --- | --- |
| `CRML_FEEDBACK_QUEUED` | 1 | Accepted; no renderer acknowledgement yet |
| `CRML_FEEDBACK_PRESENTED` | 2 | Renderer acknowledged inserting the message |
| `CRML_FEEDBACK_EXPIRED_UNPRESENTED` | 3 | Expired without an acknowledgement observed so far |
| `CRML_FEEDBACK_EXPIRED_PRESENTED` | 4 | Expired after presentation was acknowledged |
| `CRML_FEEDBACK_DISMISSED` | 5 | Guest dismissed the active message |
| `CRML_FEEDBACK_CANCELLED` | 6 | Renderer service stopped or guest requested shared cleanup |

Presentation is not proof that the player saw the message or that its associated
gameplay operation succeeded. A late acknowledgement can change expired status
3 to 4 without redisplaying anything. A missing acknowledgement does not prove
the text was never visible. These receipts describe feedback only.

Only the latest successful receipt per loaded mod is retained. A later successful
publish retires the previous receipt. Failed publishes preserve it. Status returns
`-1` if the owner is unavailable, `-2` for zero/stale/foreign receipts, or `-5`
on internal failure. Receipts are opaque and must not be persisted across reloads.

`feedback_dismiss(ticket) -> i32` returns `1` when it dismisses an active message,
`0` when the receipt is already terminal, and the same negative results as status.
`crml_release()` cancels the mod's active feedback alongside its gameplay leases;
the cancelled receipt remains readable until superseded or unloaded.
Fault and unload remove all owner state. The renderer normally observes removal
on its next 250 ms poll; it also expires messages locally if communication stops.
Transport failure hides stale messages. Page replacement rejects old page
acknowledgements and may re-present an unexpired message without extending its
deadline. A rejected page stops polling; page unload cancels pending callbacks.
Empty replies do not create or modify message elements. None of these operations
change native input ownership.

## Development workflow

The settings example shows how to coalesce preference edits into feedback and
inspect receipts. The standalone host links these imports but reports the renderer
unavailable. In a [scenario test](mod-testing.md), request the `feedback` capability
to enable a deterministic fake renderer. Set `feedback_renderer: false` in a state
to withhold polls and test missing delivery. This verifies guest behavior and
receipt handling, not the game's actual renderer or visual layout.

The runtime package includes `crml/native-ui-feedback.html` when built with native
UI support. No diagnostic marker is needed. A missing or invalid feedback payload
does not disable an otherwise valid startup or settings UI payload.
