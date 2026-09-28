---
description: Download CONTROL Resonant Mod Loader, install compatible mods, create Wasm mods, and explore Northlight engine research.
---

# CONTROL Resonant Mod Loader

<p class="crml-label">Windows x64 · Experimental release</p>

<p class="crml-intro">Install mods for CONTROL Resonant with CRML, a shared runtime for sandboxed WebAssembly mods. Download the runtime, add the mods you want, and start the game normally. No compiling is needed to play.</p>

[Download and install](installation.md){ .md-button .md-button--primary }
[Create a mod](developing.md){ .md-button }

## Install CRML

Download **`crml-runtime-<version>-windows-x64.zip`** from [GitHub Releases](https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases). Extract it and copy the contents beside `CONTROLResonant.exe`. Then install compatible mods following their own instructions.

The runtime is the shared loader. Each mod supplies its own features and controls. The **SDK** download is for writing mods; players only need the runtime and their chosen mods.

The [installation guide](installation.md) shows the exact filenames, folder layout, updates and removal. Releases support the game build listed in their compatibility information; a game update may require a matching CRML release.

## Make a mod

Use CRML's SDK, shared runtime and bounded host API to build your own gameplay behavior. Mod source examples cover movement, visibility and physics operations. Each Wasm mod has its own memory and execution budget; available operations depend on the supported API and runtime mode.

[Create your first mod](developing.md) · [API reference](api.md) · [Sandbox limits](sandbox.md)

## Contribute and explore Northlight

The project publishes research on assets, scripting, entities, rendering and physics. It is also useful to native mod and tool developers independently of CRML. The reference maps distinguish recovered engine internals from supported mod APIs.

[Engine research](engine-research.md) · [Build from source](building.md) · [Runtime architecture](architecture.md)

Independent community project. Not affiliated with Remedy Entertainment.
