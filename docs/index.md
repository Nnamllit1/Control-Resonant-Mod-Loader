# CONTROL Resonant Mod Loader

<p class="crml-label">Developer preview · Windows x64 · WebAssembly</p>

<p class="crml-intro">A mod framework for CONTROL Resonant. Build small, sandboxed mods against a versioned API, with a native loader handling the connection to the game.</p>

!!! warning "Early development"
    The sandbox and proxy pass automated tests, and the hello mod has loaded in the recorded game build. An opt-in noclip prototype and status overlay are available for testing. Actual wall traversal and restoration remain unverified, as do controller input and clean shutdown.

## Start here

- **Try a package:** [build and run the example](installation.md).
- **Write a mod:** [getting started](developing.md) and [API reference](api.md).
- **Understand the boundary:** [sandbox limits](sandbox.md) and [architecture](architecture.md).
- **Help connect the game:** [gameplay and noclip milestone](gameplay.md).

## What runs today

The standalone host discovers Wasm packages, validates their manifests, calls their lifecycle functions, and isolates guest traps. Each mod has its own memory and execution budget. The first example writes a greeting through the host logging API.

The experimental Windows proxy forwards XInput calls to the system library and starts the trusted runtime from the first `XInputGetState` call. Startup and the hello greeting have been observed in the build recorded in `compatibility.json`. This confirms the loading route, not full gameplay compatibility.

## Mod format

```text
mods/
  hello/
    mod.ini
    hello.wasm
```

Mods use a deliberately small host API. They do not load native DLLs or inherit filesystem, network, process, or game-memory access. Future gameplay and UI functions must pass through the same capability boundary.

Independent community project. Not affiliated with Remedy Entertainment.
