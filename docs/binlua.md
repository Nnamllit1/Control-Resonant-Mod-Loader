# Lua bytecode tools

`tools/binlua.py` inspects local `.binlua` files and individual script assets in Pack2 archives without loading them into the game. It provides structural JSON, instruction listings and conservative source fragments for supported functions. Python 3.10 or later is required; the tool uses the standard library and the adjacent `engine_research.py` module.

## Inspect a script

For an existing local file:

```powershell
python tools/binlua.py .local/scripts/example.binlua
python tools/binlua.py .local/scripts/example.binlua --format json --output .local/scripts/example.json
python tools/binlua.py .local/scripts/example.binlua --format disasm --output .local/scripts/example.asm
```

To read one asset directly from an installed pack:

```powershell
$gameDir = Read-Host 'Path to your CONTROL Resonant installation'
python tools/binlua.py --game-dir "$gameDir" --toc data_pack2/pc/base-generic.rmdtoc --asset data/lua_scripts/heron/generic/out_of_bounds_area.binlua --format disasm --output .local/scripts/boundary.asm
```

The virtual asset path must match exactly one entry in the selected index. Raw and LZ4 blocks are read only for that asset. TOC and blob paths must resolve inside the game directory, including paths containing `..`. The tool does not modify archives. Output inside the supplied game directory and output overwriting the input file are rejected.

Output can contain game script logic, debug names and strings. Keep it in an ignored local directory and review it before sharing. CRML distributes the tools and self-authored test fixtures, not game script payloads.

## Supported profile and output

The parser supports the observed bytecode version **6** with type information version **3**. The resource envelope is one opaque byte; it is reported without assigning a meaning. `--raw-bytecode` accepts compiler output without that envelope. Other versions and unknown opcodes are explicitly rejected rather than interpreted using the wrong layout.

The format interpretation follows the [reviewed engine loading path](engine-paths.md#script-resources-and-the-vm) and the [Luau bytecode definitions](https://github.com/luau-lang/luau/blob/0.650/Common/include/Luau/Bytecode.h). This does not identify the engine's exact upstream revision or prove compiler compatibility.

| Format | Contents |
| --- | --- |
| `summary` | Input hash, format versions, counts and opcode frequencies; default output. |
| `json` | Strings, typed constants, prototypes, child references, raw instruction words, decoded instructions, type-info bytes and available debug metadata. |
| `disasm` | Per-prototype constants and instructions, with word offsets, signed operands, jump targets and available source lines. |
| `source` | A reconstructed analysis fragment for the selected prototype, or an error explaining unsupported constructs. |

String references in JSON use zero-based indices. Strings have both display text and exact hexadecimal bytes, so invalid UTF-8 and embedded NULs are preserved. Non-finite numbers use an explicit object representation instead of invalid JSON numeric literals. Debug definition lines and per-instruction lines are separate. Original absolute source paths are not invented from debug labels.

Checks include input and aggregate item limits, truncation, string/constant/prototype references, register bounds, AUX lengths, jump destinations and closure capture sequences. `NEWCLOSURE` and `DUPCLOSURE` can both have capture instructions. Branches into AUX words or capture sequences are rejected. These checks support offline analysis; **they are not a security verifier for loading arbitrary bytecode into the game**.

Inputs are limited to 16 MiB and one million aggregate counted items. TOC inspection uses the existing Pack2 bounds. This is a resource ceiling, not a fixed memory or execution-time guarantee for every report size.

## Compile self-authored source

`tools/compile_lua.py` compiles a UTF-8 `.luau` or `.lua` file with a trusted local **Luau 0.650** compiler, using the `-O0 -g2` settings exercised by the embedded engine examples. The compiler is not bundled or downloaded automatically. Python 3.10 or later is required. The same compilation function regenerates CRML's embedded examples.

```powershell
$compiler = Read-Host 'Path to Luau 0.650 luau-compile.exe'
python tools/compile_lua.py examples/lua-probe/arithmetic.luau --compiler "$compiler" --output .local/arithmetic.bytecode
python tools/compile_lua.py examples/lua-probe/arithmetic.luau --compiler "$compiler" --output .local/arithmetic.bytecode --check
python tools/binlua.py .local/arithmetic.bytecode --raw-bytecode --format disasm
```

The output is raw compiler bytecode, without the game's resource envelope. Compilation checks the observed bytecode/type profile and structure before replacing output, and refuses to overwrite the input or compiler. `--check` compares an existing file without writing it. A compiler error, timeout, changed source or unsupported output leaves existing bytecode untouched. Matching format headers do not identify the compiler version or establish that a particular engine binding is safe to call.

This is an offline authoring tool, not an installation command. General Lua source packages are not yet loaded from `crml/mods`; placing the output there does not run it. Direct engine Lua remains trusted scripting, separate from the Wasm sandbox. See [engine Lua integration](engine-lua.md) for supported behavior and calling-context limits. SDK builds include these Python tools and their supporting modules; players do not need them.

## Readable source fragments

Select the prototype index from the JSON or disassembly:

```powershell
python tools/binlua.py .local/scripts/example.binlua --format source --prototype 0 --output .local/scripts/example.lua
```

The output is register-oriented Luau. It preserves explicit intermediate assignments rather than guessing original variable names or moving calls into expressions. A factory accepts the original environment, mutable upvalue cells and resolved imports, then returns the reconstructed function. Import values are supplied separately because the engine can cache them; they are not silently replaced by repeated global lookups.

Supported constructs include scalar constants, register moves, global and upvalue access, table reads/writes, arithmetic, fixed-count calls and returns, and supported forward `if`/`else` branches. Fast-call optimizations are represented through their ordinary call fallback. Source comments, formatting and missing local names cannot be recovered from bytecode.

Loops, closure creation, dynamic multiple-result flow, engine vectors, specialized method calls and control flow that cannot be structured by this pass are explicitly unsupported. A function with unsupported behavior produces no partial source, and a failed reconstruction does not overwrite an existing output file. Use the disassembly for those functions. Source fragments are analysis aids, not complete replacement modules or ready-to-install mods.

## Testing the tools

`python tests/test_binlua.py` runs synthetic format, bounds, reconstruction and file-handling tests. The normal CTest build includes this suite and does not require game assets or a Luau installation.

For behavioral comparisons, supply separately built official Luau compiler and interpreter executables:

```powershell
$compiler = Read-Host 'Path to luau-compile executable'
$interpreter = Read-Host 'Path to luau executable'
python tests/test_binlua.py --compiler "$compiler" --vm "$interpreter"
```

The optional test compiles self-authored functions, reconstructs them, and compares results at optimization levels 0 and 1. It covers branches, calls, imports, upvalue changes, scalar arithmetic, tables and binary strings. Luau 0.650 produces the version-6/type-3 profile used by this test. These standalone comparisons do not establish compatibility with the game's VM or validate every reconstructed game function. No game script is executed by the tests.

## Engine Lua and Wasm

The engine Lua loading path and several bindings are mapped, but CRML does not yet accept `.lua` files as mod packages. The [engine Lua integration](engine-lua.md) provides a fixed, opt-in developer smoke test. Compatible compilation, correct script environments and lifetime handling must be established before general source mods become a supported path.

Direct engine scripting would use the engine's existing bindings and trust model. It does not inherit the Wasm host's capability checks or execution limits. Wasm remains the supported sandboxed guest format; see [sandbox limits](sandbox.md). The depth of either mod path depends on the engine operations it exposes, rather than the language alone.
