"""Exercise the real Wasmtime runtime with adversarial guest modules."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

BIN = None
BASE = '(func (export "crml_abi_version") (result i32) i32.const 1)'
LOG = '(import "crml_v1" "log" (func $log (param i32 i32 i32)))'

class SandboxTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=BIN.parent)
        self.root = Path(self.temp.name)
        self.mods = self.root / 'mods'
        self.mods.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def package(self, body, name='test', manifest=None):
        folder = self.mods / name
        folder.mkdir()
        (folder / 'mod.ini').write_text(manifest or f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities=log\n')
        wat = self.root / f'{name}.wat'
        wat.write_text(body)
        subprocess.run([str(BIN / 'crml_wat.exe'), str(wat), str(folder / 'mod.wasm')], check=True, capture_output=True)
        return folder

    def run_host(self, code=0):
        result = subprocess.run([str(BIN / 'crml_host.exe'), str(self.mods), '2'], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        return result.stdout

    def reject(self, body, text=None):
        self.package(body)
        output = self.run_host(1)
        self.assertIn('Active: 0; failures: 1', output)
        if text:
            self.assertIn(text, output)

    def test_lifecycle(self):
        self.package(f'''(module {LOG} {BASE}
            (memory (export "memory") 1) (data (i32.const 0) "ITS")
            (func (export "crml_init") i32.const 1 i32.const 0 i32.const 1 call $log)
            (func (export "crml_tick") (param f32) i32.const 1 i32.const 1 i32.const 1 call $log)
            (func (export "crml_shutdown") i32.const 1 i32.const 2 i32.const 1 call $log))''')
        output = self.run_host()
        self.assertEqual([line for line in output.splitlines() if line.startswith('[test]')], ['[test] I', '[test] T', '[test] T', '[test] S'])

    def test_init_loop(self):
        self.reject(f'(module {BASE} (func (export "crml_init") (loop br 0)))', 'fuel')

    def test_start_loop(self):
        self.reject(f'(module {BASE} (func $start (loop br 0)) (start $start) (func (export "crml_init")))', 'fuel')

    def test_version_loop(self):
        self.reject('(module (func (export "crml_abi_version") (result i32) (loop br 0) i32.const 1) (func (export "crml_init")))', 'fuel')

    def test_tick_trap_disables_only_bad_mod(self):
        self.package(f'(module {BASE} (func (export "crml_init")) (func (export "crml_tick") (param f32) unreachable))', 'bad')
        self.package(f'(module {BASE} (func (export "crml_init")))', 'good')
        output = self.run_host(1)
        self.assertIn('Disabled bad:', output)
        self.assertIn('Loaded good', output)
        self.assertIn('Active: 1; failures: 1', output)

    def test_shutdown_trap_returns_failure(self):
        self.package(f'(module {BASE} (func (export "crml_init")) (func (export "crml_shutdown") unreachable))')
        self.assertIn('Disabled test:', self.run_host(1))

    def test_oversized_memory(self):
        self.reject(f'(module {BASE} (memory 257) (func (export "crml_init")))', 'memory')

    def test_memory_growth_denied(self):
        self.package(f'(module {BASE} (memory 1) (func (export "crml_init") i32.const 256 memory.grow i32.const -1 i32.ne if unreachable end))')
        self.run_host()

    def test_large_table(self):
        self.reject(f'(module {BASE} (table 4097 funcref) (func (export "crml_init")))', 'table')

    def test_bad_log_pointer(self):
        self.reject(f'(module {LOG} {BASE} (memory (export "memory") 1) (func (export "crml_init") i32.const 1 i32.const -1 i32.const 10 call $log))', 'outside guest memory')

    def test_log_overflow(self):
        self.reject(f'(module {LOG} {BASE} (memory (export "memory") 1) (func (export "crml_init") i32.const 1 i32.const 0 i32.const -1 call $log))', 'limit exceeded')

    def test_log_spam(self):
        self.reject(f'(module {LOG} {BASE} (memory (export "memory") 1) (func (export "crml_init") (loop i32.const 1 i32.const 0 i32.const 0 call $log br 0)))', 'limit exceeded')

    def test_capability_denied(self):
        self.package(f'(module {LOG} {BASE} (func (export "crml_init")))', manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=\n')
        self.assertIn('unknown import', self.run_host(1))

    def test_wasi_denied(self):
        self.reject(f'(module (import "wasi_snapshot_preview1" "proc_exit" (func (param i32))) {BASE} (func (export "crml_init")))', 'unknown import')

    def test_wrong_abi(self):
        self.reject('(module (func (export "crml_abi_version") (result i32) i32.const 2) (func (export "crml_init")))', 'Unsupported guest ABI')

    def test_wrong_signature(self):
        self.reject(f'(module {BASE} (func (export "crml_init") (param i32)))', 'Wrong signature')

    def test_native_dll_rejected(self):
        folder = self.package(f'(module {BASE} (func (export "crml_init")))')
        (folder / 'mod.wasm').write_bytes(b'MZ' + b'\0' * 100)
        self.assertIn('Only binary core WebAssembly', self.run_host(1))

    def test_path_traversal_rejected(self):
        self.package(f'(module {BASE} (func (export "crml_init")))', manifest='id=test\nabi=1\nmodule=../escape.wasm\n')
        self.assertIn('local .wasm filename', self.run_host(1))

    def test_unknown_capability_rejected(self):
        self.package(f'(module {BASE} (func (export "crml_init")))', manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=filesystem\n')
        self.assertIn('Unsupported capability', self.run_host(1))

    def test_duplicate_id_rejected(self):
        self.package(f'(module {BASE} (func (export "crml_init")))', 'a')
        self.package(f'(module {BASE} (func (export "crml_init")))', 'b', 'id=a\nabi=1\nmodule=mod.wasm\n')
        self.assertIn('Duplicate mod id', self.run_host(1))

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', required=True, type=Path)
    args, remaining = parser.parse_known_args()
    BIN = args.bin.resolve()
    unittest.main(argv=[__file__, *remaining])
