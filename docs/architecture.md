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

`DllMain` does no initialization. The first forwarded XInput call resolves the original library by absolute System32 path and schedules a worker. Forwarders preserve integer, vector, and stack arguments. The worker loads the trusted runtime with a restricted DLL search path. Both modules remain resident until process exit.

The proxy and runtime remain loaded for the lifetime of the game process. See [troubleshooting](troubleshooting.md) for startup problems.

## Runtime and SDK

`runtime/` owns discovery, validation, Wasmtime stores, budgets, logging, and lifecycle. `sdk/` defines the guest contract independently from the Windows loader. `crml_host` embeds the same core as the DLL, so sandbox behavior can be tested without launching the game.

`runtime/diagnostics/` contains the engine and fall observers, entity inspector, and camera/physics observation helpers as ordinary C++ sources and headers. They remain compiled into the targets that use them; native test entry points stay in `tests/`. The relocation does not change their startup gates or remove observation code reused by gameplay services.

Keyboard actions use an independent read-only provider rather than requiring a movement service. Each Wasm store retains its own capability mask, action bindings and native ownership ID. Availability queries cannot grant permissions, and explicit cleanup applies only to the calling mod's leases. Startup validates manifests and prepares requested services before any guest executes. An invalid manifest cannot request engine services; a module that later fails compilation may have requested a service, but cannot acquire a lease.

Movement and physics share `controller_hook.cpp`: one native trampoline forwards the six controller arguments through the movement adapter, then lets physics observe the original view after return. Visibility retains its renderer-phase hook. `gameplay_router.h` routes requests and cleanup to independent providers. Read-only input observers reuse the movement suppression hook without replacing its owner. When movement and physics coexist, only movement owns the status panel; physics results remain available through `physics_status` and logs.

Read-only player and camera services start from `player.read` and `camera.read` manifest requests. The player service reuses the controller adapter to copy authenticated position before the original call; requesting it alone enables no movement override, keyboard suppression or overlay. Camera observation shares one camera-update trampoline with the diagnostic recorder and copies the selected view after the original call. Neither service depends on a diagnostic file remaining open.

The worker reads these caches through fixed-width structures from `sdk/include/crml_state.h`. It never follows engine pointers on behalf of a snapshot request, and no engine callback runs Wasm. Output pointers refer only to the calling store's linear memory. The host checks exact structure sizes and memory bounds before calling a provider, clears output on non-success, and counts reads against the same eight-call budget as other gameplay imports. Local generation counters identify observed player/camera changes without exposing engine handles.

Snapshots are at most 500 ms old and are sampled independently, not as one synchronized engine frame. Player reads require foreground focus; camera reads depend on fresh completed updates and are not focus-gated. Camera position, basis and optional lens values describe the selected `CameraView`; renderer overrides such as `CameraManView` can produce a different final view. Snapshot availability does not establish camera ownership or authorize writes. See the [snapshot API](api.md#player-camera-and-physics-snapshots) for layouts, flags and result codes.

The Wasm physics service accepts bounded commands on that worker and consumes them after the native physics dispatcher returns. A mod owns an opaque selection token, never an engine pointer. The service resolves the current entity/body generation again before every access and retains restoration state independently of the guest. Mod traps and shutdown request cleanup without calling guest code from an engine hook. Only one mod can own the operation at a time. See the [damping API](api.md#experimental-prop-damping) for limits and result semantics.

`physics_read` copies the selected body's damping and speed observations captured on the physics callback. The caller must own the selection token; reads do not extend its lifetime or change a property. Retirement and scene changes invalidate cached observations, and focus loss or Escape makes the read unavailable while cleanup proceeds. Flags distinguish available damping and speed groups. Angular damping and speed are read-only; the bounded write operation remains temporary linear damping on one body.

Packages load in directory-name order. One failed package does not stop the rest. Guest lifecycle execution is single-threaded, with no hot reload or guest-to-guest shared memory.

## Game bridge

The opt-in experimental bridge observes the character-controller routine on the thread used by the game. Wasm remains on the runtime worker and requests an owner-bound movement lease. The hook validates the current player, substitutes private per-call arguments, and forwards the original routine. It never runs Wasm inside the hook. Unapproved executables, changed hook signatures and mismatched arguments leave movement untouched.

The runtime worker publishes the panel's visibility and status. Native DirectX 12 hooks copy a cached text texture into the current back buffer before presentation. Queue selection requires observed transitions of that specific buffer; unrelated queue submissions cannot select it. Each buffer has its own commands and completion fence, and resize hooks release retained buffers after completion. The renderer follows the [DirectX 12 presentation state requirements](https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains).

Overlay startup waits for the version-gated engine present callback to supply a live swapchain on its render thread. It creates no temporary device, window or swapchain, and can attach after presentation has already begun. Command implementation discovery uses the existing device; its temporary queue never receives submissions. Streamline proxies are resolved through the [documented native-interface query](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md#53-how-to-check-if-sl-proxies-are-used). A changed engine entry or unsupported graphics interface leaves the panel unavailable. These hooks are native runtime infrastructure, not a guest rendering API.

Native code owns input, movement and UI resources; guests receive status codes and opaque handles, never raw addresses. The current movement API accepts a bounded world-space velocity, with controls and speed selection in the guest. The legacy native noclip helper accepts a speed parameter instead. The probe records up to 600 diagnostic snapshots and then stops logging. The pinned movement hook remains a pass-through when the feature is off.

See the [legacy native noclip prototype](gameplay.md) for that implementation, or the [Wasm movement example](movement.md) for guest controls and API ownership. Players should use the [runtime installation guide](installation.md) and their mod's own instructions.

The [engine research tools](engine-research.md) map Pack2 assets, candidate script bindings, and ECS system metadata without loading the runtime into the game.
