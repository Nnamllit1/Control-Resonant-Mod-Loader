---
description: Draw bounded schematic and native-map lines and labels through the game UI.
---

# Drawing

Runtime `0.1.0-alpha.4.4.dev.1` adds the experimental `drawing` capability.
A guest submits a complete copied frame of segments and labels; the trusted
renderer places it in the existing game UI document. Frames are passive, clipped
to their rectangle, and do not capture mouse or keyboard input.

This surface uses viewport coordinates. It does not supply a game map, world
projection, occlusion, persistent markers or a complete route recorder. A mod
must choose its own schematic transform and draw only the history it observed.

```ini
min_runtime=0.1.0-alpha.4.4.dev.1
capabilities=drawing
```

Check `CRML_CAP_DRAWING` before submitting. Availability means the service and
renderer integration are enabled; it does not prove a frame reached the screen.
Native rendering still needs gameplay qualification on the reviewed build.

## Frame contract

Zero-initialize `crml_drawing_frame`, included by `crml.h`, set `version=1` and
`size=sizeof(frame)`, then populate the used primitive arrays. The descriptor is
5680 bytes; a segment is 24 bytes and a label is 80 bytes.

| Field | Contract |
| --- | --- |
| `lifetime_ms` | 100 through 1000 host-monotonic milliseconds from admission |
| `segment_count`, `label_count` | At most 128 segments and 32 labels; at least one primitive |
| `reserved[3]` | All zero |
| `x`, `y`, `width`, `height` | Normalized viewport rectangle, top-left origin, positive Y down; extents at least 0.05 and entire rectangle inside `[0,1]` |
| Segment endpoints / label positions | Finite coordinates in `[0,1]`, relative to the frame rectangle |
| Segment `width_vh` | 0.05 through 1 percent of viewport height |
| Label `font_vh` | 1 through 5 percent of viewport height |
| `rgba` | Packed `0xRRGGBBAA` color |
| Label `text[64]` | Nonempty, terminated UTF-8, at most 63 bytes; no C0/C1 controls or U+2028/U+2029 |

Segments are independent. Leaving out a segment preserves a gap; the renderer
does not connect adjacent endpoints automatically. Labels are plain text, not
HTML, and may be clipped at the frame edge. There is no arbitrary texture, icon,
script, selector or DOM access.

## Submit, renew and hide

`crml_drawing_publish(&frame, sizeof(frame))` atomically replaces this loaded
mod's host frame after complete validation and admission. Each owner can keep one
schematic frame and one native-map frame; at most four frames across both destinations are visible at once. The registry accepts at
most 32 loaded owners. Successful publishes are limited to one per 100 ms per
owner, including renewals and replacement after hide.

| Result | Meaning |
| --- | --- |
| `1` | Frame admitted; not a presentation receipt |
| `-1` | Owner or renderer service unavailable |
| `-2` | The shared four-frame capacity is occupied |
| `-3` | Invalid descriptor or primitive |
| `-4` | Publish rate limited |
| `-5` | Exhausted revision/deadline range |

Invalid guest memory or an incorrect descriptor size traps before the host reads
the frame; an unexpected host service exception also traps. A rejected publish
does not replace the previous frame, whose original
expiry still applies. Renew before expiry and allow scheduling margin; a 100 ms
lease at the minimum publish interval has no tolerance for host or renderer stalls.

`crml_drawing_hide()` returns `1` when host state was cleared, `0` when already
hidden, or `-1` for an unavailable owner. It bypasses the publish rate limit but
does not reset that limit. Both drawing imports consume the existing shared
eight-command allowance per guest invocation. `release`, traps and unload cancel
owned frames; they cannot hide another mod's frame.

## Visibility and expiry

Hide clears host state immediately. **Pixels clear on the next renderer update**;
this API does not promise immediate visual removal or renderer acknowledgement.
The renderer checks approximately every 33 ms while minimap drawings or world
markers are active, approximately 100 ms for other active drawings, and 500 ms
while idle. It allows one request in flight. A first frame published
during idle backoff can expire before it is observed. These are polling intervals,
not latency guarantees.

The native and renderer clocks are separate. Each response carries remaining
host lifetime; the renderer starts that remaining interval at request dispatch,
conservatively subtracting transport delay. An admitted renewal refreshes the host
deadline. Unchanged visible geometry keeps its revision, allowing the renderer to
refresh the deadline without rebuilding its nodes. A poll alone does not renew
the host lease. Expiry is checked on renderer ticks; a stalled renderer can delay
pixel removal.

