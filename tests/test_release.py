"""Release boundaries: exact archive contents, disabled probes, integrity, no overwrite."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_release', ROOT / 'tools/package_release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.dist = self.root / 'dist'
        self.out = self.root / 'release-output'
        self.version = 'v0.1.0-alpha.1'
        for name in ('compile_lua.py', 'binlua.py', 'binlua_source.py', 'engine_research.py'):
            self.write(self.root / 'tools' / name, (ROOT / 'tools' / name).read_bytes())
        for name in ('README-runtime.txt', 'README-noclip.txt', self.version + '.md'):
            self.write(self.root / 'release' / name, b'Public instructions')
        self.write(self.root / 'compatibility.json', json.dumps({'profiles': [{'sha256': 'a' * 64}]}).encode())
        self.write(self.root / 'THIRD_PARTY.md', b'Licenses')
        self.write(self.root / 'sdk/include/crml.h', b'SDK')
        self.write(self.root / 'sdk/include/crml_abi.h', b'ABI constants')
        self.write(self.root / 'sdk/include/crml_state.h', b'State ABI layouts')
        self.write(self.root / 'sdk/include/crml_ui.h', b'UI ABI layouts')
        self.write(self.root / 'sdk/include/crml_media.h', b'Media ABI layouts')
        for example in ('hello', 'movement', 'visibility', 'physics-damping', 'input-actions', 'state-watch', 'startup-skip'):
            self.write(self.root / 'examples' / example / 'mod.ini', b'Mod source')
        for name in ('xinput1_4.dll', 'crml/crml_runtime.dll', 'crml/wasmtime.dll', 'crml/crml_host.exe',
                     'crml/crml_wat.exe', 'crml/mods/hello/mod.ini', 'crml/mods/hello/hello.wasm',
                     'examples/movement/mod.ini', 'examples/movement/movement.wasm',
                     'licenses/wasmtime/LICENSE', 'licenses/minhook/LICENSE.txt'):
            self.write(self.dist / name, b'a' * 64)
        self.features(False)
        dependency = mock.patch.object(release, 'WASMTIME_SHA256', release.digest(b'a' * 64))
        dependency.start()
        self.addCleanup(dependency.stop)

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def features(self, probe):
        self.write(self.dist / 'crml/build-features.json', json.dumps({'movement_wasm': True, 'lua_probe': probe}).encode())

    def package(self, include_noclip=False):
        release.package(self.dist, self.out, self.version, self.root, include_noclip=include_noclip)

    def runtime_members(self):
        return {
            'xinput1_4.dll', 'crml/crml_runtime.dll', 'crml/wasmtime.dll',
            'crml/licenses/wasmtime.txt', 'crml/licenses/minhook.txt',
            'compatibility.json', 'THIRD_PARTY.md', 'README-CRML.txt', 'crml/release.json',
            'crml/mods/README.txt',
        }

    def assert_archive_members(self, name, expected):
        with zipfile.ZipFile(self.out / name) as archive:
            self.assertEqual(set(archive.namelist()), expected)

    def rewrite_archive(self, name, change):
        """Make a consistent fixture mutation, including every integrity hash."""
        path = self.out/name
        with zipfile.ZipFile(path) as archive:
            members = {member: archive.read(member) for member in archive.namelist()}
        change(members)
        with zipfile.ZipFile(path, 'w') as archive:
            for member, data in members.items():
                archive.writestr(member, data)
        manifest_path = self.out/'release.json'
        manifest = json.loads(manifest_path.read_text())
        data = path.read_bytes()
        manifest['archives'][name] = {
            'sha256': release.digest(data), 'size': len(data),
            'files': {member: release.digest(data) for member, data in members.items()},
        }
        manifest_path.write_text(json.dumps(manifest))
        (self.out/'SHA256SUMS.txt').write_text(''.join(
            f'{record["sha256"]}  {archive}\n' for archive, record in manifest['archives'].items()))

    def test_packages_and_integrity(self):
        self.write(self.dist / 'crml/private.log', b'Must not ship')
        self.write(self.dist / 'crml/physics-trial.enabled', b'')
        self.package()
        release.verify(self.out, self.version)
        expected_archives = [f'crml-{kind}-{self.version}-windows-x64.zip' for kind in ('runtime', 'sdk')]
        self.assertEqual(release.archive_names(self.version), expected_archives)
        self.assertEqual({path.name for path in self.out.glob('*.zip')}, set(expected_archives))
        self.assert_archive_members(expected_archives[0], self.runtime_members())
        for name in expected_archives:
            with zipfile.ZipFile(self.out / name) as archive:
                names = archive.namelist()
                self.assertNotIn('crml/private.log', names)
                self.assertNotIn('crml/physics-trial.enabled', names)
                self.assertFalse(any(member.startswith('crml/mods/') and member != 'crml/mods/README.txt'
                                     for member in names))
                self.assertNotIn('crml/movement-wasm.enabled', names)
                self.assertEqual('xinput1_4.dll' in names, name == expected_archives[0])
        with zipfile.ZipFile(self.out / expected_archives[1]) as sdk:
            self.assertIn('examples/hello/mod.ini', sdk.namelist())
            self.assertIn('examples/input-actions/mod.ini', sdk.namelist())
            self.assertIn('sdk/include/crml_abi.h', sdk.namelist())
            self.assertIn('sdk/include/crml_state.h', sdk.namelist())
            self.assertIn('examples/state-watch/mod.ini', sdk.namelist())
            self.assertIn('examples/startup-skip/mod.ini', sdk.namelist())
            self.assertIn('sdk/include/crml_ui.h', sdk.namelist())
            self.assertIn('sdk/include/crml_media.h', sdk.namelist())
            self.assertIn('tools/crml_host.exe', sdk.namelist())
            self.assertIn('tools/crml_wat.exe', sdk.namelist())
            self.assertIn('README-LUA.txt', sdk.namelist())
            for name in ('compile_lua.py', 'binlua.py', 'binlua_source.py', 'engine_research.py'):
                self.assertEqual(sdk.read('tools/' + name), (ROOT / 'tools' / name).read_bytes())
            extracted = self.root / 'sdk-extracted'
            sdk.extractall(extracted)
            help_result = subprocess.run([sys.executable, str(extracted / 'tools/compile_lua.py'), '--help'],
                                         cwd=extracted, capture_output=True, text=True, timeout=10)
            self.assertEqual(help_result.returncode, 0, help_result.stderr)
            self.assertIn('--compiler', help_result.stdout)
        archive = self.out / release.archive_names(self.version)[0]
        archive.write_bytes(archive.read_bytes() + b'tamper')
        with self.assertRaisesRegex(ValueError, 'hash/size'):
            release.verify(self.out, self.version)
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.package()

    def test_ui_bridge_payload_required_and_mod_separate(self):
        self.write(self.dist / 'crml/build-features.json', json.dumps({
            'movement_wasm': True, 'lua_probe': False, 'ui_bridge': True}).encode())
        with self.assertRaisesRegex(ValueError, 'bootstrap'):
            self.package()
        self.assertFalse(self.out.exists())
        self.write(self.dist / 'crml/ui-bootstrap.html', b'Trusted bootstrap')
        self.write(self.dist / 'crml/native-ui.enabled', b'')
        self.write(self.dist / 'crml/native-ui-panel.html', b'Unreleased diagnostic')
        self.package()
        self.assert_archive_members(release.archive_names(self.version)[0],
                                    self.runtime_members() | {'crml/ui-bootstrap.html'})
        release.verify(self.out, self.version)

    def test_all_supported_profiles_must_match_runtime_and_metadata(self):
        fingerprints = ['a' * 64, 'b' * 64]
        self.write(self.root / 'compatibility.json', json.dumps({
            'profiles': [{'sha256': value} for value in fingerprints]}).encode())
        with self.assertRaisesRegex(ValueError, 'fingerprint'):
            self.package()
        self.assertFalse(self.out.exists())
        self.write(self.dist / 'crml/crml_runtime.dll', '\n'.join(fingerprints).encode())
        self.package()
        manifest = release.verify(self.out, self.version)
        self.assertEqual(manifest['game_sha256s'], fingerprints)
        self.assertEqual(manifest['game_sha256'], fingerprints[0])
        for name in release.archive_names(self.version):
            with zipfile.ZipFile(self.out / name) as archive:
                metadata = 'release.json' if '-sdk-' in name else 'crml/release.json'
                self.assertEqual(json.loads(archive.read(metadata))['game_sha256s'], fingerprints)
        # Hashes still match, but the advertised support set has been truncated.
        manifest['game_sha256s'] = fingerprints[:1]
        (self.out / 'release.json').write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'profile|fingerprint'):
            release.verify(self.out, self.version)
        manifest['game_sha256s'] = None
        (self.out / 'release.json').write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'profile|fingerprint'):
            release.verify(self.out, self.version)

    def test_invalid_or_duplicate_profiles_refused(self):
        for profiles in ([], [{'sha256': 'not-a-sha'}],
                         [{'sha256': 'a' * 64}, {'sha256': 'a' * 64}],
                         [{'sha256': 'a' * 64}, {}]):
            with self.subTest(profiles=profiles):
                self.write(self.root / 'compatibility.json', json.dumps({'profiles': profiles}).encode())
                with self.assertRaisesRegex(ValueError, 'profile|fingerprint'):
                    self.package()
                self.assertFalse(self.out.exists())

    def test_rehashed_archive_must_retain_every_supported_runtime_profile(self):
        fingerprints = ['a' * 64, 'b' * 64]
        self.write(self.root / 'compatibility.json', json.dumps({
            'profiles': [{'sha256': value} for value in fingerprints]}).encode())
        self.write(self.dist / 'crml/crml_runtime.dll', '\n'.join(fingerprints).encode())
        self.package()
        runtime = release.archive_names(self.version)[0]
        self.rewrite_archive(runtime, lambda members: members.update({
            'crml/crml_runtime.dll': fingerprints[0].encode()}))
        with self.assertRaisesRegex(ValueError, 'Runtime fingerprint missing'):
            release.verify(self.out, self.version)

    def test_modern_profiles_cannot_downgrade_to_legacy_verification(self):
        fingerprints = ['a'*64, 'b'*64]
        self.write(self.root/'compatibility.json', json.dumps({
            'profiles': [{'sha256': value} for value in fingerprints]}).encode())
        self.write(self.dist/'crml/crml_runtime.dll', '\n'.join(fingerprints).encode())
        self.package()
        manifest_path = self.out/'release.json'
        manifest = json.loads(manifest_path.read_text())
        del manifest['game_sha256s']
        for first in (fingerprints[0], 'c'*64):
            with self.subTest(legacy_fingerprint=first):
                manifest['game_sha256'] = first
                manifest_path.write_text(json.dumps(manifest))
                with self.assertRaisesRegex(ValueError, 'Modern archive requires complete'):
                    release.verify(self.out, self.version)

    def test_modern_profiles_must_exist_and_match_in_archive_metadata(self):
        self.package()
        name = release.archive_names(self.version)[1]
        with zipfile.ZipFile(self.out/name) as archive:
            original = json.loads(archive.read('release.json'))
        for value in (None, ['b'*64]):
            with self.subTest(inner_profiles=value):
                changed = dict(original)
                if value is None:
                    del changed['game_sha256s']
                else:
                    changed['game_sha256s'] = value
                self.rewrite_archive(name, lambda members: members.update({
                    'release.json': json.dumps(changed).encode()}))
                with self.assertRaisesRegex(ValueError, 'Archive release fingerprints differ'):
                    release.verify(self.out, self.version)
        self.rewrite_archive(name, lambda members: members.pop('release.json'))
        with self.assertRaisesRegex(ValueError, 'Modern archive is missing release profile metadata'):
            release.verify(self.out, self.version)

    def test_optional_noclip_packages_and_legacy_verification(self):
        self.package(include_noclip=True)
        expected_archives = [f'crml-{kind}-{self.version}-windows-x64.zip'
                             for kind in ('runtime', 'noclip', 'noclip-bundle', 'sdk')]
        self.assertEqual(release.archive_names(self.version, include_noclip=True), expected_archives)
        self.assertEqual({path.name for path in self.out.glob('*.zip')}, set(expected_archives))
        noclip = {
            'crml/movement-wasm.enabled', 'crml/mods/movement/mod.ini',
            'crml/mods/movement/movement.wasm', 'crml/mods/movement/release.json',
            'README-NOCLIP.txt', 'compatibility.json',
        }
        self.assert_archive_members(expected_archives[0], self.runtime_members())
        self.assert_archive_members(expected_archives[1], noclip)
        self.assert_archive_members(expected_archives[2], self.runtime_members() | noclip)
        # Existing four-archive releases have no new distribution-mode field.
        # A genuine legacy archive also predates full-profile inner metadata.
        def legacy_metadata(members):
            for member in ('release.json', 'crml/release.json', 'crml/mods/movement/release.json'):
                if member in members:
                    data = json.loads(members[member])
                    del data['game_sha256s']
                    members[member] = json.dumps(data).encode()
        for name in expected_archives:
            self.rewrite_archive(name, legacy_metadata)
        manifest = json.loads((self.out / 'release.json').read_text())
        legacy_fields = {key: manifest[key] for key in
                         ('schema', 'version', 'source_commit', 'source_dirty', 'game_sha256', 'lua_probe', 'archives')}
        (self.out / 'release.json').write_text(json.dumps(legacy_fields))
        release.verify(self.out, self.version)

    def test_incomplete_or_mixed_archive_sets_refused(self):
        self.package()
        manifest_path = self.out / 'release.json'
        manifest = json.loads(manifest_path.read_text())
        runtime, sdk = release.archive_names(self.version)
        noclip = f'crml-noclip-{self.version}-windows-x64.zip'
        bundle = f'crml-noclip-bundle-{self.version}-windows-x64.zip'
        for names in ((runtime,), (sdk,), (runtime, noclip), (runtime, sdk, noclip), (runtime, sdk, bundle)):
            with self.subTest(archives=names):
                changed = dict(manifest, archives={name: manifest['archives'][runtime] for name in names})
                manifest_path.write_text(json.dumps(changed))
                with self.assertRaisesRegex(ValueError, 'Unexpected release archive set'):
                    release.verify(self.out, self.version)

    def test_probe_and_optional_missing_files_refused(self):
        self.features(True)
        with self.assertRaisesRegex(ValueError, 'without -LuaProbe'):
            self.package()
        self.features(False)
        (self.dist / 'examples/movement/movement.wasm').unlink()
        with self.assertRaises(FileNotFoundError):
            self.package(include_noclip=True)
        self.assertFalse(self.out.exists())

    def test_source_development_build_refused(self):
        self.write(self.dist / 'crml/build-features.json', json.dumps({
            'movement_wasm': True, 'lua_probe': False, 'lua_source': True}).encode())
        with self.assertRaisesRegex(ValueError, '-LuaSource'):
            self.package()
        self.assertFalse(self.out.exists())

    def test_runtime_sdk_do_not_require_compiled_mod_artifacts(self):
        for name in ('examples/movement/mod.ini', 'examples/movement/movement.wasm',
                     'crml/mods/hello/mod.ini', 'crml/mods/hello/hello.wasm'):
            (self.dist / name).unlink()
        (self.root / 'release/README-noclip.txt').unlink()
        self.package()
        release.verify(self.out, self.version)

    def test_private_binary_path_refused(self):
        self.write(self.dist / 'crml/crml_host.exe', str(self.root).encode('utf-16le'))
        with self.assertRaisesRegex(ValueError, 'Private build path'):
            self.package()
        self.assertFalse(self.out.exists())
        # Reproduce hosted Windows runners: the supplied temp directory spelling
        # differs from resolve(), and the account directory can be abbreviated.
        original_resolve = Path.resolve
        def expanded(path, *args, **kwargs):
            return path.parent / 'expanded-account-path' if path == self.root else original_resolve(path, *args, **kwargs)
        with mock.patch.object(Path, 'resolve', expanded), mock.patch.object(Path, 'home', return_value=self.root.parent / 'full-account-name'):
            with self.assertRaisesRegex(ValueError, 'Private build path'):
                self.package()
        self.assertFalse(self.out.exists())

    def test_version_cannot_escape_output(self):
        for version in ('../release', 'v1.2.3/other', 'v1.2.3\n', '--help'):
            with self.assertRaises(ValueError):
                release.checked_version(version)

    def test_upstream_paths_require_exact_dependency_hash(self):
        upstream = str(self.root).encode() + b'upstream build diagnostics'
        self.write(self.dist / 'crml/wasmtime.dll', upstream)
        with self.assertRaisesRegex(ValueError, 'pinned upstream binary'):
            self.package()
        with mock.patch.object(release, 'WASMTIME_SHA256', release.digest(upstream)):
            self.package()
        release.verify(self.out, self.version)


if __name__ == '__main__':
    unittest.main()
