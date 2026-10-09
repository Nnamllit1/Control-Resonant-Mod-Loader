---
title: Extend the native bridge
description: Turn a concrete mod requirement into a bounded, compatible CRML capability with clear lifetimes and outcomes.
---

# Extend the native bridge

Wasm mods can call the imports shipped by their installed runtime. Finding an
engine function in the research maps does not make it callable from a guest.
If a mod needs an operation outside the [supported scope](sdk-contracts.md),
propose a runtime capability or contribute an adapter to the native runtime.
There is no guest API for loading a native provider DLL or arbitrary engine code.

## Describe the mod requirement

Start with the player's intended result and the information the guest needs to
decide when to request it. Include a small guest-level example, expected outcomes,
and what should happen on disable, reload, target replacement and interference
from another mod. An issue or contribution should distinguish a direct player
request from a hypothetical use case.

For example, temporary slippery props require a friction operation. The existing
linear-damping adapter cannot supply it: damping changes velocity decay, not
surface friction. A proposal should identify how to select the affected bodies,
which material/contact property changes, and how to restore it without overwriting
a later game change. Avoid extending `physics_apply` to mean several incompatible
operations through undocumented modes.

Keep activation keys, selection preferences, values, durations, notifications and
saved preferences in the mod. The native adapter should provide the smallest
reusable engine operation that meets the requirement, with explicit parameters.

## Define the contract before adding the import

| Contract area | Required decisions |
| --- | --- |
| Permission and availability | Which capability authorizes the call, how the adapter starts, and how unsupported builds or missing services are reported |
| Data | Fixed-width layout/version, units and coordinate frame, finite/range checks, copied strings and exact memory bounds |
| Identity | Owner-scoped opaque tokens, target lifetime, stale-token behavior and replacement detection; no engine pointers or inferred save identities |
| Scheduling | The native phase that owns the operation, whether it can be queued, and what a return value actually proves |
| Outcomes | Admission, contention, applied/observed evidence, cancellation, rejection and uncertain delivery; receipt lifetime and eviction |
| Composition | Resource-specific ownership, how conflicting requests resolve, and how unrelated owners remain independent |
| Cleanup | Expiry, explicit release, guest trap/load failure, shutdown, reload and target retirement; recovery when restoration cannot run immediately |
| Bounds | Per-call/callback limits, queue/receipt capacities, maximum lifetime and measured cost under composed guests |

Input is an example of why these distinctions matter. Window focus does not
establish gameplay ownership. The current diagnostic observers can identify a
completed player-action-view eligibility check and a positive native UI-context
match. A negative UI scan does not rule out text entry, other menus or capture.
Do not turn those observations into unrestricted suppression permission.

## Implement through the existing layers

1. Add fixed-width public types under `sdk/include` and an import declaration in
   `crml.h`. Introduce a capability only when it represents a distinct permission;
   an additional observation of an existing service can share that permission.
   Preserve ABI 1 imports and document the required `min_runtime` for new symbols.
2. Validate the guest call in `runtime/runtime.cpp` before native access. The
   runtime supplies the owner, checks exported memory and sizes, copies bounded
   inputs, charges the relevant budget and clears failed snapshot outputs.
3. Route the operation through a native service. `runtime/runtime.h` defines the
   existing trusted provider interface; `runtime/gameplay_router.h` routes game
   services. These are implementation interfaces, not a native add-on ABI. Keep
   the new service's state and lifecycle in its own component when appropriate.
4. Perform engine work only in the reviewed owning phase. Use build guards,
   identity checks and copied observations; do not call a recovered method from
   the Wasm worker merely because its address is known. Wire service availability
   and startup in `runtime/entry.cpp` where the installed feature requires them.
5. Provide deterministic guest behavior in `tools/simulation.cpp` and the scenario
   protocol in `tools/mod.py` where useful. Reuse pure contract/state helpers;
   label synthetic observations explicitly and keep the simulator independent
   of engine execution.

A command accepted by a queue is not completed engine work. Preserve uncertainty
where the adapter cannot observe completion. Do not retry an uncertain delivered
command automatically when replay could apply it twice. For reversible writes,
restoration must respect target identity and newer game changes rather than
blindly copying an old value back.

## Verify and package the capability

Use native tests for state transitions, lifetimes, contention and cleanup, and
real Wasmtime guests for permission, memory, budget and malformed-input boundaries.
Exercise at least two competing owners when the operation owns a resource.
Guest scenarios should cover ordinary success and a useful refusal/recovery path.
Examples belong in `examples`, with guest policy visible in their source.

Static maps and mocks do not prove native behavior. Arrange a short gameplay check
with explicit expected results for the engine operation and restoration/reload
paths that cannot be established offline. Mark unsupported or unverified behavior
in the service contract rather than describing it as generally available.

Update the API reference, scope matrix, relevant engine maps, examples and release
notes together. Keep extracted game code and local investigation artifacts out of
public documentation. Add new reference pages to `sdk/reference-files.txt`, extend
the maintained example inventory when adding a package, and test the actual SDK
ZIP using the [release workflow](releases.md). A source build passing tests does
not establish that the published archive includes the new headers or tools.

## Capabilities that need separate engine work

Equipment changes need unlocked-item validation and evidence of transaction and
rollback semantics. Combat events need authoritative attribution and ordered,
bounded delivery. Entity spawning/deletion needs ownership, references, resource
loading and scene/save cleanup. Camera control needs ownership and restoration
independent of player movement. None follows automatically from the existing
damping, visibility or flight adapters; each needs its own contract and evidence.
