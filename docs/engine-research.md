---
title: Northlight engine research
description: Public CONTROL Resonant reverse-engineering tools and Northlight research for Wasm and native mod developers: assets, scripts, and ECS systems.
---

# Northlight engine research for CONTROL Resonant

The research tools map assets, scripting, and ECS systems independently of the experimental noclip bridge. They read installed files without starting the game. The resulting catalog preserves build fingerprints and evidence locations so contributors can reproduce and refine each finding.

[Northlight](https://www.remedygames.com/northlight) is Remedy Entertainment's in-house game engine. This community research focuses on the recorded CONTROL Resonant builds. It is intended for Wasm mod authors, native DLL mod developers, and tooling contributors; using the findings does not require adopting CRML. It does not establish compatibility with Control, Alan Wake 2, or other Northlight titles.

For the mapping from assets to native objects, owned dependencies, and execution stages, see [engine internals](engine-internals.md). That investigation traces individual code paths beyond the catalog.

## Generate and search a catalog

Python 3.10 or newer is sufficient; no additional packages are needed. Run from the repository root:

```powershell
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
python tools/engine_research.py "$gameDir" --output .local/engine/baseline.json
python tools/engine_query.py .local/engine/baseline.json asset .binlua
python tools/engine_query.py .local/engine/baseline.json binding nl_resource_stream
python tools/engine_query.py .local/engine/baseline.json system coregame::lua_script
python tools/engine_query.py .local/engine/baseline.json anchor PackFileManager
```

Searches use case-insensitive substrings. `--limit 50` changes the number displayed. Asset results retain their source pack; duplicate paths across language and streaming packs are separate records. A catalog does not establish which overlapping resource wins at runtime.

The scanner reads the executable, selected middleware import tables, and asset indexes. It checks blob existence and sizes without scanning their contents. Optional `--sample-header` arguments decode the first block of specific resources and record only 16 header bytes, a hash, and the decoded block size:

```powershell
python tools/engine_research.py "$gameDir" --output .local/engine/headers.json --sample-header data/lua_scripts/systems.binlua --sample-header data/uiresources/game/ui/ui.ui --sample-header data/uiresources/game/ui/ui.bundle.css
```

Choose a new report filename for each run. Reports cannot be written inside the game directory. Failed indexes and samples appear in `errors`, and the command exits nonzero while preserving successfully collected evidence. The tool rejects unsupported encodings and bounds decoded indexes to 128 MiB. Header sampling permits up to 16 requested paths and a first block of at most 16 MiB each. It does not extract asset files or execute scripts.

Keep generated catalogs under the ignored `.local/` directory. Commit the tools and format findings; keep game resource contents out of the repository.

## Recorded baseline

The initial full survey used executable SHA-256:

```text
2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0
```

| Evidence | Result | Limit |
| --- | --- | --- |
| Pack2 indexes | 25 decoded; 294,899 file records | Counts include records across languages and packs |
| Packed scripts | 806 `.binlua` records | Script format and execution semantics remain under investigation |
| Script registration pattern | 904 candidate name/callback pairs | Partial heuristic scan, not a verified callable API |
| ECS metadata | 2,474 system signatures | Names and argument types, not scheduler ordering |
| Pack consistency | All referenced blob sizes, file block totals, directory ownership, and metadata ranges validated | Most blob payloads have not been decoded or integrity-checked |

The tests use generated fixtures, never game files:

```powershell
python tests/test_engine_research.py
```

They cover compressed data bounds, invalid archive records, metadata types, path reconstruction, PE bounds, candidate callback filtering, and report behavior. The suite also runs through the normal CMake tests.

## Asset loading

The installation contains loose `data/lualibs/*.lua` libraries and `data/shaders/build/pc_dx12/*.binrfx` shaders. Packaged resources live under `data_pack2`, with `.rmdtoc` indexes referring to numbered `.rmdblob` files in both `pc` and `generic` directories.

The [Pack2 format notes](pack2.md) describe the decoded records and compression descriptors. Each catalog asset has its virtual path, logical size, typed metadata, and physical blob block locations. This gives a concrete route from an asset name to its stored bytes.

Executable evidence includes `PackFileManager`, `Pack2FileSystem`, `Loader_Pack2Index`, `Loader_Adaptor_Pack2`, and asynchronous resource-loading continuations. These names support a layered file system and resource loader. Mount precedence, cache invalidation, loose-file overrides, and reload behavior still require code tracing or runtime observation.

| String anchor | String RVA | Candidate code reference RVA |
| --- | --- | --- |
| `PackFileManager::tryCreate` | `0x50409e8` | `0x31da9b9` |
| `PackFileManager::m_pack2FS` | `0x5040b90` | `0x31daa89` |
| `Loader_Pack2Index::m_impl` | `0x505b5b0` | `0x3245aca` |
| `data/lualibs` | `0x3aa0978` | `0x3daffe` |
| `content::LuaScriptResource` | `0x3bec0a8` | `0x7123b0` |

These are locations in the fingerprinted executable, not public entry points. Add the loaded image base to an RVA only when inspecting that same build. The scanner finds RIP-relative `LEA` byte patterns; confirm instruction boundaries in a disassembler. PE exception-table ranges can describe function fragments.

## Scripting and lifecycle

The executable contains a Lua 5.1.4 identifier, `content::LuaScriptResource` loading metadata, and ECS systems that name script loading, initialization, updates, events, and streaming. The identifier does not establish the serialized format: subsequent [loader tracing](engine-paths.md#script-resources-and-the-vm) reaches a Luau-like bytecode reader. The observed symbols identify several lifecycle stages:

| System group | Named operations |
| --- | --- |
| `coregame::lua_systems` | `processPendingRegistrations`, `processThrottledGlueComponents`, `streamInResources`, `streamOutResources` |
| `coregame::lua_script` | `collectScriptsToLoadIntoVM`, `updatePendingResources`, `processNewScriptEntities`, `processThrottledLoading`, `processThrottledInits` |
| Script execution | `updateSystemUpdateOrder`, `luaFixedUpdate`, `collectGarbage` |
| Script events | `luaStreamInEvents`, `luaStreamOutEvents`, `coregame::lua_events::sortInitEvents` |
| Trigger and UI integration | `coregame::trigger_scripting::applyQueuedEvents`, `coregame::ui_events::system::handleLuaCallbacks` |

This suggests a staged lifecycle with deferred loading and initialization. The actual scheduling order, owning threads, state ownership, and error handling are not yet verified. Search each name with `engine_query.py ... system` to inspect its full component/environment signature and candidate code references.

Candidate bindings include `nl_add_event_handler`, `nl_send_custom_event`, `nl_resource_stream_in`, `nl_resource_stream_out`, and `nl_is_resource_ready`. The scanner currently recognizes one compiler-generated sequence storing a string address followed by a function address. Other registration styles will be missed; names alone do not establish argument types or calling conventions.

`data/lua_scripts/systems.binlua` is a 1,630-byte resource in the baseline, with `content::LuaScriptMetadata`. Its decoded first bytes are `00 06 03 2b`, whereas [Lua 5.1 defines its chunk signature](https://www.lua.org/source/5.1/lua.h.html) as `1b 4c 75 61`. The traced reader separates an envelope byte; the remainder is consistent with the Luau-like loader's bytecode version 6, type version 3, and initial string count. See [the VM path and its limits](engine-paths.md#script-resources-and-the-vm). Exact compiler compatibility and the complete engine resource format remain unverified.

## UI and gameplay are connected through engine APIs

The executable imports Cohtml. Its `cohtml.WindowsDesktop.dll` imports `v8.dll`, `v8_libplatform.dll`, and `v8_libbase.dll`. This establishes a dependency chain for the UI middleware; it does not establish V8 as the gameplay script VM.

The catalog resolves `data/uiresources/game/ui/ui.bundle.css`, compiled `ui.ui`, fonts, and locale CSS. Sampling confirmed CSS text in the first resource and a binary header beginning `3f b3 4d d3 02 00 00 00` in `ui.ui`. No assumption is made that the compiled page can be replaced by a plain HTML file.

The existing native bridge also demonstrates entity handles, generation checks, archetype rows, component lookup, and shared environment data; see `runtime/movement_view.cpp`. ECS signatures expose component access definitions and environment inputs. Component identities, resource identities, entity handles, and persistent GlobalIDs must remain distinct when tracing their relationships.

## Related references

The [engine atlas](engine-atlas.md) indexes recovered system declarations and candidate bindings. [Engine operation paths](engine-paths.md) connect script, spawn/remove, physics, material, UI, audio, animation, camera, AI, and save entries to their implementations. Each reference describes the available evidence and its limits.

Engine Lua callbacks are separate from the Wasm host API. Only the operations documented in the [SDK reference](api.md) are exposed to guest modules.
