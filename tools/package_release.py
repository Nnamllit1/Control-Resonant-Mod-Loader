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
# Exact DLL from the archive pinned by fetch-deps.py. Upstream Rust diagnostics
# contain public build-runner paths; retain the unmodified, verified dependency.
WASMTIME_SHA256 = 'fed9c7984a8fc0d4bf3a11356b4ce6bc9f7b2317a13c233b0eaf08d8a9fca9df'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked_version(value):
    if not VERSION.fullmatch(value):
        raise ValueError('Expected a version such as v0.1.0-alpha.1')
    return value


def archive_names(version, include_noclip=False):
    kinds = ('runtime', 'noclip', 'noclip-bundle', 'sdk') if include_noclip else ('runtime', 'sdk')
    return [f'crml-{kind}-{version}-windows-x64.zip' for kind in kinds]


def profile_fingerprints(document):
    profiles = document.get('profiles')
    if not isinstance(profiles, list) or not profiles:
        raise ValueError('Compatibility profiles must be a nonempty list')
    values = [profile.get('sha256') if isinstance(profile, dict) else None for profile in profiles]
    if any(not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{64}', value) for value in values):
        raise ValueError('Invalid compatibility profile fingerprint')
    if len(values) != len(set(values)):
        raise ValueError('Duplicate compatibility profile fingerprint')
    return values


def verify(output, version):
    checked_version(version)
    manifest = json.loads((output / 'release.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or manifest.get('version') != version:
        raise ValueError('Release metadata does not match the requested version')
    # Retain verification of older releases and explicitly requested local mods.
    if set(manifest['archives']) not in (set(archive_names(version)), set(archive_names(version, True))):
        raise ValueError('Unexpected release archive set')
    fingerprints = manifest.get('game_sha256s')
    if 'game_sha256s' in manifest:
        if not isinstance(fingerprints, list):
            raise ValueError('Invalid release profile fingerprints')
        profile_fingerprints({'profiles': [{'sha256': value} for value in fingerprints]})
        if manifest.get('game_sha256') != fingerprints[0]:
            raise ValueError('Legacy release fingerprint differs from first supported profile')
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
            metadata_members = {}
            for metadata in ('release.json', 'crml/release.json', 'crml/mods/movement/release.json'):
                if metadata in names:
                    member = json.loads(archive.read(metadata))
                    if not isinstance(member, dict):
                        raise ValueError(f'Invalid archive release metadata: {name}')
                    metadata_members[metadata] = member
                    # A missing outer field must not downgrade a modern archive
                    # to the legacy verifier. Genuine legacy archives omit the
                    # complete profile set from both metadata locations.
                    if fingerprints is None and 'game_sha256s' in member:
                        raise ValueError(f'Modern archive requires complete release profile fingerprints: {name}')
            # Older schema-1 archives omit the complete set; retain their hash
            # verification. New archives must agree with every advertised profile.
            if fingerprints is not None:
                if name.startswith('crml-sdk-'):
                    required_metadata = ('release.json',)
                elif name.startswith('crml-noclip-bundle-'):
                    required_metadata = ('crml/release.json', 'crml/mods/movement/release.json')
                elif name.startswith('crml-noclip-'):
                    required_metadata = ('crml/mods/movement/release.json',)
                else:
                    required_metadata = ('crml/release.json',)
                if any(member not in metadata_members for member in required_metadata):
                    raise ValueError(f'Modern archive is missing release profile metadata: {name}')
                if 'compatibility.json' in names and profile_fingerprints(
                        json.loads(archive.read('compatibility.json'))) != fingerprints:
                    raise ValueError(f'Archive compatibility profiles differ: {name}')
                for member in metadata_members.values():
                    if member.get('game_sha256s') != fingerprints or member.get('game_sha256') != fingerprints[0]:
                        raise ValueError(f'Archive release fingerprints differ: {name}')
                if 'crml/crml_runtime.dll' in names:
                    binary = archive.read('crml/crml_runtime.dll')
                    if any(value.encode('ascii') not in binary for value in fingerprints):
                        raise ValueError(f'Runtime fingerprint missing from advertised profiles: {name}')
        expected_checksums.append(f"{record['sha256']}  {name}\n")
    if (output / 'SHA256SUMS.txt').read_text() != ''.join(expected_checksums):
        raise ValueError('Checksum list does not match the manifest')
    print(f'Verified {len(manifest["archives"])} release archives and every member hash.')
    return manifest


def package(dist, output, version, root=ROOT, include_noclip=False):
    checked_version(version)
    if output.exists():
        raise ValueError('Output already exists; choose a new directory to preserve the previous release')
    features = json.loads((dist / 'crml/build-features.json').read_text(encoding='utf-8-sig'))
    if features.get('movement_wasm') is not True or features.get('lua_probe') is not False or features.get('lua_source', False) is not False:
        raise ValueError('Build with -ExperimentalGameplay, without -LuaProbe or -LuaSource, before packaging')
    notes = root / 'release' / f'{version}.md'
    if not notes.is_file():
        raise ValueError('Add release notes for this version before packaging')
    fingerprints = profile_fingerprints(json.loads((root / 'compatibility.json').read_text()))
    # Match the compiled compatibility guard to the metadata shipped beside it.
    binary = (dist / 'crml/crml_runtime.dll').read_bytes()
    if any(value.encode('ascii') not in binary for value in fingerprints):
        raise ValueError('Runtime fingerprint differs from compatibility.json')
    runtime = {
        'xinput1_4.dll': dist / 'xinput1_4.dll',
        'crml/crml_runtime.dll': dist / 'crml/crml_runtime.dll',
        'crml/wasmtime.dll': dist / 'crml/wasmtime.dll',
        'crml/mods/README.txt': b'Install compatible CRML mod folders here.\nEach mod includes its own installation instructions and requirements.\nhttps://crml.nnamllit.de/installation/\n',
        'crml/licenses/wasmtime.txt': dist / 'licenses/wasmtime/LICENSE',
        'crml/licenses/minhook.txt': dist / 'licenses/minhook/LICENSE.txt',
        'compatibility.json': root / 'compatibility.json',
        'THIRD_PARTY.md': root / 'THIRD_PARTY.md',
        'README-CRML.txt': root / 'release/README-runtime.txt',
    }
    if features.get('ui_bridge') is True:
        bootstrap=dist / 'crml/ui-bootstrap.html'
        if not bootstrap.is_file():
            raise ValueError('UI-enabled runtime requires ui-bootstrap.html; rebuild before packaging')
        runtime['crml/ui-bootstrap.html']=bootstrap
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
        'README-SDK.txt': b'CRML SDK\n\nHeaders are in sdk/include. Example .wat files are the build inputs; .c files\nare readable alternatives. Compile with tools/crml_wat.exe input.wat output.wasm.\nRun standalone mods with tools/crml_host.exe <mods-directory> [ticks]. Gameplay APIs\nrequire the in-game runtime and native service support; the standalone host cannot\ncontrol the game. Do not copy this SDK archive into the game directory.\n\nMod development and API: https://crml.nnamllit.de/developing/\n',
    }
    for path in (root / 'sdk').rglob('*.h'):
        sdk[path.relative_to(root).as_posix()] = path
    for name in ('compile_lua.py', 'binlua.py', 'binlua_source.py', 'engine_research.py'):
        sdk['tools/' + name] = root / 'tools' / name
    sdk['README-LUA.txt'] = (
        b'Offline Luau tools\n\nRequires Python 3.10+ and a trusted local Luau 0.650 compiler.\n'
        b'python tools/compile_lua.py example.luau --compiler <luau-compile.exe> --output example.bytecode\n'
        b'python tools/binlua.py example.bytecode --raw-bytecode --format disasm\n\n'
        b'These tools do not install or run Lua mods. General Lua source packages are not\n'
        b'yet supported by the game runtime. Direct engine Lua is trusted scripting,\n'
        b'not the Wasm sandbox. The compiler and extracted game scripts are not bundled.\n\n'
        b'https://crml.nnamllit.de/engine-lua/\n'
    )
    for example in ('hello', 'movement', 'visibility', 'physics-damping', 'input-actions', 'state-watch', 'startup-skip'):
        for path in (root / 'examples' / example).iterdir():
            if path.suffix in ('.c', '.wat', '.ini', '.md'):
                sdk[path.relative_to(root).as_posix()] = path
    metadata = (json.dumps({'version': version, 'game_sha256': fingerprints[0], 'game_sha256s': fingerprints, 'lua_probe': False}, indent=2) + '\n').encode()
    runtime['crml/release.json'] = metadata
    noclip['crml/mods/movement/release.json'] = metadata
    sdk['release.json'] = metadata
    mappings = (runtime, noclip, {**runtime, **noclip}, sdk) if include_noclip else (runtime, sdk)
    try:
        commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True, stderr=subprocess.DEVNULL).strip()
        dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root, text=True, stderr=subprocess.DEVNULL).strip())
    except (OSError, subprocess.CalledProcessError):
        commit, dirty = None, True
    manifest = {'schema': 1, 'version': version, 'source_commit': commit, 'source_dirty': dirty,
                'game_sha256': fingerprints[0], 'game_sha256s': fingerprints, 'lua_probe': False, 'archives': {}}
    # A local build must not publish personal checkout or home paths, including PE
    # CodeView records. Third-party notices are preserved as supplied upstream.
    private = []
    # Windows temp paths can use an 8.3 alias (for example a shortened account
    # directory). Check both supplied and resolved spellings, in both encodings.
    for path in (root.absolute(), root.resolve(), Path.home(), Path.home().resolve()):
        for form in (str(path), path.as_posix()):
            private.extend((form.encode().lower(), form.encode('utf-16le').lower()))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='crml-package-', dir=output.parent) as tmp:
        stage = Path(tmp)
        for name, mapping in zip(archive_names(version, include_noclip), mappings):
            member_hashes = {}
            with zipfile.ZipFile(stage / name, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
                for member, source in sorted(mapping.items()):
                    data = source if isinstance(source, bytes) else source.read_bytes()
                    upstream = member in ('crml/wasmtime.dll', 'tools/wasmtime.dll')
                    if upstream and digest(data) != WASMTIME_SHA256:
                        raise ValueError('Wasmtime DLL differs from the pinned upstream binary')
                    if not upstream and any(value in data.lower() for value in private):
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
    parser.add_argument('--include-noclip', action='store_true',
                        help='Also prepare local mod-only and bundle archives; not part of the GitHub release')
    args = parser.parse_args()
    try:
        checked_version(args.version)
        output = args.output or ROOT / '.local/releases' / args.version
        (verify(output, args.version) if args.verify else package(args.dist, output, args.version, include_noclip=args.include_noclip))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
