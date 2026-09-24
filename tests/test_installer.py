import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('installer', ROOT / 'tools' / 'install.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)

class InstallerTests(unittest.TestCase):
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

    def test_receipt_path_injection_refused(self):
        installer.install(self.game, self.dist, self.profiles, True)
        receipt = self.game / installer.RECEIPT
        receipt.write_text('{"schema":1,"files":{"../outside":"hash"}}')
        with self.assertRaisesRegex(ValueError, 'Invalid installation receipt'):
            installer.uninstall(self.game, True)
        self.assertTrue((self.game / 'xinput1_4.dll').exists())

if __name__ == '__main__':
    unittest.main()
