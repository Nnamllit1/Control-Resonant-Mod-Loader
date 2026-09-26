---
description: Inspect the CONTROL Resonant player entity, component identities, and movement state using the experimental read-only diagnostic.
---

# Player entity inspector

The experimental inspector records the player entity's component identities and known movement state without requesting gameplay changes when used alone. The separate `visibility.enabled` opt-in permits the visibility experiment during the same capture. It is the first step toward mapping live model, material, and physics relationships. It currently identifies selected rendering components by hash; it does **not** decode their payloads or resolve model/material resource names.

Build and preview an update with:

```powershell
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
.\build.bat -ExperimentalGameplay -Test
python tools/install.py "$gameDir" --update --entity-inspector
```

With the game closed, add `--apply` to install. For a fresh installation omit `--update`. The installer adds `crml/entity-inspector.enabled`. While that file is present, inspector mode takes precedence over `noclip.enabled`: the movement hook forwards the original arguments, and noclip input, recovery, and overlay hooks are not started. No Wasm mod is required to collect observations.

Launch normally, load a playable save, and walk for around 30 seconds. The file `crml/entity-inspector.jsonl` contains a fingerprinted header followed by observations, at most once per second for approximately ten minutes. Each observation includes:

- Timestamp and sampling thread, generation-bearing entity identity, archetype and row.
- Up to 2048 component hashes, with selected known names. Larger inventories are rejected rather than partially reported. `truncated` remains in the log format for compatibility with older captures.
- Transform and controller positions, controller-disabled, keyframed and teleported flags.

Samples are copied immediately before the original player-controller update. The worker serializes copied values; it never follows game pointers. Invalid generation/location checks or unreadable memory produce an invalid, cleared snapshot. Missing player calls produce no new snapshot; the last timestamp must not be mistaken for current state. Logs are overwritten on each launch.

Only the world component table validated by the existing movement probe is read. Other accessor layouts remain unresolved. Unknown hashes remain unnamed, and rendering presence does not imply a decoded render handle. The inspector reports the player only; related model entities, rigid-body shapes, mass, gravity, and GPU bindings are not yet mapped. There is no in-game inspector panel in this version.

When reporting an inspector issue, preserve the JSONL file before restarting the game. Include the actions that caused the issue and whether it occurred during movement or a loading transition.

To return to the previous mode, close the game and remove `crml/entity-inspector.enabled`. The installer will subsequently detect the missing owned marker; restore it before a receipt-validated update or uninstall.

## Component identities

FNV-1a hashes of fully qualified type names in the executable's ECS signatures can be matched to captured component hashes. A name match identifies a candidate type; it does not determine the payload layout or provide a callable API.

| Hash | Candidate type in `coregame::component` | Relationship |
| --- | --- | --- |
| `6f477177` | `MeshResourceID` | Asset identity to mesh resource |
| `eece657a` | `MeshResource` | Resource ownership and lifetime |
| `355cf83d` | `MeshMaterialSet` | Mesh to materials |
| `4a8e27fd` | `PhysicsResourceID` | Asset identity to physics resource |
| `e1da5fb1` | `CollisionResource` | Collision resource ownership |
| `167fac8a` | `CharacterControllerBody` | Controller to physics body |

Component membership can change without replacing the entity. Use the current component set and generation when interpreting each sample.
