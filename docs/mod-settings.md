---
description: Register typed mod preferences and read validated snapshots from Wasm.
---

# Typed settings

Development builds after Alpha 4.3 provide the experimental `settings` capability.
A mod registers its own Boolean, integer, numeric and short text preferences. The host validates
values and, on supported game/renderer builds, displays them in Options > Mods.
Changing a preference does not invoke any gameplay feature automatically: the guest
reads the value and chooses its behavior.

Use the manifest's optional `name` for the page heading. The package ID remains
the identity used for ownership and storage; `version` and `author` provide
additional display metadata and appear in startup diagnostics.

```ini
id=my-mod
abi=1
module=my-mod.wasm
min_runtime=0.1.0-alpha.4.4.dev.1
capabilities=settings
```

The complete C example is in `examples/settings`. Build it with:

```powershell
python tools/mod.py build examples/settings --output build/mods/settings
python tools/mod.py check build/mods/settings
```

## Register a schema

`crml_setting_definition` is a 360-byte version-1 structure declared in
`crml_settings.h`, included by `crml.h`. Zero-initialize it, then specify:

| Field | Contract |
| --- | --- |
| `version` | `1` |
| `kind` | `CRML_SETTING_BOOL`, `CRML_SETTING_INT` or `CRML_SETTING_NUMBER` |
| `key` | Stable key, 1–31 lowercase ASCII letters, digits, `_` or `-` |
| `label` | Nonempty UTF-8 label, at most 95 bytes |
| `description` | Optional UTF-8 explanation, at most 191 bytes |
| `initial` | Default value, within range and on the step grid |
| `minimum`, `maximum` | Finite bounds within ±1,000,000,000; minimum strictly below maximum |
| `step` | Positive finite increment; both endpoints on the grid; at most 1,000,000 intervals |

All strings must be NUL-terminated and contain no ASCII control characters.
Boolean definitions use minimum `0`, maximum `1`, step `1` and default `0` or `1`.
Integer bounds, step and default must be integral. A step too small to distinguish
reliably at the chosen numeric magnitude is rejected. Numeric comparisons allow
floating-point roundoff; they do not accept arbitrary off-grid values.

`crml_settings_register(&definition, sizeof(definition))` copies the definition and
returns a handle from 1 through 32. Handles are local to that loaded mod instance;
another mod registering the same key or receiving the same numeric handle has its
own independent value. Register definitions during initialization. Duplicate keys
are rejected rather than silently changing a schema.

## Read and change values

`crml_settings_read(values, sizeof(values))` returns one atomic snapshot of all this
mod's settings, in handle order. Each 24-byte `crml_setting_value` contains a handle,
kind, double value and 64-bit revision. Allocate up to `CRML_SETTINGS_MAX` entries;
the result is the number filled. Revisions start at 1 and increase only when a
value changes. One snapshot costs one host call, regardless of setting count.

`crml_settings_set(handle, value, expected_revision)` applies a validated value.
Use the observed revision to reject an intervening edit, or `0` for an explicitly
unconditional update, such as restoring saved preferences during initialization.
The native UI always supplies an observed revision. If another writer changes the
value while a slider is being edited, the stale edit is rejected and the UI refreshes.

| Result | Register | Read | Set |
| --- | --- | --- | --- |
| Positive | New handle | Number of entries | `1`: value changed |
| `0` | — | No registered settings | Value already equal |
| `-1` | Service unavailable | Service unavailable | Service unavailable |
| `-2` | — | — | Unknown handle |
| `-3` | Invalid definition | Output too small | Invalid value |
| `-4` | Duplicate key | — | Stale revision |
| `-5` | Capacity/resource failure | Resource failure | Revision/resource failure |

Invalid memory or structure sizes trap before registry access. Read capacity is in
bytes, at most 768, and must be a multiple of 24. Failed reads leave output unchanged.
Settings imports have a separate budget of 64 calls per invocation; they do not
consume the gameplay observation or command allowances. Each runtime supports at most 32 mod
owners with 32 settings each. Guest traps and unload remove that owner's controls.

## Text values

Text imports require `min_runtime=0.1.0-alpha.4.4.dev.1`. They use the same
`settings` permission, 32-control/key/handle namespace, and 64-call allowance as
numeric settings. Existing numeric layouts and imports are unchanged.

