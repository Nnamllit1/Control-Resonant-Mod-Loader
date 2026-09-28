---
description: Understand the CONTROL Resonant loader, trusted native runtime, Wasm sandbox, SDK, and boundary between mods and game internals.
---

# Architecture

```text
CONTROLResonant.exe
  -> xinput1_4.dll proxy
       -> Windows System32 XInput (forward original calls)
       -> crml/crml_runtime.dll (trusted worker)
            -> Wasmtime
                 -> isolated Wasm stores
                      -> versioned capability API
```

## Loader

`loader/` contains the Windows entry point, export table, and x64 forwarding stubs. Static inspection of the recorded executable found an ordinal-2 import from `XINPUT1_4.dll`. The proxy preserves the named API ordinals and additional unnamed exports observed on the development system.

`DllMain` does no initialization. The first call to `XInputGetState` resolves the original library by absolute System32 path and schedules a worker. Forwarders preserve integer, vector, and stack arguments. The worker loads the trusted runtime with a restricted DLL search path. Both modules remain resident until process exit.

The proxy and runtime remain loaded for the lifetime of the game process. See [troubleshooting](troubleshooting.md) for startup problems.

## Runtime and SDK

`runtime/` owns discovery, validation, Wasmtime stores, budgets, logging, and lifecycle. `sdk/` defines the guest contract independently from the Windows loader. `crml_host` embeds the same core as the DLL, so sandbox behavior can be tested without launching the game.

The isolated Wasm physics service accepts bounded commands on that worker and consumes them after the native physics dispatcher returns. A mod owns an opaque selection token, never an engine pointer. The service resolves the current entity/body generation again before every access and retains restoration state independently of the guest. Mod traps and shutdown request cleanup without calling guest code from an engine hook. Only one mod can own the operation at a time. See the [damping API](api.md#experimental-prop-damping) for limits and result semantics.

Packages load in directory-name order. One failed package does not stop the rest. The current runtime is single-threaded, has no hot reload, and has no guest-to-guest shared memory.

## Game bridge

The opt-in experimental bridge observes the character-controller routine on the thread used by the game. Wasm remains on the runtime worker and requests an owner-bound movement lease. The hook validates the current player, substitutes private per-call arguments, and forwards the original routine. It never runs Wasm inside the hook. Unknown builds and mismatched arguments leave movement untouched.

The runtime worker publishes the panel's visibility and status. Native DirectX 12 hooks copy a cached text texture into the current back buffer before presentation. Queue selection requires observed transitions of that specific buffer; unrelated queue submissions cannot select it. Each buffer has its own commands and completion fence, and resize hooks release retained buffers after completion. The renderer follows the [DirectX 12 presentation state requirements](https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains).

Native code owns input, movement and UI resources; guests receive status codes and opaque handles, never raw addresses. The current movement API accepts a bounded world-space velocity, with controls and speed selection in the guest. The legacy native noclip helper accepts a speed parameter instead. The probe records up to 600 diagnostic snapshots and then stops logging. The pinned movement hook remains a pass-through when the feature is off.

See the [legacy native noclip prototype](gameplay.md) for that implementation, or the [Wasm movement example](movement.md) for guest controls and API ownership. Players should use the [runtime installation guide](installation.md) and their mod's own instructions.

The [engine research tools](engine-research.md) map Pack2 assets, candidate script bindings, and ECS system metadata without loading the runtime into the game.
