# Player entity inspector

The experimental inspector records the player entity's component identities and known movement state without requesting gameplay changes when used alone. The separate `visibility.enabled` opt-in permits the visibility experiment during the same capture. It is the first step toward mapping live model, material, and physics relationships. It currently identifies selected rendering components by hash; it does **not** decode their payloads or resolve model/material resource names.

Build and preview an update with:

```powershell
.\build.bat -ExperimentalGameplay -Test
python tools/install.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant" --update --entity-inspector
```

With the game closed, add `--apply` to install. For a fresh installation omit `--update`. The installer adds `crml/entity-inspector.enabled`. While that file is present, inspector mode takes precedence over `noclip.enabled`: the movement hook forwards the original arguments, and noclip input, recovery, and overlay hooks are not started. No Wasm mod is required to collect observations.

Launch normally, load a playable save, and walk for around 30 seconds. The file `crml/entity-inspector.jsonl` contains a fingerprinted header followed by observations, at most once per second for approximately ten minutes. Each observation includes:

- Timestamp and sampling thread, generation-bearing entity identity, archetype and row.
- Up to 2048 component hashes, with selected known names. Larger inventories are rejected rather than partially reported. `truncated` remains in the log format for compatibility with older captures.
- Transform and controller positions, controller-disabled, keyframed and teleported flags.

Samples are copied immediately before the original player-controller update. The worker serializes copied values; it never follows game pointers. Invalid generation/location checks or unreadable memory produce an invalid, cleared snapshot. Missing player calls produce no new snapshot; the last timestamp must not be mistaken for current state. Logs are overwritten on each launch.

Only the world component table validated by the existing movement probe is read. Other accessor layouts remain unresolved. Unknown hashes remain unnamed, and rendering presence does not imply a decoded render handle. The inspector reports the player only; related model entities, rigid-body shapes, mass, gravity, and GPU bindings are not yet mapped. There is no in-game inspector panel in this version.

For feedback, preserve the JSONL file after walking, jumping, and a normal loading transition. Check that timestamps advance, records are valid, and identities change appropriately across reloads. Synthetic tests validate reads and rejection paths. An observed gameplay capture on the supported executable produced 51 valid samples over 50 seconds, with 524?531 component hashes per sample and no truncation. Movement and an archetype change on the same entity were observed. Reload and entity recreation have not been verified.

To return to the previous mode, close the game and remove `crml/entity-inspector.enabled`. The installer will subsequently detect the missing owned marker; restore it before a receipt-validated update or uninstall. Prefer retaining inspector mode during engine research.

## Component mapping leads

Matching captured hashes with FNV-1a hashes of fully qualified type names in the executable's ECS signatures produced 451 candidate names out of 531 observed identities. These matches identify investigation targets, not validated payload layouts or callable APIs.

| Hash | Candidate type in `coregame::component` | Investigation target |
| --- | --- | --- |
| `6f477177` | `MeshResourceID` | Asset identity to mesh resource |
| `eece657a` | `MeshResource` | Resource ownership and lifetime |
| `355cf83d` | `MeshMaterialSet` | Mesh to materials |
| `4a8e27fd` | `PhysicsResourceID` | Asset identity to physics resource |
| `e1da5fb1` | `CollisionResource` | Collision resource ownership |
| `167fac8a` | `CharacterControllerBody` | Controller to physics body |

The observed archetype transition added seven identities, with no removals. Candidate names associate them with cloth state, shield effects, Lua streaming, and an active material override. This observation does not establish which subsystem caused the transition. Follow the corresponding native accessors and lifecycle systems before reading these payloads or exposing operations to mods.