```c
crml_text_setting_definition name = {
    .version = 1, .max_bytes = 80,
    .key = "marker_name", .label = "Marker name", .initial = "Return here"
};
int32_t handle = crml_settings_text_register(&name, sizeof(name));
crml_text_setting_value value;
if (handle > 0 && crml_settings_text_read((uint32_t)handle, &value, sizeof(value)) == 1) {
    /* value.value is copied UTF-8; value.length excludes the NUL. */
}
```

The version-1 definition is 584 bytes. `max_bytes` is 1 through 255, excluding
the terminating NUL; `initial[256]` must be terminated. Key, label and description
follow the numeric descriptor rules. Values may be empty and must be valid
single-line UTF-8, without embedded NUL, C0/C1 controls, U+2028 or U+2029.
The host validates byte length, including multibyte characters.

`settings_text_register` returns a handle or the numeric registration errors.
`settings_text_read(handle, output, 280)` returns `1` with version, handle,
revision, byte limit, length and a terminated `value[256]`. It returns `-1` for
unavailable, `-2` for unknown handle, `-3` for a numeric handle and `-5` for host
failure. Failures leave output unchanged. `settings_read` includes text handles
with kind `CRML_SETTING_TEXT`, revision and numeric value `0`; fetch their text
separately when needed. These separate reads are not one atomic cross-call snapshot.

`settings_text_set(handle, bytes, length, expected_revision)` copies exactly the
specified UTF-8 bytes, without a terminator. Results and revision policy match
numeric `settings_set`. Invalid guest memory, structure sizes or a length above
255 trap before registry access; invalid content returns `-3`. A numeric handle
cannot be written through the text API, or vice versa.

## Native UI and persistence

The native page uses the game's existing Options context and visual classes. It
does not push an unregistered menu state or release Options input ownership.
Controls are grouped by manifest ID. Previous mod / Next mod remain available,
and the mod picker shows the current mod and lets you search installed mods by
display name or package ID for direct selection. The collapsed picker shows the
display name; results show the package ID as secondary text when the name differs.
The picker has a bounded, scrollable result list; a search with no matches does
not change the selected mod. Escape closes the picker first. Search text is only
a local UI filter and does not change a mod setting.
The page supports mouse toggles, slider drags and text fields. Text drafts remain
while focused or blurred; Enter or Apply commits, and Escape discards the draft
and reloads the current value. A concurrent revision blocks a stale text commit.
IME composition does not itself commit. Changing mods or leaving the page discards
unsubmitted drafts and releases focus. General keyboard/controller
navigation of mod controls remains unsupported. Edits to a pending control are
temporarily disabled until the host responds. Closing or replacing the page releases
its scoped listeners and visual selection.

The registry works in the standalone host and deterministic simulator even without
a game renderer. `CRML_CAP_SETTINGS` means the registry is available, not that the
game has successfully displayed a Mods tab. Native presentation requires the bundled
`crml/native-ui-panel.html` and reviewed game/UI signatures. No diagnostic marker is
needed for a mod declaring `settings`. Unsupported builds retain guest defaults and
programmatic setting access; check the runtime log for renderer availability.

Values are **not automatically persisted**. Use the separate [storage API](mod-storage.md)
when a mod should remember preferences. Store stable keys or a versioned guest
schema, not transient handles or revisions. Validate a saved schema before applying
it, and check asynchronous save completion. Removing and reloading a mod creates
fresh settings initialized to its defaults unless the guest restores them.

Automated checks cover the registry, Wasm boundary, stale edits, renderer protocol,
mouse behavior, menu ownership and cleanup in an authored browser fixture. The
new text controls still need a live game check for renderer focus, IME and input
ownership; browser results do not establish those native behaviors.

## Dynamic rows and search

Use the separate [list service](mod-lists.md) to show changing rows with stable
IDs, availability and activation events inside the owning mod's Options group.
Numeric setting indices do not identify a row safely when a catalog reorders.
A text setting can hold a search query; the guest filters its own data and
publishes the resulting list. A numeric preference can control the list's bounded
font scale. Settings and list reads are separate observations, not an atomic
model snapshot; retain the model associated with each published list revision.
