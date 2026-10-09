---
description: Request native tutorial hints, timed prompts and dismissible panels from Wasm mods.
---

# Native tutorials

The `tutorials` capability provides a timed hint, a timed prompt with the
game's dismiss action and progress bar, and a panel with the game's Continue
control. All use the engine's tutorial renderers,
with private mod-owned page data. They do not add campaign tutorial records or
mark campaign tutorials complete. Requests use bounded, copied page descriptions;
the runtime owns native presentation and cleanup.

Include `crml.h` and declare `capabilities=tutorials` in `mod.ini`. Also declare
`log` or `input.actions` if the mod uses those services. Use
`min_runtime=0.1.0-alpha.4.4.dev.0` for hints and panels, or
`min_runtime=0.1.0-alpha.4.4.dev.2` for prompts. Check availability at runtime: a compatible
SDK version alone does not establish engine compatibility.

## Requests

| Import in `crml_v1` | Wasm signature | Result |
| --- | --- | --- |
| `tutorial_available` | `(i32 kind) -> i32` | `1` available, `0` unavailable, `-3` invalid kind |
| `tutorial_show` | `(i32 kind, i32 title, i32 title_length, i32 body, i32 body_length, i32 duration_ms) -> i64` | Positive request ticket; negative error |
| `tutorial_present` | `(i32 page, i32 page_size) -> i64` | Positive request ticket for a versioned page; negative error |
| `tutorial_status` | `(i64 ticket) -> i32` | Current status for this mod's latest ticket |
| `tutorial_dismiss` | `(i64 ticket) -> i32` | `1` cancellation requested, `0` already terminal |

`CRML_TUTORIAL_HINT` (`0`) requires a duration of 1,000–30,000 milliseconds.
`CRML_TUTORIAL_PROMPT` (`2`) has the same duration bound. By default it shows
the stock bottom progress bar and `CLOSE_TUTORIAL` action callout. The game
supplies the current key or controller glyph; a mod does not choose a physical
key. The player can dismiss the prompt through that action, or it expires.
The input event stays in a private request vector and does not complete a
campaign tutorial.
`CRML_TUTORIAL_PANEL` (`1`) requires duration `0`; the player dismisses it through
the native control, or the mod requests cancellation.

Titles contain 1–128 UTF-8 bytes and bodies 1–1,024 bytes, without a terminating
NUL. Both are plain text. Bodies permit newlines and tabs; markup is escaped.
Other ASCII controls, NUL, DEL and invalid UTF-8 are rejected. Text is copied
before the import returns. The native localization layer normally echoes unknown
text; exact existing localization keys and localization debug modes can change
its display. No invisible prefix is inserted, because the game font can display
format characters as missing-glyph boxes. Out-of-bounds guest memory and oversized buffers trap
the calling mod. Custom image files, localization resources and dynamic key
glyphs for arbitrary actions are not exposed in this version.

### Page descriptions and image layout

Use `tutorial_present(&page, sizeof(page))` for authored pages. Version 1 of
`crml_tutorial_page` occupies 1,444 bytes and contains no pointers. Zero-initialize
it, set `version=CRML_TUTORIAL_PAGE_VERSION`, choose `kind` and `duration_ms`, and
fill its NUL-terminated `title`, `body` and optional `image_url` arrays. The text
limits above exclude the terminator. Keep `reserved` zero. Invalid versions,
missing terminators and unsupported layout values return `-3`; a wrong buffer
size or out-of-bounds guest buffer traps the mod. The host copies the descriptor
before returning, including for unaligned guest addresses.

Version 1 remains unchanged. For a prompt it defaults to native dismissal and
progress. To select prompt options, use the same 1,444-byte descriptor with
`version=CRML_TUTORIAL_PAGE_VERSION_OPTIONS` (`2`) and put a bitwise combination
of `CRML_TUTORIAL_OPTION_NATIVE_DISMISS` and `CRML_TUTORIAL_OPTION_PROGRESS`
in `reserved`. With version 2, `reserved=0` gives a timed prompt without those
two features. Other bits are rejected, and hints and panels require zero
options. `tutorial_show(CRML_TUTORIAL_PROMPT, ...)` uses both defaults.

`tutorial_show` remains unchanged for compact text-only requests. Both calls
use the same queue, receipts, cancellation and lifetime rules.

| `page.image` field | Values | Zero default |
| --- | --- | --- |
| `position` | `CRML_TUTORIAL_IMAGE_ABOVE`, `CRML_TUTORIAL_IMAGE_BELOW` | Above the body |
| `alignment` | `CRML_TUTORIAL_ALIGN_CENTER`, `CRML_TUTORIAL_ALIGN_LEFT`, `CRML_TUTORIAL_ALIGN_RIGHT` | Center |
| `width_percent` | 1-100 percent of body width | 100 |
| `max_height_vh` | 1-32 percent of viewport height | 18 |
| `gap_vh` | 1-4 percent of viewport height between image and body | 1 |

Image and body occupy separate blocks; text never wraps alongside the image.
The native heading remains above the content and Continue remains below it.
The runtime reserves an image area using `width_percent` and `max_height_vh`.
The engine fits the full texture proportionally inside it, without cropping or
stretching, aligned toward the body text. Unused space remains transparent. An empty
`image_url` gives the same text-only content as `tutorial_show`, without an
empty image block or extra spacing. Layout values are still checked.

