"""Compile self-authored Luau source to the observed engine bytecode profile.

Offline only: no engine loading, execution, resource envelope or mod installation.
Use a trusted local Luau 0.650 compiler; structural checks do not sandbox code.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess

from binlua import MAX_BYTES, parse
from engine_research import read_bounded


def compile_source(compiler, source):
    """Return structurally checked raw bytecode; leave the source untouched."""
    compiler = Path(compiler).resolve(strict=True)
    source = Path(source).resolve(strict=True)
    original = read_bounded(source, MAX_BYTES)
    original.decode("utf-8")
    result = subprocess.run([str(compiler), "--binary", "-O0", "-g2", str(source)],
                            capture_output=True, timeout=60, check=False)
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()[:4096]
        raise ValueError(f"Luau compilation failed (exit {result.returncode}): {detail}")
    if read_bounded(source, MAX_BYTES) != original:
        raise ValueError("Source changed during compilation; compile again")
    # The same observed profile is used by the embedded, gameplay-tested examples.
    # This is format validation, not proof of safe or compatible engine bindings.
    parse(result.stdout, envelope=False)
    return result.stdout


def compile_file(compiler, source, output, *, check=False):
    compiler, source, output = (Path(p).resolve() for p in (compiler, source, output))
    for protected in (source, compiler):
        if output == protected or (output.exists() and protected.exists() and output.samefile(protected)):
            raise ValueError("Output must not overwrite the source or compiler")
    code = compile_source(compiler, source)
    if check:
        if read_bounded(output, MAX_BYTES) != code:
            raise ValueError("Existing bytecode differs from compiler output")
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(code)
    return code


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check", action="store_true", help="compare existing output without modifying it")
    args = parser.parse_args(argv)
    try:
        code = compile_file(args.compiler, args.source, args.output, check=args.check)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    verb = "Verified" if args.check else "Compiled"
    print(f"{verb} {len(code)} bytes; bytecode 6 / types 3; SHA256 {hashlib.sha256(code).hexdigest()}")


if __name__ == "__main__":
    main()
