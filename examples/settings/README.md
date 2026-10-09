# Typed settings example

Registers four mod-owned preferences, including an editable marker name, in the native Options > Mods tab on supported
game builds. It changes no gameplay. Adjusting a control updates the guest's next
settings snapshot and writes `Settings snapshot changed` to the log.
Subsequent changes also request a passive `Preferences updated.` message through
the `feedback` API. Rapid edits are coalesced while a message is active or the
service is rate-limited. No message is requested for the initial defaults.

Requires runtime `0.1.0-alpha.4.4.dev.1` or later. Build from the repository or
extracted SDK root:

```powershell
python tools/mod.py build examples/settings --output build/mods/settings
python tools/mod.py check build/mods/settings
```

Copy the built package into `crml/mods/settings`. The runtime prepares the native
settings page from the manifest; no diagnostic marker is required. UI availability
still depends on a supported renderer/game build. The standalone host validates
the guest and its defaults without displaying the game UI.

Use the mouse to toggle the Boolean or drag the numeric tracks. Type a name and
press Enter or Apply to commit it; Escape discards the draft. The marker name is
only a preference: this example does not place a game marker. These example
values last for this loaded mod instance; restarting restores defaults. Settings
are not automatically saved. Use the separate `storage` capability for persistent
preferences and keep the saved schema independent of transient setting handles.

See [typed settings](../../docs/mod-settings.md) for return codes, revisions,
ownership and the native UI's current limitations.
The [feedback contract](../../docs/mod-feedback.md) explains delivery receipts and
why an accepted message is not yet confirmed as presented.