The image URL may contain at most 256 bytes. Only canonical
`coui://base/textures/uiresources/` PNG paths are accepted. The remaining path
may contain ASCII letters, digits, `/`, `_`, `-` and `.`; traversal, dot-only or
empty path segments, query strings, escapes and other schemes are rejected.
Use images already provided by the game. This API does not register custom
textures or permit network requests, arbitrary file access, CSS or HTML.

The runtime adds an image container beside the native rich-text paragraph inside
the tutorial content area. It keeps images outside the engine's `cohinline` text
layout, so text cannot flow around them. Ordinary game tutorials keep their
original binding and layout. If the image-layout adapter is unavailable, the
page falls back to readable text. Its streamed header-image slot is
not used: that slot also requires a matching engine resource identity and
readiness state. Mods cannot pass native resource handles through this API.

Image loading is asynchronous. `PRESENTED` acknowledges native tutorial state,
not successful image decoding. Essential instructions should remain in the
body; a missing image leaves an empty image area and does not hide the body or
Continue control. Rendering does not depend on JavaScript image-size getters or
load events.

`tutorial_show` and `tutorial_present` return `-1` when unavailable, `-2` when busy, `-3` for invalid
arguments, and `-5` when the process text budget is exhausted. Receipt operations
return `-1` for an unavailable owner and `-2` for an unknown or superseded ticket.
Tickets from other mods cannot be queried or cancelled.

## Status and cleanup

| Constant | Meaning |
| --- | --- |
| `CRML_TUTORIAL_QUEUED` | Accepted, awaiting an eligible native update |
| `CRML_TUTORIAL_PRESENTED` | Native presentation state entered; not proof the player read it |
| `CRML_TUTORIAL_CANCELLING` | Cancellation requested; native retirement is still pending |
| `CRML_TUTORIAL_DISMISSED` | Native retirement completed after timeout or player dismissal |
| `CRML_TUTORIAL_CANCELLED` | Cancelled before presentation or retired after cancellation |
| `CRML_TUTORIAL_UNAVAILABLE` | Provider could not service the request |
| `CRML_TUTORIAL_FAILED` | Adapter failed; does not imply successful native cleanup |

Each mod can have one outstanding request, and the service accepts eight in total.
One hint or prompt runs in the native dynamic-toast channel at a time. Queued
mod hints and prompts enter in ticket order; game hints take precedence over
both. Panels use their separate native context and wait until it is idle;
requests are not a way
to force arbitrary menu navigation. Request progress depends on engine callbacks,
so it may pause while the relevant world or UI is inactive.

Unload, a guest trap and `crml_release()` cancel outstanding requests. The native
adapter retains its data until the renderer acknowledges retirement. If the
original context disappears, retirement can remain pending; the adapter does not
free data based solely on a timeout. Native faults disable that adapter and can
retain one bounded allocation until process exit.

All tutorial imports share a limit of 16 calls per guest callback. The process
accepts at most 64 distinct title/rendered-body combinations to bound the engine's
localization fallback cache. Image and layout variations count toward that limit.
Zero defaults and equivalent explicit values share one entry. Reusing the same
content does not consume another entry; reloading
mods does not reset the budget. Use tutorials for authored instructions, not
continuously changing telemetry.

The service requires a reviewed native executable and a runtime built with
the gameplay services enabled. Read-only tutorial observation and writable tutorial adapters cannot
run together. The offline SDK host reports this service unavailable; its test
provider does not emulate native tutorial rendering.

The native prompt input adapter is enabled only when its reviewed worker hook
installs successfully. If that hook is unavailable, `tutorial_available(2)`
returns `0` while the existing hint and panel remain usable. Automated tests
cover private input isolation, timer values and native retirement; live game
input and UI behavior for prompts have not yet been qualified.

## Example

```c
if (crml_tutorial_available(CRML_TUTORIAL_HINT) == 1) {
    const char title[] = "New ability";
    const char body[] = "Open this mod's settings to choose your controls.";
    int64_t ticket = crml_tutorial_show(CRML_TUTORIAL_HINT,
        title, sizeof(title) - 1, body, sizeof(body) - 1, 6000);
    /* Retain a positive ticket and poll its status in later callbacks. */
}
```

The SDK's `examples/tutorials` package demonstrates both forms, polling and
cancellation without opening a panel automatically. F9 places a game-owned
image above its hint body; F10 places it below the panel body. No extracted game
images are included in the SDK.

For a dismissible bottom prompt, use the same text API with
`CRML_TUTORIAL_PROMPT` and a bounded duration:

```c
if (crml_tutorial_available(CRML_TUTORIAL_PROMPT) == 1) {
    const char title[] = "Movement";
    const char body[] = "Try your new movement ability.";
    int64_t ticket = crml_tutorial_show(CRML_TUTORIAL_PROMPT,
        title, sizeof(title) - 1, body, sizeof(body) - 1, 6000);
}
```

```c
static const crml_tutorial_page page = {
    .version = CRML_TUTORIAL_PAGE_VERSION,
    .kind = CRML_TUTORIAL_HINT,
    .duration_ms = 6000,
    .image = { CRML_TUTORIAL_IMAGE_ABOVE, CRML_TUTORIAL_ALIGN_CENTER, 80, 12, 2 },
    .title = "New ability",
    .body = "Open this mod's settings to choose your controls.",
    .image_url = "coui://base/textures/uiresources/UI/streamed/default_save_menu_header.png"
};
/* After checking availability, retain and poll the returned ticket. */
int64_t ticket = crml_tutorial_present(&page, sizeof(page));
```
