# Engine research

The research tools map assets, scripting, and ECS systems independently of the experimental noclip bridge. They read installed files without starting the game. The resulting catalog preserves build fingerprints and evidence locations so contributors can reproduce and refine each finding.

For the mapping from assets to native objects, owned dependencies, and execution stages, see [engine internals](engine-internals.md). That investigation traces individual code paths beyond the catalog.

## Generate and search a catalog

Python 3.10 or newer is sufficient; no additional packages are needed. Run from the repository root:

```powershell
python tools/engine_research.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant" --output .local/engine/baseline.json
python tools/engine_query.py .local/engine/baseline.json asset .binlua
python tools/engine_query.py .local/engine/baseline.json binding nl_resource_stream
python tools/engine_query.py .local/engine/baseline.json system coregame::lua_script
python tools/engine_query.py .local/engine/baseline.json anchor PackFileManager
```

Searches use case-insensitive substrings. `--limit 50` changes the number displayed. Asset results retain their source pack; duplicate paths across language and streaming packs are separate records. A catalog does not establish which overlapping resource wins at runtime.

The scanner reads the executable, selected middleware import tables, and asset indexes. It checks blob existence and sizes without scanning their contents. Optional `--sample-header` arguments decode the first block of specific resources and record only 16 header bytes, a hash, and the decoded block size:

```powershell
python tools/engine_research.py "F:\SteamLibrary\steamapps\common\CONTROL Resonant" --output .local/engine/headers.json --sample-header data/lua_scripts/systems.binlua --sample-header data/uiresources/game/ui/ui.ui --sample-header data/uiresources/game/ui/ui.bundle.css
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

The executable contains a Lua 5.1.4 identifier, `content::LuaScriptResource` loading metadata, and ECS systems that name script loading, initialization, updates, events, and streaming. The observed symbols give several specific lifecycle investigation targets:

| System group | Named operations |
| --- | --- |
| `coregame::lua_systems` | `processPendingRegistrations`, `processThrottledGlueComponents`, `streamInResources`, `streamOutResources` |
| `coregame::lua_script` | `collectScriptsToLoadIntoVM`, `updatePendingResources`, `processNewScriptEntities`, `processThrottledLoading`, `processThrottledInits` |
| Script execution | `updateSystemUpdateOrder`, `luaFixedUpdate`, `collectGarbage` |
| Script events | `luaStreamInEvents`, `luaStreamOutEvents`, `coregame::lua_events::sortInitEvents` |
| Trigger and UI integration | `coregame::trigger_scripting::applyQueuedEvents`, `coregame::ui_events::system::handleLuaCallbacks` |

This suggests a staged lifecycle with deferred loading and initialization. The actual scheduling order, owning threads, state ownership, and error handling are not yet verified. Search each name with `engine_query.py ... system` to inspect its full component/environment signature and candidate code references.

Candidate bindings include `nl_add_event_handler`, `nl_send_custom_event`, `nl_resource_stream_in`, `nl_resource_stream_out`, and `nl_is_resource_ready`. The scanner currently recognizes one compiler-generated sequence storing a string address followed by a function address. Other registration styles will be missed; names alone do not establish argument types or calling conventions.

`data/lua_scripts/systems.binlua` is a 1,630-byte resource in the baseline, with `content::LuaScriptMetadata`. Its decoded first bytes are `00 06 03 2b`, whereas [Lua 5.1 defines its chunk signature](https://www.lua.org/source/5.1/lua.h.html) as `1b 4c 75 61`. Treat `.binlua` as an engine-specific serialized resource until its reader is traced; a stock Lua bytecode loader has not been shown to accept it.

## UI and gameplay are connected through engine APIs

The executable imports Cohtml. Its `cohtml.WindowsDesktop.dll` imports `v8.dll`, `v8_libplatform.dll`, and `v8_libbase.dll`. This establishes a dependency chain for the UI middleware; it does not establish V8 as the gameplay script VM.

The catalog resolves `data/uiresources/game/ui/ui.bundle.css`, compiled `ui.ui`, fonts, and locale CSS. Sampling confirmed CSS text in the first resource and a binary header beginning `3f b3 4d d3 02 00 00 00` in `ui.ui`. No assumption is made that the compiled page can be replaced by a plain HTML file.

The existing native bridge also demonstrates entity handles, generation checks, archetype rows, component lookup, and shared environment data; see `runtime/movement_view.cpp`. ECS signatures expose component access definitions and environment inputs. Component identities, resource identities, entity handles, and persistent GlobalIDs must remain distinct when tracing their relationships.

## Next investigation steps

1. Trace `LuaScriptResource::requestLoad` and `blockingLoad` into the `.binlua` deserializer. Identify its field encoding and how it creates a Lua function or script instance.
2. Follow `processPendingRegistrations`, script initialization, and `luaFixedUpdate` to establish ownership and lifecycle order. Match at least one named asset to its live instance.
3. Trace resource mount selection and `nl_resource_stream_in` to determine identity lookup, cache behavior, and whether a supported override mechanism exists.
4. Trace the compiled UI page reader and Lua UI callbacks to identify a bounded UI extension point.

The Wasm sandbox remains the mod boundary. Discovering engine Lua callbacks does not make arbitrary engine-script execution safe for guests. New mod capabilities should expose reviewed operations with validated arguments, lifetimes, and thread ownership through the trusted native bridge.
