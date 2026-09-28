"""Package allowlisted release files, or verify a completed release directory."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.compile(r"v\d+\.\d+\.\d+(?:-[a-z0-9]+(?:\.[a-z0-9]+)*)?", re.ASCII)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked_version(value):
    if not VERSION.fullmatch(value):
        raise ValueError('Expected a version such as v0.1.0-alpha.1')
    return value


def archive_names(version):
    return [f'crml-{kind}-{version}-windows-x64.zip' for kind in ('runtime', 'noclip', 'noclip-bundle', 'sdk')]


def verify(output, version):
    checked_version(version)
    manifest = json.loads((output / 'release.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or manifest.get('version') != version:
        raise ValueError('Release metadata does not match the requested version')
    if set(manifest['archives']) != set(archive_names(version)):
        raise ValueError('Unexpected release archive set')
    expected_checksums = []
    for name, record in manifest['archives'].items():
        data = (output / name).read_bytes()
        if digest(data) != record['sha256'] or len(data) != record['size']:
            raise ValueError(f'Archive hash/size mismatch: {name}')
        with zipfile.ZipFile(output / name) as archive:
            names = archive.namelist()
            if len(names) != len(set(names)) or set(names) != set(record['files']):
                raise ValueError(f'Archive contents differ: {name}')
            for entry in names:
                if entry.startswith('/') or '\\' in entry or ':' in entry or '..' in entry.split('/'):
                    raise ValueError('Unsafe archive member')
                if digest(archive.read(entry)) != record['files'][entry]:
                    raise ValueError(f'Archive member hash mismatch: {name}')
        expected_checksums.append(f"{record['sha256']}  {name}\n")
    if (output / 'SHA256SUMS.txt').read_text() != ''.join(expected_checksums):
        raise ValueError('Checksum list does not match the manifest')
    print(f'Verified {len(manifest["archives"])} release archives and every member hash.')
    return manifest


def package(dist, output, version, root=ROOT):
    checked_version(version)
    if output.exists():
        raise ValueError('Output already exists; choose a new directory to preserve the previous release')
    features = json.loads((dist / 'crml/build-features.json').read_text(encoding='utf-8-sig'))
    if features.get('movement_wasm') is not True or features.get('lua_probe') is not False:
        raise ValueError('Build with -ExperimentalGameplay, without -LuaProbe, before packaging')
    notes = root / 'release' / f'{version}.md'
    if not notes.is_file():
        raise ValueError('Add release notes for this version before packaging')
    profile = json.loads((root / 'compatibility.json').read_text())['profiles'][0]
    # Match the compiled compatibility guard to the metadata shipped beside it.
    if profile['sha256'].encode('ascii') not in (dist / 'crml/crml_runtime.dll').read_bytes():
        raise ValueError('Runtime fingerprint differs from compatibility.json')
    runtime = {
        'xinput1_4.dll': dist / 'xinput1_4.dll',
        'crml/crml_runtime.dll': dist / 'crml/crml_runtime.dll',
        'crml/wasmtime.dll': dist / 'crml/wasmtime.dll',
        'crml/mods/hello/mod.ini': dist / 'crml/mods/hello/mod.ini',
        'crml/mods/hello/hello.wasm': dist / 'crml/mods/hello/hello.wasm',
        'crml/licenses/wasmtime.txt': dist / 'licenses/wasmtime/LICENSE',
        'crml/licenses/minhook.txt': dist / 'licenses/minhook/LICENSE.txt',
        'compatibility.json': root / 'compatibility.json',
        'THIRD_PARTY.md': root / 'THIRD_PARTY.md',
        'README-CRML.txt': root / 'release/README-runtime.txt',
    }
    noclip = {
        'crml/movement-wasm.enabled': b'',
        'crml/mods/movement/mod.ini': dist / 'examples/movement/mod.ini',
        'crml/mods/movement/movement.wasm': dist / 'examples/movement/movement.wasm',
        'README-NOCLIP.txt': root / 'release/README-noclip.txt',
        'compatibility.json': root / 'compatibility.json',
    }
    sdk = {
        'tools/crml_host.exe': dist / 'crml/crml_host.exe',
        'tools/crml_wat.exe': dist / 'crml/crml_wat.exe',
        'tools/wasmtime.dll': dist / 'crml/wasmtime.dll',
        'licenses/wasmtime.txt': dist / 'licenses/wasmtime/LICENSE',
        'THIRD_PARTY.md': root / 'THIRD_PARTY.md',
        'README-SDK.txt': b'CRML SDK\n\nHeaders are in sdk/include. Example .wat files are the build inputs; .c files\nare readable alternatives. Compile with tools/crml_wat.exe input.wat output.wasm.\nRun standalone mods with tools/crml_host.exe <mods-directory> [ticks]. Gameplay APIs\nrequire the in-game runtime and the matching mode; the standalone host cannot\ncontrol the game. Do not copy this SDK archive into the game directory.\n\nMod development and API: https://crml.nnamllit.de/developing/\n',
    }
    for path in (root / 'sdk').rglob('*.h'):
        sdk[path.relative_to(root).as_posix()] = path
    for example in ('hello', 'movement', 'visibility', 'physics-damping'):
        for path in (root / 'examples' / example).iterdir():
            if path.suffix in ('.c', '.wat', '.ini', '.md'):
                sdk[path.relative_to(root).as_posix()] = path
    metadata = (json.dumps({'version': version, 'game_sha256': profile['sha256'], 'lua_probe': False}, indent=2) + '\n').encode()
    runtime['crml/release.json'] = metadata
    noclip['crml/mods/movement/release.json'] = metadata
    sdk['release.json'] = metadata
    mappings = (runtime, noclip, {**runtime, **noclip}, sdk)
    try:
        commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True, stderr=subprocess.DEVNULL).strip()
        dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root, text=True, stderr=subprocess.DEVNULL).strip())
    except (OSError, subprocess.CalledProcessError):
        commit, dirty = None, True
    manifest = {'schema': 1, 'version': version, 'source_commit': commit, 'source_dirty': dirty,
                'game_sha256': profile['sha256'], 'lua_probe': False, 'archives': {}}
    # A local build must not publish personal checkout or home paths, including PE
    # CodeView records. Third-party notices are preserved as supplied upstream.
    private = []
    for path in (root.resolve(), Path.home()):
        for form in (str(path), path.as_posix()):
            private.extend((form.encode().lower(), form.encode('utf-16le').lower()))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='crml-package-', dir=output.parent) as tmp:
        stage = Path(tmp)
        for name, mapping in zip(archive_names(version), mappings):
            member_hashes = {}
            with zipfile.ZipFile(stage / name, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
                for member, source in sorted(mapping.items()):
                    data = source if isinstance(source, bytes) else source.read_bytes()
                    if any(value in data.lower() for value in private):
                        raise ValueError(f'Private build path found in {member}; rebuild with portable debug records')
                    info = zipfile.ZipInfo(member, (2026, 1, 1, 0, 0, 0))
                    info.compress_type = zipfile.ZIP_DEFLATED
                    info.external_attr = 0o100644 << 16
                    archive.writestr(info, data)
                    member_hashes[member] = digest(data)
            data = (stage / name).read_bytes()
            manifest['archives'][name] = {'sha256': digest(data), 'size': len(data), 'files': member_hashes}
        (stage / 'release.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        (stage / 'SHA256SUMS.txt').write_text(''.join(f"{record['sha256']}  {name}\n" for name, record in manifest['archives'].items()), encoding='utf-8')
        (stage / 'NEXUS-DESCRIPTION.txt').write_bytes((root / 'release/NEXUS-DESCRIPTION.txt').read_bytes())
        (stage / 'RELEASE-NOTES.md').write_bytes(notes.read_bytes())
        verify(stage, version)
        # Publish only a fully verified set. Copy into a newly created directory:
        # Windows can deny renaming temporary directories with inherited ACLs.
        output.mkdir()
        created = []
        try:
            for source in stage.iterdir():
                target = output / source.name
                with target.open('xb') as dest:
                    created.append(target)
                    with source.open('rb') as stream:
                        shutil.copyfileobj(stream, dest)
            verify(output, version)
        except Exception:
            for target in created:
                target.unlink(missing_ok=True)
            output.rmdir()
            raise
    print(f'Release prepared: {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--dist', type=Path, default=ROOT / 'dist')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    try:
        checked_version(args.version)
        output = args.output or ROOT / '.local/releases' / args.version
        (verify(output, args.version) if args.verify else package(args.dist, output, args.version))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
