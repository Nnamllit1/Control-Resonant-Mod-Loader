"""Fetch pinned official dependencies and verify archives before extraction."""
import argparse
import hashlib
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
NAME = 'wasmtime-v49.0.0-x86_64-windows-c-api'
SHA256 = '279b5c06ed44994860ac8e4135cd73c550e4cb4e4a0c2905f6400b75d42dd460'
URL = f'https://github.com/bytecodealliance/wasmtime/releases/download/v49.0.0/{NAME}.zip'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--movement-probe', action='store_true')
    args = parser.parse_args()
    fetch(NAME, URL, SHA256, 'wasmtime.zip')
    if args.movement_probe:
        fetch('MinHook 1.3.4', 'https://codeload.github.com/TsudaKageyu/minhook/zip/refs/tags/v1.3.4',
              '172708123daa0c98d20d3a980b16a50be14af243dc95dee6f79c24193ad010e4', 'minhook.zip')

def fetch(name, url, sha256, filename):
    deps = ROOT / 'build' / 'deps'
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps / filename
    if not archive.exists():
        print(f'Downloading {name}', flush=True)
        with urllib.request.urlopen(url, timeout=120) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != sha256:
            raise SystemExit(f'{name} download checksum mismatch')
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != sha256:
        raise SystemExit(f'{name} archive checksum mismatch: {archive}')
    with zipfile.ZipFile(archive) as source:
        for entry in source.infolist():
            target = (deps / entry.filename).resolve()
            if not target.is_relative_to(deps.resolve()):
                raise SystemExit('Unsafe archive path')
        for entry in source.infolist():
            target = deps / entry.filename
            if entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            data = source.read(entry)
            # Preserve timestamps on verified, unchanged headers for incremental builds.
            if not target.is_file() or target.read_bytes() != data:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
    print(f'{name} verified')

if __name__ == '__main__':
    main()
