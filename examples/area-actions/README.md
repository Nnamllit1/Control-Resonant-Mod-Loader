# Area actions

A Wasm example that requests area-restriction exceptions through
`player.action_rules`. All policy and settings live in `area-actions.c`.

Requires runtime `0.1.0-alpha.4.4.dev.11` or later and a supported game build.
Build with `python tools/mod.py build examples/area-actions --output build/mods/area-actions` from the SDK root.
Install the resulting package as described by the command's output.

In **Options > Mods > Area actions**, enable **Allow actions in story areas**,
then select the actions to include. The master setting starts off each session;
this example does not persist preferences. Disable it to release the request.

Requests expire if the mod stops renewing them. They do not unlock powers,
force attacks, or waive conversation, quest, flashback and other restrictions.
The API accepts requests independently of whether an action can execute.

See `docs/action-rules.md` for the contract and current adapter coverage.
