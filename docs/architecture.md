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

This deferred route still requires real-game validation, including startup, keyboard-only play, controller input, and shutdown. An imported function does not prove that the game reaches it at a suitable time. In particular, the host application's use of the API during its own loader callbacks must be investigated if startup hangs.

## Runtime and SDK

`runtime/` owns discovery, validation, Wasmtime stores, budgets, logging, and lifecycle. `sdk/` defines the guest contract independently from the Windows loader. `crml_host` embeds the same core as the DLL, so sandbox behavior can be tested without launching the game.

Packages load in directory-name order. One failed package does not stop the rest. The current runtime is single-threaded, has no hot reload, and has no guest-to-guest shared memory.

## Game bridge

The next layer will translate narrow, versioned operations into verified game-thread actions. Player transforms, collision state, and UI resources remain owned by the native bridge. Mods receive opaque handles and validated values, never raw addresses. Unknown game builds must disable unsupported gameplay features.

See [gameplay and noclip](gameplay.md) for the first integration milestone.
