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

    def test_visibility_guest_chooses_key(self):
        source=(Path(__file__).resolve().parents[1] / 'examples/visibility/visibility.wat').read_text()
        self.package(source,manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=input.buttons,player.visibility\n')
        result=subprocess.run([str(BIN / 'crml_gameplay_tests.exe'),str(self.mods),'sequence'],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual([x for x in result.stdout.splitlines() if x.startswith('Visibility request:')],
                         ['Visibility request: '+str(x) for x in [0,1,1,0,0]])
        self.assertIn('failures: 0; owners: 0',result.stdout)
        # Change only guest bytecode: the same native host now responds to F8.
        wat=self.root / 'f8.wat'
        wat.write_text(source.replace('(i32.const 1)) (i32.const 0)', '(i32.const 2)) (i32.const 0)'))
        subprocess.run([str(BIN / 'crml_wat.exe'),str(wat),str(self.mods / 'test/mod.wasm')],check=True,capture_output=True)
        result=subprocess.run([str(BIN / 'crml_gameplay_tests.exe'),str(self.mods),'sequence'],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual([x for x in result.stdout.splitlines() if x.startswith('Visibility request:')],
                         ['Visibility request: '+str(x) for x in [0,0,0,1,0]])

    def test_visibility_set_requires_capability(self):
        self.reject(f'(module (import "crml_v1" "visibility_set" (func (param i32) (result i32))) {BASE} (func (export "crml_init")))','unknown import')

    def test_input_requires_separate_capability(self):
        self.package(f'(module (import "crml_v1" "input_buttons" (func (result i32))) {BASE} (func (export "crml_init")))',manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=player.visibility\n')
        self.assertIn('unknown import',self.run_host(1))

    def test_guest_can_request_visibility_without_input(self):
        self.package(f'(module (import "crml_v1" "visibility_set" (func $set (param i32) (result i32))) {BASE} (func (export "crml_init") i32.const 1 call $set drop) (func (export "crml_tick") (param f32) unreachable))',manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=player.visibility\n')
        result=subprocess.run([str(BIN / 'crml_gameplay_tests.exe'),str(self.mods)],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('Visibility request: 1',result.stdout)
        self.assertIn('Gameplay calls: 1; failures: 1; owners: 0',result.stdout)

    def test_visibility_set_rejects_non_boolean(self):
        self.package(f'(module (import "crml_v1" "visibility_set" (func $set (param i32) (result i32))) {BASE} (func (export "crml_init") i32.const 2 call $set drop))',manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=player.visibility\n')
        self.assertIn('Visibility argument',self.run_host(1))

    def test_input_and_visibility_share_budget(self):
        self.package(f'(module (import "crml_v1" "input_buttons" (func $input (result i32))) (import "crml_v1" "visibility_set" (func $set (param i32) (result i32))) {BASE} (func (export "crml_init") (loop call $input drop i32.const 0 call $set drop br 0)))',manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=input.buttons,player.visibility\n')
        self.assertIn('Gameplay call budget exceeded',self.run_host(1))

    def test_visibility_capability_required(self):
        self.reject(f'(module (import "crml_v1" "visibility_poll" (func (result i32))) {BASE} (func (export "crml_init")))', 'unknown import')

    def test_visibility_lifecycle_release(self):
        self.package(f'(module (import "crml_v1" "visibility_poll" (func $p (result i32))) {BASE} (func (export "crml_init") call $p drop) (func (export "crml_tick") (param f32) unreachable))', manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=player.visibility\n')
        result=subprocess.run([str(BIN / 'crml_gameplay_tests.exe'),str(self.mods)],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('Gameplay calls: 1; failures: 1; owners: 0',result.stdout)

    def test_visibility_budget(self):
        self.package(f'(module (import "crml_v1" "visibility_poll" (func $p (result i32))) {BASE} (func (export "crml_init") (loop call $p drop br 0)))', manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=player.visibility\n')
        self.assertIn('Gameplay call budget exceeded',self.run_host(1))

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

    def test_noclip_capability_denied(self):
        self.reject(f'(module (import "crml_v1" "noclip_poll" (func (param f32) (result i32))) {BASE} (func (export "crml_init")))', 'unknown import')

    def test_packaged_noclip_example(self):
        example = Path(__file__).resolve().parents[1] / 'examples/noclip'
        manifest = (example / 'mod.ini').read_text().replace('noclip.wasm', 'mod.wasm')
        self.package((example / 'noclip.wat').read_text(), manifest=manifest)
        self.assertIn('[noclip] Experimental noclip: unavailable\n', self.run_host())

    def test_noclip_unavailable_without_bridge(self):
        self.package(f'(module (import "crml_v1" "noclip_poll" (func $p (param f32) (result i32))) {BASE} (func (export "crml_init") f32.const 5 call $p i32.const -1 i32.ne if unreachable end))', manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=player.noclip\n')
        self.run_host()

    def gameplay_fixture(self, body, calls, failures):
        self.package(f'(module (import "crml_v1" "noclip_poll" (func $p (param f32) (result i32))) {BASE} {body})', manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=player.noclip\n')
        result = subprocess.run([str(BIN / 'crml_gameplay_tests.exe'), str(self.mods)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'Gameplay calls: {calls}; failures: {failures}; owners: 0', result.stdout)

    def test_noclip_released_after_tick_trap(self):
        self.gameplay_fixture('(func (export "crml_init")) (func (export "crml_tick") (param f32) f32.const 5 call $p drop unreachable)', 1, 1)

    def test_noclip_released_after_start_trap(self):
        self.gameplay_fixture('(func $start f32.const 5 call $p drop unreachable) (start $start) (func (export "crml_init"))', 1, 1)

    def test_noclip_released_after_shutdown_trap(self):
        self.gameplay_fixture('(func (export "crml_init") f32.const 5 call $p drop) (func (export "crml_shutdown") unreachable)', 1, 1)

    def test_noclip_released_without_guest_shutdown(self):
        self.gameplay_fixture('(func (export "crml_init") f32.const 5 call $p drop)', 1, 0)

    def test_noclip_nan_rejected(self):
        self.gameplay_fixture('(func (export "crml_init") f32.const nan call $p drop)', 0, 1)

    def test_noclip_call_budget(self):
        self.gameplay_fixture('(func (export "crml_init") (loop f32.const 5 call $p drop br 0))', 8, 1)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', required=True, type=Path)
    args, remaining = parser.parse_known_args()
    BIN = args.bin.resolve()
    unittest.main(argv=[__file__, *remaining])
