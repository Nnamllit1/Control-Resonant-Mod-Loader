"""Release boundaries: exact archive contents, disabled probes, integrity, no overwrite."""
import importlib.util
import json
from pathlib import Path
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
        for name in ('README-runtime.txt', 'README-noclip.txt', self.version + '.md'):
            self.write(self.root / 'release' / name, b'Public instructions')
        self.write(self.root / 'compatibility.json', json.dumps({'profiles': [{'sha256': 'a' * 64}]}).encode())
        self.write(self.root / 'THIRD_PARTY.md', b'Licenses')
        self.write(self.root / 'sdk/include/crml.h', b'SDK')
        for example in ('hello', 'movement', 'visibility', 'physics-damping'):
            self.write(self.root / 'examples' / example / 'mod.ini', b'Mod source')
        for name in ('xinput1_4.dll', 'crml/crml_runtime.dll', 'crml/wasmtime.dll', 'crml/crml_host.exe',
                     'crml/crml_wat.exe', 'crml/mods/hello/mod.ini', 'crml/mods/hello/hello.wasm',
                     'examples/movement/mod.ini', 'examples/movement/movement.wasm',
                     'licenses/wasmtime/LICENSE', 'licenses/minhook/LICENSE.txt'):
            self.write(self.dist / name, b'a' * 64)
        self.features(False)

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def features(self, probe):
        self.write(self.dist / 'crml/build-features.json', json.dumps({'movement_wasm': True, 'lua_probe': probe}).encode())

    def package(self):
        release.package(self.dist, self.out, self.version, self.root)

    def test_packages_and_integrity(self):
        self.write(self.dist / 'crml/private.log', b'Must not ship')
        self.write(self.dist / 'crml/physics-trial.enabled', b'')
        self.package()
        release.verify(self.out, self.version)
        for index, name in enumerate(release.archive_names(self.version)):
            with zipfile.ZipFile(self.out / name) as archive:
                names = archive.namelist()
                self.assertNotIn('crml/private.log', names)
                self.assertNotIn('crml/physics-trial.enabled', names)
                self.assertEqual('crml/movement-wasm.enabled' in names, index in (1, 2))
                self.assertEqual('xinput1_4.dll' in names, index in (0, 2))
        archive = self.out / release.archive_names(self.version)[0]
        archive.write_bytes(archive.read_bytes() + b'tamper')
        with self.assertRaisesRegex(ValueError, 'hash/size'):
            release.verify(self.out, self.version)
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.package()

    def test_probe_and_missing_files_refused(self):
        self.features(True)
        with self.assertRaisesRegex(ValueError, 'without -LuaProbe'):
            self.package()
        self.features(False)
        (self.dist / 'examples/movement/movement.wasm').unlink()
        with self.assertRaises(FileNotFoundError):
            self.package()
        self.assertFalse(self.out.exists())

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


if __name__ == '__main__':
    unittest.main()
