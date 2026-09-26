import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('installer', ROOT / 'tools' / 'install.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)

class InstallerTests(unittest.TestCase):
    def test_inspector_requires_build_support(self):
        features=self.dist / 'crml/build-features.json'
        features.write_text('{"experimental_gameplay": false}')
        with self.assertRaises(ValueError):
            installer.sources_for(self.dist, entity_inspector=True)
        features.write_text('{"experimental_gameplay": true}')
        marker=self.dist / 'crml/entity-inspector.enabled'
        marker.write_text('')
        sources=installer.sources_for(self.dist, entity_inspector=True)
        self.assertEqual(sources['crml/entity-inspector.enabled'],marker)
        self.assertNotIn('crml/noclip.enabled',sources)
    def test_inspector_receipt_roundtrip(self):
        (self.dist / 'crml/build-features.json').write_text('{"experimental_gameplay": true}')
        (self.dist / 'crml/entity-inspector.enabled').write_text('')
        installer.install(self.game, self.dist, self.profiles, True, entity_inspector=True)
        self.assertIn('crml/entity-inspector.enabled', installer.read_receipt(self.game)['files'])
        installer.update(self.game, self.dist, self.profiles, True, entity_inspector=True)
        installer.uninstall(self.game, True)
        self.assertFalse((self.game / 'crml/entity-inspector.enabled').exists())

    def test_visibility_roundtrip(self):
        (self.dist / 'crml/build-features.json').write_text('{"experimental_gameplay": true}')
        for name in installer.VISIBILITY_FILES.values():
            p=self.dist / name
            p.parent.mkdir(parents=True,exist_ok=True)
            p.write_text('fixture')
        installer.install(self.game,self.dist,self.profiles,True,experimental_visibility=True)
        installer.update(self.game,self.dist,self.profiles,True,experimental_visibility=True)
        installer.uninstall(self.game,True)
        self.assertFalse((self.game / 'crml/visibility.enabled').exists())

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.root = Path(self.temp.name)
        self.game = self.root / 'game'
        self.game.mkdir()
        (self.game / 'CONTROLResonant.exe').write_bytes(b'fixture, not a game')
        self.profiles = [{'sha256': installer.digest(self.game / 'CONTROLResonant.exe'), 'executable': 'CONTROLResonant.exe'}]
        self.dist = self.root / 'dist'
        for name in installer.FILES:
            name = 'licenses/wasmtime/LICENSE' if name == 'crml/licenses/wasmtime.txt' else name
            path = self.dist / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
        self.output = contextlib.redirect_stdout(io.StringIO())
        self.output.__enter__()

    def tearDown(self):
        self.output.__exit__(None, None, None)
        self.temp.cleanup()

    def test_preview_writes_nothing(self):
        installer.install(self.game, self.dist, self.profiles)
        self.assertEqual([p.name for p in self.game.iterdir()], ['CONTROLResonant.exe'])

    def test_unknown_game_refused(self):
        with self.assertRaisesRegex(ValueError, 'Unknown game'):
            installer.install(self.game, self.dist, [])
        self.assertFalse((self.game / 'crml').exists())

    def test_existing_proxy_preserved(self):
        path = self.game / 'xinput1_4.dll'
        path.write_bytes(b'other mod')
        with self.assertRaisesRegex(ValueError, 'Refusing existing'):
            installer.install(self.game, self.dist, self.profiles, True)
        self.assertEqual(path.read_bytes(), b'other mod')

    def test_roundtrip_preserves_extra_files(self):
        installer.install(self.game, self.dist, self.profiles, True)
        extra = self.game / 'crml' / 'personal.txt'
        extra.write_text('keep')
        installer.uninstall(self.game)
        self.assertTrue((self.game / 'xinput1_4.dll').exists())
        installer.uninstall(self.game, True)
        self.assertFalse((self.game / 'xinput1_4.dll').exists())
        self.assertEqual(extra.read_text(), 'keep')
        self.assertEqual((self.game / 'CONTROLResonant.exe').read_bytes(), b'fixture, not a game')

    def test_modified_install_refuses_removal(self):
        installer.install(self.game, self.dist, self.profiles, True)
        path = self.game / 'crml' / 'crml_runtime.dll'
        path.write_bytes(b'modified')
        with self.assertRaisesRegex(ValueError, 'modified'):
            installer.uninstall(self.game, True)
        self.assertTrue((self.game / 'xinput1_4.dll').exists())
        self.assertEqual(path.read_bytes(), b'modified')

    def test_update_preview_and_preservation(self):
        installer.install(self.game, self.dist, self.profiles, True)
        extra = self.game / 'crml' / 'personal.txt'
        extra.write_text('keep')
        target = self.game / 'crml' / 'crml_runtime.dll'
        original = target.read_bytes()
        (self.dist / 'crml' / 'crml_runtime.dll').write_bytes(b'new runtime')
        license = self.dist / 'licenses' / 'minhook' / 'LICENSE.txt'
        license.parent.mkdir(parents=True)
        license.write_text('MinHook license fixture')
        installer.update(self.game, self.dist, self.profiles)
        self.assertEqual(target.read_bytes(), original)
        installer.update(self.game, self.dist, self.profiles, True)
        self.assertEqual(target.read_bytes(), b'new runtime')
        self.assertEqual(extra.read_text(), 'keep')
        installer.uninstall(self.game, True)
        self.assertFalse((self.game / 'crml/licenses/minhook.txt').exists())
        self.assertEqual(extra.read_text(), 'keep')

    def test_update_refuses_modified_file_and_changed_game(self):
        installer.install(self.game, self.dist, self.profiles, True)
        target = self.game / 'crml/crml_runtime.dll'
        target.write_bytes(b'user changed this')
        with self.assertRaisesRegex(ValueError, 'modified'):
            installer.update(self.game, self.dist, self.profiles, True)
        target.write_bytes((self.dist / 'crml/crml_runtime.dll').read_bytes())
        (self.game / 'CONTROLResonant.exe').write_bytes(b'updated game')
        with self.assertRaisesRegex(ValueError, 'Unknown game'):
            installer.update(self.game, self.dist, self.profiles, True)

    def test_update_rolls_back_publish_failure(self):
        installer.install(self.game, self.dist, self.profiles, True)
        before = {p.relative_to(self.game): p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        (self.dist / 'crml/crml_runtime.dll').write_bytes(b'new runtime')
        original_replace = Path.replace
        def fail_receipt(path, target):
            if path.name.endswith('.new') and Path(target).name == 'install-receipt.json':
                raise OSError('simulated publish failure')
            return original_replace(path, target)
        with patch.object(Path, 'replace', fail_receipt):
            with self.assertRaisesRegex(OSError, 'simulated'):
                installer.update(self.game, self.dist, self.profiles, True)
        after = {p.relative_to(self.game): p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        self.assertEqual(before, after)

    def test_update_refuses_running_game(self):
        installer.install(self.game, self.dist, self.profiles, True)
        with patch.object(installer, 'require_closed', side_effect=ValueError('Close CONTROL Resonant')):
            with self.assertRaisesRegex(ValueError, 'Close CONTROL'):
                installer.update(self.game, self.dist, self.profiles, True)

    def test_noclip_install_requires_matching_build(self):
        features = self.dist / 'crml/build-features.json'
        features.write_text(json.dumps({'experimental_gameplay': False}))
        with self.assertRaisesRegex(ValueError, 'Rebuild'):
            installer.install(self.game, self.dist, self.profiles, True, True)
        self.assertFalse((self.game / 'crml').exists())
        features.write_text(json.dumps({'experimental_gameplay': True}))
        for name, source in installer.NOCLIP_FILES.items():
            target = self.dist / source
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b'' if name.endswith('.enabled') else b'fixture')
        installer.install(self.game, self.dist, self.profiles, True, True)
        self.assertTrue((self.game / 'crml/noclip.enabled').exists())
        installer.uninstall(self.game, True)
        self.assertFalse((self.game / 'crml/noclip.enabled').exists())

    def test_receipt_path_injection_refused(self):
        installer.install(self.game, self.dist, self.profiles, True)
        receipt = self.game / installer.RECEIPT
        receipt.write_text('{"schema":1,"files":{"../outside":"hash"}}')
        with self.assertRaisesRegex(ValueError, 'Invalid installation receipt'):
            installer.uninstall(self.game, True)
        self.assertTrue((self.game / 'xinput1_4.dll').exists())

if __name__ == '__main__':
    unittest.main()
