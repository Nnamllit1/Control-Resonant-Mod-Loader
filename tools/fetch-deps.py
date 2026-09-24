"""Fetch a pinned official Wasmtime archive and verify it before extraction."""
import hashlib
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
NAME = 'wasmtime-v49.0.0-x86_64-windows-c-api'
SHA256 = '279b5c06ed44994860ac8e4135cd73c550e4cb4e4a0c2905f6400b75d42dd460'
URL = f'https://github.com/bytecodealliance/wasmtime/releases/download/v49.0.0/{NAME}.zip'

def main():
    deps = ROOT / 'build' / 'deps'
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps / 'wasmtime.zip'
    if not archive.exists():
        print(f'Downloading {NAME}', flush=True)
        with urllib.request.urlopen(URL, timeout=120) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise SystemExit('Wasmtime download checksum mismatch')
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit('Wasmtime archive checksum mismatch; remove build/deps/wasmtime.zip and retry')
    with zipfile.ZipFile(archive) as source:
        for entry in source.infolist():
            target = (deps / entry.filename).resolve()
            if not target.is_relative_to(deps.resolve()):
                raise SystemExit('Unsafe archive path')
        source.extractall(deps)
    print('Wasmtime 49.0.0 verified')

if __name__ == '__main__':
    main()
