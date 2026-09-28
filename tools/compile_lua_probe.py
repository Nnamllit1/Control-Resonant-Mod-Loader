"""Regenerate the embedded, self-authored Lua probe with a local Luau 0.650 compiler."""
import argparse
import hashlib
from pathlib import Path
import subprocess
from binlua import parse


def generate(compiler, root):
    lines = ["// Generated from examples/lua-probe with Luau 0.650, -O0 -g2.",
             "// Contains only CRML-authored code. Regenerate with tools/compile_lua_probe.py.",
             "#pragma once", "namespace crml::probe::lua::bytecode {"]
    for name in ("arithmetic", "error", "bindings", "events", "persistent", "persistent_error"):
        source = root / "examples" / "lua-probe" / (name + ".luau")
        code = subprocess.run([str(compiler), "--binary", "-O0", "-g2", str(source)],
                              check=True, capture_output=True).stdout
        parse(code, envelope=False)
        lines.append("// Source SHA256 (LF): " + hashlib.sha256(source.read_text(encoding="utf-8").encode()).hexdigest())
        lines.append(f"inline constexpr unsigned char {name}[]{{")
        for start in range(0, len(code), 16):
            lines.append("    " + ",".join(f"0x{b:02x}" for b in code[start:start+16]) + ",")
        lines.append("};")
    lines.append("}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    text = generate(args.compiler.resolve(), root)
    output = root / "runtime" / "lua_probe_bytecode.h"
    if args.check:
        if output.read_text(encoding="utf-8") != text:
            parser.error("Embedded Lua probe differs from compiler output")
    else:
        output.write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
