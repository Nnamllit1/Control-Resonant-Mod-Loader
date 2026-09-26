---
description: Build sandboxed Wasm mods for CONTROL Resonant and explore public Northlight engine research on assets, scripting, rendering, entities, and physics.
---

# CONTROL Resonant Mod Loader

<p class="crml-label">Developer preview · Windows x64 · WebAssembly</p>

<p class="crml-intro">CONTROL Resonant Mod Loader (CRML) is a modding framework for CONTROL Resonant, pairing sandboxed WebAssembly mods with a trusted native runtime. The project also publishes Northlight engine research covering assets, scripting, rendering, ECS entities, and physics.</p>

!!! warning "Early development"
    CRML is a developer preview for the game build listed in `compatibility.json`. It includes a sandboxed mod runtime, a hello example, and an opt-in noclip prototype with a status overlay. See the [noclip guide](gameplay.md) for controls and known limitations.

## Start here

- **Try a package:** [build and run the example](installation.md).
- **Write a mod:** [getting started](developing.md) and [API reference](api.md).
- **Understand the boundary:** [sandbox limits](sandbox.md) and [architecture](architecture.md).
- **Use noclip:** [gameplay and noclip](gameplay.md).
- **Research the engine:** [subsystem atlas](engine-atlas.md), [system index](engine-system-index.md), and [reviewed operation paths](engine-paths.md).

## What runs today

The standalone host discovers Wasm packages, validates their manifests, calls their lifecycle functions, and isolates guest traps. Each mod has its own memory and execution budget. The first example writes a greeting through the host logging API.

The experimental Windows proxy forwards XInput calls to the system library and starts the trusted runtime from the first `XInputGetState` call. The executable fingerprint in `compatibility.json` determines which game build the installer accepts.

## Northlight engine research for mod developers

The [Northlight research guide](engine-research.md) is useful independently of the loader. Native DLL mod authors, tool developers, and reverse engineers can use the same public format notes, subsystem index, and fingerprinted reference maps. Findings distinguish static evidence, live observations, and unresolved behavior; they are not an official engine SDK.

- [Pack2 asset format](pack2.md): archive indexes, resource records, and reproducible inspection tools.
- [Engine atlas](engine-atlas.md): recovered ECS systems and script-binding candidates.
- [Engine internals](engine-internals.md): resource ownership, entity identity, and physics-body relationships.
- [Physics dynamics](physics-dynamics.md): forces, mass, inertia, damping, and simulation paths.
- [Reviewed engine paths](engine-paths.md): scripting, rendering, collision filters, and other subsystem operations.

## Wasm mods and native DLL loaders

CRML uses a native XInput proxy to start its runtime, while mod packages contain sandboxed `.wasm` modules. It does not load third-party native mod DLLs. Mod authors use the [versioned host API](api.md) and declared capabilities. Research published here can inform native integrations, but offsets and compatibility findings apply to the recorded game builds, not every Northlight game.

## Mod format

```text
mods/
  hello/
    mod.ini
    hello.wasm
```

Mods use a deliberately small host API. They do not load native DLLs or inherit filesystem, network, process, or game-memory access. Available gameplay functions use the same capability boundary.

Independent community project. Not affiliated with Remedy Entertainment.
