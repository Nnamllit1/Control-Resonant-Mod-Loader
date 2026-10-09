# Route sketch

A Wasm example combining navigation snapshots, editable names and a drawing surface
inside the game's native UI document. Recording, naming and schematic projection
live in the guest source; the host projects world coordinates for the full map and HUD minimap.
It does not change movement or collision. Map editing temporarily owns map input.

Requires runtime `0.1.0-alpha.4.4.dev.10` or later and the reviewed navigation adapter.
Build with `python tools/mod.py build examples/route-sketch --output build/mods/route-sketch`, then install the output
folder beneath `crml/mods/` as described in the SDK installation guide.

Use **Options → Mods → Route sketch** to show or clear the route. Open **M** to
create and edit markers directly on the map.
Up to 128 CRML marker entries and a
128-segment route overview are retained. Straight forward sections are merged.
When the overview fills, the least significant older bends are simplified while
keeping the beginning and the most recent 24 points. Long routes become less
detailed; this is not a lossless recording. Disconnected sections remain separate.
If too many short disconnected sections fill the budget, the oldest section is
removed and the reason is logged. The route has a brighter recent end and a
rounded stroke. The sketch is 80 world units across, centered on the player and
projected onto the current movement plane when the native minimap is unavailable.
Height is shown relative to that plane: green above, cyan within two world units,
purple below. Named markers include signed height in world units. Overlapping
floors can still overlap geometrically; colors are not engine floor identifiers.
The fallback sketch clips lines at its view boundary, retaining visible portions
even when an endpoint is outside the view.

## Markers on the full map

Use the game's **X - Place Marker** action. The first six slots keep their stock
behavior. Once they are occupied, the same action creates additional CRML markers.
Click a marker to change its symbol, name, description and color. Extra markers
appear on the full map and native minimap; **Delete** removes them. Hover an extra marker and use the game's marker action (**X** by default) to remove it. There is no second placement button.

The collection holds 128 CRML entries total: world markers and details attached to
the six stock slots. World marker IDs and stock slot numbers have separate identities.
Saved entries across all map geometries share a 128-record
archive. Full collections refuse new placement without replacing old markers.
Placement requires a fresh map projection and a previously observed player height
plane. It uses that plane, not terrain picking or the map's selected floor.

Existing stock markers remain editable. **Reset details** restores a stock marker's
original appearance without deleting it. Their native positions are not verified
world coordinates, so their editor has no distance field. Extra markers have world
positions and an optional visibility distance; 0 means unrestricted.

The editor uses the game's map-card styles and replaces its tooltip while open.
Scroll within the card to reach Save and Cancel. While editing, map actions and
parent-menu shortcuts pause so typing does not move the map or change tabs.
Closing the map discards an unfinished draft. Unsupported adapters leave stock
placement unchanged. Unloading the mod removes its extra markers from view;
the six game-owned markers remain in the game save.

The settings page also offers a temporary player-position marker shortcut. These
markers are not saved until edited through the map editor.

To remove this mod's markers without clearing the route, turn on **Confirm marker
deletion**, then turn on **Delete Route sketch markers**. The action resets both
switches, removes the mod's temporary and saved world markers, and clears stock X
appearance details stored by this mod across all map geometries. It leaves the
game's six stock X markers in place; remove those with the game's X map action.
If confirmation is off, the action resets without deleting anything. Wait for the
save receipt before exiting so the empty marker archive is committed.

## Saved markers

Version 0.6.0 uses the typed map annotation API for world markers and stock marker
attachments. It keeps the version 1/2 storage format, so 0.5.0 extra marker data
and older 0.4.0 stock metadata remain readable without rewriting the archive.
No extra records are inserted into the game's save format.

Wait for **Marker details saved** before exiting. Writes are asynchronous;
closing the game while Saving is pending can lose the latest edit. Delete and
Reset details also update the archive. Large collections restore in small batches
so loading them stays within the Wasm execution budget.

Restoration matches copied map geometry. Stock metadata additionally matches the
native marker number and normalized map position. **Storage is not save-slot-aware:**
saves sharing map geometry share extra markers, and stock markers at the same
position can share details. A reused stock slot at exactly the same position is
indistinguishable from the old marker. Corrupt or unsupported archives are left
untouched; the mod reports the problem in its log.

The route remains a session-only API example. Clear route removes the trail and
temporary world markers; saved map markers and stock-marker details remain. Use **Reset details**
in a stock marker's editor, or Delete on an extra marker, to remove saved data. Hiding clears temporary
history and hides all decorations until the mod is shown again. Missing/stale observations hide
the schematic immediately. A reported teleport, clock rollback, or an uninterrupted
sampled jump over 20 units clears the stored points. A change in the observed
player/world continuity token always clears them. A sample gap preserves history
when observations resume with the same continuity token. The next section starts
separately, so movement during the gap is not invented as a straight connector.
Named markers survive those gaps too.
Those checks cannot establish every zone/frame transition.
Stable campaign and coordinate-frame identities are still unavailable in this API.

The guest publishes at 5 Hz with a 600 ms drawing lease. Hiding clears host state on
the next guest tick; pixels disappear on the next native UI poll or lease expiry.
Neither host acceptance nor automated tests establish native renderer performance or
ground orientation during actual gravity transitions.


The same guest also submits its temporary
world-space trail and named markers to the native full map. Open M after moving
to see it; native pan and zoom apply to the layer. During gameplay the route is
drawn inside the stock HUD minimap beneath its icons, using its native projection.
Only the near-distance region is shown; distant sections are clipped instead of
being pushed onto the rim. The separate schematic is hidden while this destination
is available and is used as a fallback otherwise.

Opening the map may pause movement samples. An already-associated route remains
visible while the same native player-map projection continues to refresh. Layout
resizing does not change that context. If the native projection stream expires or
changes, rendering waits for a fresh player sample. The old route is retained
only if that sample confirms the same observed player/world lifetime. A new
context cannot adopt stale coordinates. This is session continuity,
not a persistent world ID, and does not support saved routes across reloads.