Malformed replies, connection failure and document teardown clear displayed
frames. A replacement UI document clears host frames instead of replaying an old
frame. The guest must renew after a replacement. Hiding the UI document clears
its drawing and suppresses polling while hidden. There is no final-view or
occlusion acknowledgement.

## Routes and validation

The [navigation observation](api.md#navigation-observation) can provide position
and optional movement-plane up. Its generation and sequence are transient, its
flags are sampled, and no campaign or persistent coordinate-frame identity is
provided. Save-aware markers and verified gravity-transition behavior need
additional evidence. Drawing plus navigation does not complete Exploration
Breadcrumbs or the other proposed full mod designs.

The maintained `examples/route-sketch` guest combines navigation, text settings
and this drawing API for a session-only route schematic. It conservatively breaks
or clears uncertain history. World markers and routes remain session-only;
native X marker metadata uses the storage API and matching map geometry as
described below. This does not establish campaign or save-slot identity.

Automated service and browser fixtures exercise bounds, ownership, whole-frame
replacement, coordinate geometry, clipping, expiry and teardown. Those checks do
not prove native appearance, renderer timing or player input ownership. Validate
the installed renderer at several viewport sizes and through pause, UI replacement,
guest failure and competing mods before relying on it during play.

## Native full-map drawing

Runtime `0.1.0-alpha.4.4.dev.2` adds `crml_map_read`, `crml_map_publish` and
`crml_map_hide` under the same `drawing` permission. Set that minimum runtime
when importing them. Schematic ABI 1 keeps its existing coordinate convention.

`crml_map_read(&state, sizeof(state))` returns `1` with a 24-byte
`crml_map_state`, or `-1` with a cleared output when no fresh projection is
available. Its `context` is an ephemeral geometry token. It is **not** a campaign,
world, floor or persistent map identifier. Layout-only rectangle changes preserve
the token; changes to the world projection or disabling the renderer replace it.
Closing and reopening the same map preserves the token, but reads and drawing
still require fresh native observations. A token alone never proves freshness.
Refresh it before submitting a frame.

Zero-initialize `crml_map_frame` (6816 bytes), set `version=CRML_MAP_VERSION`,
`size=sizeof(frame)`, `target=CRML_MAP_FULL`, the observed `context`, and
`lifetime_ms`. Fill at most 128 independent segments and 32 labels using the
same world axes and units as `crml_navigation_read`. Coordinates must be finite
between -100,000,000 and 100,000,000 units; style and text limits match schematic drawing.
The host projects copied positions and clips segments to the district rectangle.
It drops labels outside that rectangle instead of pinning them to its edge.

The native layer is attached to the game's marker plane and inherits its map
pan, zoom, visibility and clipping. It does not replace game markers or consume
map input. The HUD minimap uses the separate target described below.
Rectangle placement in the game remains under integration review; the district
calculation matched 336 sampled native results on the reviewed build.

Publish results follow schematic drawing, with two additions: `0` means all
primitives fell outside the district (the previous native frame is cleared),
and `-6` means the context changed. A native frame expires when its lease ends,
when the projection changes, or after 500 ms without a native refresh. Renewals
are limited to one per 100 ms per owner and destination. `map_hide` clears only
that owner's native-map frame; ordinary `drawing_hide` clears its schematic.
Release, traps and unload clear every destination. Reads consume the observation allowance;
publish/hide consume the command allowance.

The route-sketch example keeps sampling, route history, names and clearing policy
in Wasm. Its bounded overview simplifies older bends instead of retaining only
a recent sliding window. Missing samples leave a break in the route. A fresh
navigation v2 sample with the same observed player/world continuity token can
restore that history after a projection gap; a changed token clears it.
Host geometry tokens alone do not make stored coordinates safe across reloads
or district changes. This example does not provide persistent saved routes.

## Native HUD minimap

Runtime `0.1.0-alpha.4.4.dev.4` adds `CRML_MAP_SONAR` and the target-aware
imports `crml_map_read_target(target, &state, sizeof(state))` and
`crml_map_hide_target(target)`. They accept `CRML_MAP_FULL` or `CRML_MAP_SONAR`;
unknown targets return `-3`. A failed read clears its output. The original
`map_read` and `map_hide` continue to operate on the full map only.

Submit the same world-space `crml_map_frame` with the selected target and that
target's current context. Each owner has one independently replaceable frame per
destination; these frames share the four-surface capacity with schematic frames.
Minimap frames retain their copied world coordinates for their original lease.
The host reprojects them using the latest native snapshot when the renderer polls;
mods do not need to republish just because the player or camera moves. Off-screen
minimap frames return `0` but retain their surface slot until hidden or expired,
so geometry can reappear when the camera moves. Camera updates never extend a
mod's lease. Explicit hide, unload and native stream expiry clear retained frames.

A minimap context marks a continuous native projection stream, not a fixed camera
pose. Normal camera/player movement does not replace it. An expired stream does.

The host uses copied player, movement-plane and camera transforms from the stock
sonar projection. The layer lives inside the existing minimap beneath its POIs,
resizes with it, inherits HUD visibility, and never accepts input. Lines are
clipped to the near-distance region rather than pinning distant route segments
to the minimap rim. Labels outside that region are omitted. No game markers are
inserted, replaced or removed.

Height styling belongs to the mod. The route example colors segments green above,
cyan within two world units of the current movement plane, and purple below. Its
named markers include signed height in world units. These describe relative
height, not engine floor numbers. The example uses the minimap when its projection
is available and retains the separate schematic as a fallback.


## Interactive full-map annotations

Runtime `0.1.0-alpha.4.4.dev.5` adds `map_annotations_publish`,
`map_annotations_next`, and `map_annotations_hide` under the `drawing` capability.
These are separate from the existing passive line/label descriptors and do not
change their ABI. The SDK types are in `crml_drawing.h`.

Publish a zero-initialized `crml_map_annotations` (5624 bytes), version 1, with a
fresh full-map context, a nonzero data revision, an origin and unit up vector,
and up to 24 uniquely identified annotations. Each annotation has a world
position, RGBA color, one ASCII symbol, a UTF-8 name (63 bytes), optional
single-line description (127 bytes), and a visibility distance (0?1000000 world
units; 0 means unrestricted). `CRML_MAP_ANNOTATION_EDITABLE` enables its editor.
`CRML_MAP_ANNOTATIONS_CREATE` permits create requests on the supplied plane when
it can be inverted through the current projection. It does not add a placement
button to the game map; native X placement remains owned by the engine. Four owners may publish annotation
sets. Renew the 600 ms lease no faster than once per 100 ms.

The native UI document renders previews, clickable annotations and a local editor
inside the full-map workflow. Ordinary map interactions remain available outside
those controls. Placement uses the supplied plane through `origin`; it does not
infer terrain, depth, a selected floor, or an entity. Singular planes reject creation.
The origin also supplies the distance reference. Coordinates and projection
contexts are transient; none establish persistent save identity.

`map_annotations_next` reads one copied `crml_map_annotation_event` (248 bytes).
The return value is 1 for an event, 0 for an empty queue, or -1 when unavailable;
non-success clears the output. Events contain the original data revision and a
create/update/delete action. Create supplies id 0 and the projected world point;
the guest assigns a stable nonzero ID. Update and delete refer to a published ID;
updates cannot move it. The guest checks the event revision, applies its policy,
and republishes a higher revision. Camera/origin updates alone need not change
that revision. A reused revision cannot replace annotation contents.

UI requests validate page nonce, replay sequence, context, owner, ID, revision,
text and bounds before admission. Each owner has a 16-event FIFO. Expiry, context
change, hide, release, trap, page replacement and unload clear pending interactions.
Publish/hide each consume one command; event reads consume one observation.
Publish uses drawing result codes, including -6 for a stale context or data revision.
Invalid guest memory or descriptor sizes trap before reaching the service.

## Metadata on native markers

Runtime `0.1.0-alpha.4.4.dev.6` adds the optional
`CRML_MAP_ANNOTATIONS_NATIVE_MARKERS` frame flag. One opted-in owner supplies the
editor for the game's six numbered X markers. Before dev.10, selection follows
numeric owner-handle order. Dev.10 uses the annotation ownership lease below.
Hovering shows a preview; clicking
opens a local editor. Native placement/removal actions remain unchanged. Before
dev.10, another opted-in owner can displace the editor when it becomes available.
Passive annotation owners remain independent.

A `CRML_MAP_ANNOTATION_ATTACH` event (4) proposes metadata for an existing native
marker. Its item has `CRML_MAP_ANNOTATION_NATIVE_MARKER | CRML_MAP_ANNOTATION_EDITABLE`,
`id` equal to the displayed native number (1?6), and `position` containing native
map-local **pixel x, pixel y, zero**, rather than world coordinates. Its distance
must be zero. The host validates and copies a presentation proposal; it does not
claim a native world-space handle, terrain height or persistent save identity.

The guest upserts the metadata and republishes a higher revision, preserving the
native-marker flag. In v1/v2, reserve IDs 1?6 when mixing native metadata with
guest-created
annotations. Native metadata is shown only when number and map position match.
Resetting details queues the existing DELETE action; it removes guest metadata,
not the game marker. The current UI offers symbol, name, description and color.
It does not offer range filtering for native markers because their world positions
are not established. Symbol and color also decorate existing native minimap
POIs when their tracking number and the full-map slot position match. Native
sonar positioning, range, elevation and clipping remain unchanged. Names and
descriptions remain on the full map.

An annotation lease can be renewed while either the full map or sonar is fresh;
editing still requires a fresh full map. Closing the full map alone does not
discard metadata. Hiding, unloading or stopping renewal removes the decorations.
Discard native attachments when the full-map context changes. Do not persist them
under the slot number alone: slots can be reused, and replacement at exactly the
same position is indistinguishable through these UI bindings. Closing, hiding,
expiry and unload remove the editor and restore the underlying marker appearance.
The Route sketch example stores native appearance metadata through the storage
capability. Its archive matches map geometry, slot and normalized position; it
is installation-wide and cannot distinguish identical markers in different saves.

## Copied affine map projection

Runtime `0.1.0-alpha.4.4.dev.7` adds `crml_map_projection_read` under `drawing`.
Pass a 64-byte `crml_map_projection`; success returns `1`, version `1`, the current
full-map context, two affine rows in `world_to_map[8]`, and a map-local pixel
rectangle `[left, top, width, height]`. Each row has three world-axis coefficients
and an offset: `uv[row] = a*x + b*y + c*z + offset`.

These are copied single-precision values. They describe normalized map geometry,
not visibility, depth picking, a floor, a world handle or a save identity. Native
pan and zoom are applied separately by the map UI. Reads use the observation
budget; unavailable/stale observations return `-1` and clear the output. Invalid
sizes or guest memory ranges trap before accessing memory. No pointers are exposed.

A mod can compare geometry when restoring presentation metadata, but equal
geometry does not prove the same campaign. Use a separately established identity
before restoring gameplay state or world-space objects across saves.


## Larger annotation collections and native placement

Runtime `0.1.0-alpha.4.4.dev.8` adds `map_annotations_publish_v2` under `drawing`.
`crml_map_annotations_v2` is 29752 bytes, version 2, with up to 128 entries. The
original 5624-byte, 24-entry descriptor and import remain unchanged. Both versions
use the same owner lease, edit queue, IDs and revisions. V2 world annotations also
render through the copied native minimap projection, independently of route lines.

`CRML_MAP_ANNOTATIONS_PLACE_ACTION` opts into the supported adapter's native map
placement action. When the six stock slots are full, combining it with
`CRML_MAP_ANNOTATIONS_CREATE` queues a normal create event on the supplied plane.
The guest assigns the ID, decides what to store and republishes. The same action
on a hovered, visible editable world annotation queues deletion by its published
owner and ID. The current placement owner must hold `CRML_MAP_ANNOTATIONS_PLACE_ACTION`.
Selection expires after 200 ms unless the UI renews it. The action also checks
the current client-space pointer against the clipped marker hit rectangle; leaving the marker, opening
an editor, changing context or losing focus releases selection. The native add
and remove paths both honor this selection, so a stock marker beneath it is not
removed instead. Without a selected annotation, the existing nearby-point
selection remains available for native map navigation. Among fresh opted-in
owners before dev.10, the lowest runtime owner ID claims the action, matching native marker
editor arbitration. That ID is a runtime allocation, not a persistent priority;
dev.10 uses the annotation ownership lease below.
Repeated actions at the same pending marker are coalesced. A hovered selection
consumes the action even when its event queue is full, so backpressure cannot
remove a stock marker underneath; retry after the guest drains its queue. Other actions for
a full or temporarily busy owner fall back to stock handling; the action is not
redirected to a competing mod. Coordinate unrelated placement mods explicitly.
Queued events are not durable saves. A full collection refuses overflow placement.

The adapter checks the executable profile, function bytes and exact caller. It
copies map cursor coordinates and the six-slot occupancy state; it never enlarges
the game's stack array or writes mod records into its save database. The stock six
markers retain stock serialization. Additional marker persistence is guest policy,
as shown by `examples/route-sketch/marker-store.h` using the bounded storage API.
The example matches map geometry rather than save identity; see its README for
cross-save behavior and asynchronous write completion.

## Separate world annotations and native attachments

Runtime `0.1.0-alpha.4.4.dev.10` adds `map_annotations_publish_v3`,
`map_annotations_next_v2`, and `map_annotations_status` under `drawing`. Set a
zero-initialized `crml_map_annotations_v3` to version 3 and size 31104 bytes. It
has at most 128 world items and six native attachments; `count + attachment_count`
must also be at most 128. Its `reserved` field must be zero. The full-map
`context`, nonzero data `revision`, creation `origin` and unit `up` retain their
meaning. The existing 24-item v1 and 128-entry v2 descriptors, imports, event
layout and ABI 1 remain available. V3 retains the four-owner capacity, 600 ms
renewal, at most one publication per owner per 100 ms, drawing result codes and
the existing command allowance.

Each `crml_map_world_annotation` is 232 bytes. Its nonzero `id` is chosen by the
guest and is unique among world items. `world_position[3]` and `distance` are in
world coordinates; zero distance means unrestricted. Only
`CRML_MAP_ANNOTATION_EDITABLE` is valid in its `flags`. Symbols, names,
descriptions and colors follow the existing annotation rules. A
`crml_map_native_attachment` is 224 bytes and addresses an existing stock X
marker through a unique `native_slot` 1..6. Its `map_pixels[2]` are **native map-local
pixels**, not a world point. Its `reserved` must be zero and only `EDITABLE` is
valid in `flags`. The slot is a separate namespace from guest world IDs: a guest
may use world ID 1 and attach metadata to native slot 1 in the same frame.
Attachments do not create, remove or extend the game's six stock marker slots.

`map_annotations_next_v2` reads a 488-byte `crml_map_annotation_event_v2` with
version 2. It reports the original `revision` and `context`, the action and a
`target` of `CRML_MAP_ANNOTATION_TARGET_WORLD` or
`CRML_MAP_ANNOTATION_TARGET_NATIVE`. For a world target, `world` is active and
`attachment` is zero; a CREATE proposal has world ID zero until the guest assigns
one. For a native target, `attachment` is active and `world` is zero. An ATTACH
proposal names the existing native slot, not a guest ID. The guest checks the
event's revision and context, applies its policy and republishes a higher data
revision. The event queue remains owner-local and bounded to 16 entries. The
legacy `map_annotations_next` refuses a v3 owner's queue with `-3`, leaving its
events intact; `map_annotations_next_v2` likewise returns `-3` for a legacy
owner. Use the reader matching the published descriptor. Otherwise, reads retain
the legacy results: `1` for a copied event, `0` for none, `-1` unavailable.

`map_annotations_status` is an observation and copies a 32-byte
`crml_map_annotation_status` with
version 1. It returns `1` for a fresh annotation lease owned by the caller,
`0` when the caller has no fresh lease and clears the output, or `-1` when
unavailable. `requested` and `granted` contain only the
`CRML_MAP_ANNOTATIONS_NATIVE_MARKERS` and
`CRML_MAP_ANNOTATIONS_PLACE_ACTION` bits. `requested` reflects the owner's
latest opt-ins; `granted` shows which interactions it currently owns. `context`
and `revision` identify that owner's current annotation publication. CREATE is
the guest's own creation policy and is not an ownership grant.

Native-marker attachment editing and native map placement have separate owners.
The first successful opt-in arrival for each interaction receives its lease;
later publishers cannot take it merely by publishing. Waiting publishers may
continue showing their world annotations and can read `status` to see contention.
After an owner releases, expires or opts out, waiting owners take over in arrival
order. Ownership is independent of numeric runtime owner IDs. The status reports
an interaction lease, not successful rendering or a persistent marker identity.
The adapter still uses the game's six-slot native database, and neither map
geometry nor slot number proves campaign identity.
