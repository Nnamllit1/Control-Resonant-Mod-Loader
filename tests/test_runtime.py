"""Exercise the real Wasmtime runtime with adversarial guest modules."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest
import re
import struct

BIN = None
BASE = '(func (export "crml_abi_version") (result i32) i32.const 1)'
LOG = '(import "crml_v1" "log" (func $log (param i32 i32 i32)))'

class SandboxTests(unittest.TestCase):
    def rule_guest(self, body, caps='player.action_rules'):
        self.package(f'''(module
          (import "crml_v1" "action_rule_set" (func $set (param i32 i32) (result i32)))
          (import "crml_v1" "action_rule_read" (func $read (param i32 i32) (result i32)))
          {BASE} (memory (export "memory") 1)
          (func (export "crml_init") {body}))''',
          manifest=f'id=test\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\n')

    def test_action_rules_capability_required(self):
        self.rule_guest('', '')
        self.assertIn('unknown import', self.run_host(1))

    def test_action_rules_unavailable_and_invalid_masks(self):
        self.rule_guest('''i32.const 31 i32.const 1 call $set i32.const -1 i32.ne if unreachable end
          i32.const 32 i32.const 1 call $set i32.const -3 i32.ne if unreachable end
          i32.const 1 i32.const 2 call $set i32.const -3 i32.ne if unreachable end
          i32.const 0 i32.const 1 call $set i32.const -3 i32.ne if unreachable end
          i32.const 0 i32.const 99 i32.store
          i32.const 0 i32.const 32 call $read i32.const -1 i32.ne if unreachable end
          i32.const 0 i32.load if unreachable end''')
        self.run_host()

    def test_action_rules_snapshot_memory(self):
        self.rule_guest('i32.const 65520 i32.const 32 call $read drop')
        self.assertIn('Snapshot output outside guest memory', self.run_host(1))

    def test_action_rules_snapshot_layout(self):
        self.rule_guest('i32.const 0 i32.const 28 call $read drop')
        self.assertIn('Snapshot output size does not match ABI', self.run_host(1))

    def action_rule_owner_fixture(self, trap):
        self.rule_guest('''i32.const 31 i32.const 1 call $set i32.const 1 i32.ne if unreachable end
          i32.const 0 i32.const 32 call $read i32.const 1 i32.ne if unreachable end
          i32.const 4 i32.load i32.const 1 i32.ne if unreachable end
          i32.const 8 i32.load i32.const 31 i32.ne if unreachable end''' + (' unreachable' if trap else ''))
        result=subprocess.run([str(BIN/'crml_action_rules_guest_tests.exe'),str(self.mods),'trap' if trap else 'success'],
                              capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('Owner request and automatic cleanup passed',result.stdout)

    def test_action_rules_owner_cleanup_on_shutdown(self):
        self.action_rule_owner_fixture(False)

    def test_action_rules_owner_cleanup_on_init_trap(self):
        self.action_rule_owner_fixture(True)

    def test_action_rules_command_budget(self):
        self.rule_guest('(loop i32.const 1 i32.const 1 call $set drop br 0)')
        self.assertIn('Gameplay call budget exceeded', self.run_host(1))

    def tutorial_page_guest(self, offset=0, length=1444, caps='tutorials',
                            memory='(memory (export "memory") 1)', version=1, expected=None, loop=False):
        page = struct.pack('<8I', version, 0, 1000, 0, 0, 0, 0, 0)
        page += b'Title'.ljust(129, b'\0') + b'Body'.ljust(1025, b'\0') + bytes(257) + bytes(1)
        at = offset if 0 <= offset <= 65536-len(page) else 0
        encoded = ''.join(f'\\{byte:02x}' for byte in page)
        data = f'(data (i32.const {at}) "{encoded}")' if memory.startswith('(memory') else ''
        result = 'drop' if expected is None else f'i64.const {expected} i64.ne if unreachable end'
        call = f'i32.const {offset} i32.const {length} call $present {result}'
        if loop:
            call = f'(loop {call} br 0)'
        self.package(f'''(module
          (import "crml_v1" "tutorial_present" (func $present (param i32 i32) (result i64)))
          {BASE} {memory} {data} (func (export "crml_init") {call}))''',
          manifest=f'id=test\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\n')

    def test_tutorial_page_capability_required(self):
        self.tutorial_page_guest(caps='')
        self.assertIn('unknown import', self.run_host(1))

    def test_tutorial_page_range_checked_without_renderer(self):
        self.tutorial_page_guest(offset=-1)
        self.assertIn('Tutorial descriptor outside guest memory', self.run_host(1))

    def test_tutorial_page_size_checked_before_copy(self):
        self.tutorial_page_guest(length=1445)
        self.assertIn('Invalid tutorial descriptor size', self.run_host(1))

    def test_tutorial_page_requires_memory_export(self):
        self.tutorial_page_guest(memory='')
        self.assertIn('Missing guest memory', self.run_host(1))

    def test_tutorial_page_rejects_wrong_memory_kind(self):
        self.tutorial_page_guest(memory='(global (export "memory") i32 (i32.const 0))')
        self.assertIn('Invalid guest memory', self.run_host(1))

    def test_tutorial_page_accepts_unaligned_descriptor(self):
        self.tutorial_page_guest(offset=3, expected=-1)
        self.run_host()

    def test_tutorial_page_last_valid_memory_range(self):
        self.tutorial_page_guest(offset=65536-1444, expected=-1)
        self.run_host()

    def test_tutorial_page_rejects_newer_version(self):
        self.tutorial_page_guest(version=3, expected=-3)
        self.run_host()

    def test_tutorial_page_accepts_options_version(self):
        self.tutorial_page_guest(version=2, expected=-1)
        self.run_host()

    def test_tutorial_page_shares_tutorial_call_budget(self):
        self.tutorial_page_guest(loop=True)
        self.assertIn('Tutorial call budget exceeded', self.run_host(1))

    def tutorial_guest(self, body, caps='tutorials'):
        self.package(f'''(module
          (import "crml_v1" "tutorial_available" (func $available (param i32) (result i32)))
          (import "crml_v1" "tutorial_show" (func $show (param i32 i32 i32 i32 i32 i32) (result i64)))
          (import "crml_v1" "tutorial_status" (func $status (param i64) (result i32)))
          (import "crml_v1" "tutorial_dismiss" (func $dismiss (param i64) (result i32)))
          {BASE} (memory (export "memory") 1) (data (i32.const 0) "TitleBody")
          (func (export "crml_init") {body}))''',
          manifest=f'id=test\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\n')

    def test_tutorial_imports_require_capability(self):
        self.tutorial_guest('', '')
        self.assertIn('unknown import', self.run_host(1))

    def test_tutorial_offline_availability_and_receipts(self):
        self.tutorial_guest('''i32.const 0 call $available if unreachable end
          i32.const 1 call $available if unreachable end
          i32.const 2 call $available if unreachable end
          i32.const 3 call $available i32.const -3 i32.ne if unreachable end
          i32.const 0 i32.const 0 i32.const 5 i32.const 5 i32.const 4 i32.const 6000
          call $show i64.const -1 i64.ne if unreachable end
          i64.const 1 call $status i32.const -2 i32.ne if unreachable end
          i64.const 1 call $dismiss i32.const -2 i32.ne if unreachable end''')
        self.run_host()

    def test_tutorial_memory_checked_even_without_renderer(self):
        self.tutorial_guest('i32.const 0 i32.const -1 i32.const 5 i32.const 5 i32.const 4 i32.const 6000 call $show drop')
        self.assertIn('Tutorial buffer outside guest memory', self.run_host(1))

    def test_tutorial_text_size_checked_before_copy(self):
        self.tutorial_guest('i32.const 0 i32.const 0 i32.const 129 i32.const 5 i32.const 4 i32.const 6000 call $show drop')
        self.assertIn('Invalid tutorial text size', self.run_host(1))

    def test_tutorial_callback_budget(self):
        self.tutorial_guest('(loop i32.const 0 call $available drop br 0)')
        self.assertIn('Tutorial call budget exceeded', self.run_host(1))

    def profile_host(self, code=0):
        result = subprocess.run([str(BIN/'crml_host.exe'), str(self.mods), '2', '--profile'],
                                capture_output=True, text=True, encoding='utf-8', timeout=10)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        records = {}
        for line in result.stdout.splitlines():
            if '[host] Metrics ' not in line:
                continue
            name, fields = line.split('[host] Metrics ', 1)[1].split(' ', 1)
            values = dict(field.split('=', 1) for field in fields.split())
            records[name, values.get('phase', 'load')] = values
        return records

    def test_metrics_count_callbacks_and_separate_host_budgets(self):
        self.package(f'''(module
          (import "crml_v1" "clock_ms" (func $clock (result i64)))
          (import "crml_v1" "release" (func $release))
          {BASE} (func (export "crml_init") call $clock drop)
          (func (export "crml_tick") (param f32)
            call $clock drop call $clock drop call $release)
          (func (export "crml_shutdown") call $release))''')
        records = self.profile_host()
        self.assertEqual(records['test', 'load']['state'], 'stopped')
        self.assertGreaterEqual(int(records['test', 'load']['load_ns']), int(records['test', 'load']['compile_ns']))
        for phase, calls in [('start', 1), ('abi', 1), ('init', 1), ('tick', 2), ('shutdown', 1)]:
            values = records['test', phase]
            self.assertEqual(int(values['calls']), calls)
            self.assertEqual(int(values['failures']), 0)
            self.assertEqual(int(values['fuel_errors']), 0)
            self.assertGreaterEqual(int(values['total_ns']), int(values['max_ns']))
        self.assertEqual(int(records['test', 'tick']['reads_peak']), 2)
        self.assertEqual(int(records['test', 'tick']['commands_peak']), 1)
        self.assertEqual(int(records['test', 'init']['reads_peak']), 1)
        self.assertEqual(int(records['test', 'shutdown']['reads_peak']), 0)
        self.assertGreater(int(records['test', 'tick']['fuel_peak']), 0)
        self.assertNotIn('[host] Metrics ', self.run_host())

    def test_metrics_retain_failed_start_and_healthy_mod(self):
        self.package(f'''(module {BASE} (func $start (loop br 0)) (start $start)
          (func (export "crml_init")))''', name='a-bad')
        self.package(f'(module {BASE} (func (export "crml_init")))', name='b-good')
        records = self.profile_host(1)
        self.assertEqual(records['a-bad', 'load']['state'], 'rejected')
        self.assertEqual(int(records['a-bad', 'start']['failures']), 1)
        self.assertGreater(int(records['a-bad', 'start']['fuel_peak']), 0)
        self.assertNotIn(('a-bad', 'init'), records)
        self.assertEqual(records['b-good', 'load']['state'], 'stopped')
        self.assertEqual(int(records['b-good', 'init']['failures']), 0)

    def test_metrics_retain_trapped_tick_budget_and_stop_running_guest(self):
        self.package(f'''(module
          (import "crml_v1" "clock_ms" (func $clock (result i64)))
          {BASE} (func (export "crml_init"))
          (func (export "crml_tick") (param f32) (loop call $clock drop br 0)))''')
        records = self.profile_host(1)
        self.assertEqual(records['test', 'load']['state'], 'disabled')
        self.assertEqual(int(records['test', 'tick']['calls']), 1)
        self.assertEqual(int(records['test', 'tick']['failures']), 1)
        self.assertEqual(int(records['test', 'tick']['reads_peak']), 9)

    def binding_package(self, body, name='test', caps='input.actions', binding='F10',
                        memory='(memory (export "memory") 1) (data (i32.const 0) "F10NoneEscape")'):
        self.package(f'''(module
            (import "crml_v1" "input_bind" (func $bind (param i32 i32 i32) (result i32)))
            (import "crml_v1" "input_read" (func $read (param i32 i32) (result i32)))
            (import "crml_v1" "input_actions" (func $actions (result i32)))
            (import "crml_v1" "release" (func $release))
            {BASE} {memory} {body})''', name=name,
            manifest=f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\n'+
                     (f'action.0={binding}\n' if binding else ''))

    def test_input_binding_standalone_snapshot_and_unaligned_output(self):
        self.binding_package('''(func (export "crml_init")
          i32.const 257 i32.const 232 call $read i32.const 1 i32.ne if unreachable end
          i32.const 257 i32.load i32.const 232 i32.ne if unreachable end
          i32.const 261 i32.load i32.const 1 i32.ne if unreachable end
          i32.const 265 i32.load if unreachable end
          i32.const 269 i32.load if unreachable end
          i32.const 273 i32.load i32.const 1 i32.ne if unreachable end
          i32.const 289 i64.load i64.const 1 i64.ne if unreachable end
          i32.const 297 i32.load i32.const 0x00303146 i32.ne if unreachable end
          i32.const 0 i32.const 0 i32.const 3 call $bind if unreachable end
          i32.const 0 i32.const 3 i32.const 4 call $bind i32.const 1 i32.ne if unreachable end
          call $release
          i32.const 257 i32.const 232 call $read drop
          i32.const 273 i32.load if unreachable end
          i32.const 289 i64.load i64.const 2 i64.ne if unreachable end
          call $actions if unreachable end)''')
        self.assertIn('Active: 1; failures: 0', self.run_host())

    def test_input_binding_rejects_unbounded_access(self):
        cases=[('i32.const 16 i32.const 0 i32.const 3 call $bind',-3),
               ('i32.const 0 i32.const 7 i32.const 6 call $bind',-3)]
        for i,(call,result) in enumerate(cases):
            self.binding_package(f'(func (export "crml_init") {call} i32.const {result} i32.ne if unreachable end)',name=f'bad{i}')
        self.assertIn('Active: 2; failures: 0',self.run_host())

    def test_input_binding_memory_and_permission_checks(self):
        cases=[('i32.const 65500 i32.const 232 call $read drop','Input range outside guest memory'),
               ('i32.const 256 i32.const 231 call $read drop','Input output size does not match ABI'),
               ('i32.const 0 i32.const 65535 i32.const 3 call $bind drop','Input range outside guest memory'),
               ('i32.const 0 i32.const 0 i32.const 12 call $bind drop','Input key name length out of range'),
               ('i32.const 0 i32.const 0 i32.const 0 call $bind drop','Input key name length out of range')]
        for i,(call,error) in enumerate(cases):
            with self.subTest(error=error):
                self.mods=self.root/f'input-memory-{i}';self.mods.mkdir()
                self.binding_package(f'(func (export "crml_init") {call})')
                self.assertIn(error,self.run_host(1))
        self.mods=self.root/'input-no-permission';self.mods.mkdir()
        self.binding_package('(func (export "crml_init"))',caps='',binding='')
        self.assertIn('unknown import',self.run_host(1))

    def test_input_binding_budget_and_snapshot_budget(self):
        self.binding_package('(func (export "crml_init") (loop i32.const 0 i32.const 0 i32.const 3 call $bind drop br 0))')
        self.assertIn('Input binding call budget exceeded',self.run_host(1))
        self.mods=self.root/'input-read-budget';self.mods.mkdir()
        self.binding_package('(func (export "crml_init") (loop i32.const 256 i32.const 232 call $read drop br 0))')
        self.assertIn('Observation call budget exceeded',self.run_host(1))

    def test_input_binding_requires_exported_linear_memory(self):
        for i,memory in enumerate(('', '(global (export "memory") i32 (i32.const 0))')):
            for j,call in enumerate(('i32.const 0 i32.const 0 i32.const 3 call $bind drop',
                                     'i32.const 256 i32.const 232 call $read drop')):
                self.mods=self.root/f'input-memory-kind-{i}-{j}';self.mods.mkdir()
                self.binding_package(f'(func (export "crml_init") {call})',memory=memory)
                self.assertIn('Invalid guest memory' if memory else 'Missing guest memory',self.run_host(1))

    def test_input_binding_all_slots_fit_per_invocation(self):
        calls=''.join(f'i32.const {slot} i32.const 0 i32.const 3 call $bind drop ' for slot in range(16))
        self.binding_package(f'''(func (export "crml_init") {calls})
          (func (export "crml_tick") (param f32) {calls})
          (func (export "crml_shutdown") {calls})''')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_input_binding_legacy_provider_has_unknown_context(self):
        self.binding_package('''(func (export "crml_init")
          i32.const 256 i32.const 232 call $read drop
          i32.const 264 i32.load i32.const 1 i32.ne if unreachable end
          i32.const 268 i32.load i32.const 1 i32.ne if unreachable end
          i32.const 0 i32.const 3 i32.const 4 call $bind drop
          i32.const 256 i32.const 232 call $read drop
          i32.const 268 i32.load if unreachable end)''')
        self.assertIn('failures: 0; owners: 0',self.run_gameplay())

    def test_input_binding_other_owner_fault_removes_shared_mask(self):
        self.binding_package('''(global $tick (mut i32) (i32.const 0))
          (func (export "crml_init"))
          (func (export "crml_tick") (param f32)
            i32.const 256 i32.const 232 call $read drop
            i32.const 280 i32.load global.get $tick i32.eqz i32.ne if unreachable end
            i32.const 288 i64.load i64.const 1 i64.ne if unreachable end
            global.get $tick i32.const 1 i32.add global.set $tick)''',name='a')
        self.binding_package('''(func (export "crml_init")
          i32.const 256 i32.const 232 call $read drop
          i32.const 280 i32.load i32.const 1 i32.ne if unreachable end)
          (func (export "crml_tick") (param f32) unreachable)''',name='b')
        output=self.run_host(1)
        self.assertIn('Disabled b:',output)
        self.assertNotIn('Disabled a:',output)
        self.assertIn('Active: 1; failures: 1',output)

    def test_input_binding_rejected_init_does_not_leave_conflict(self):
        self.binding_package('(func (export "crml_init") unreachable)',name='a')
        self.binding_package('''(func (export "crml_init")
          i32.const 256 i32.const 232 call $read drop
          i32.const 280 i32.load if unreachable end)''',name='b')
        output=self.run_host(1)
        self.assertIn('Rejected a:',output)
        self.assertIn('Loaded b',output)

    def settings_package(self, body, name='test', caps='settings', payload=None):
        if payload is None:
            payload=struct.pack('<II32s96s192sdddd',1,3,b'speed',b'Speed',b'Movement speed',2,0,10,0.25)
        data=''.join(f'\\{b:02x}' for b in payload)
        return self.package(f'''(module
          (import "crml_v1" "settings_register" (func $define (param i32 i32) (result i32)))
          (import "crml_v1" "settings_read" (func $read (param i32 i32) (result i32)))
          (import "crml_v1" "settings_set" (func $set (param i32 f64 i64) (result i32)))
          (import "crml_v1" "capabilities" (func $caps (result i32)))
          {BASE} (memory (export "memory") 1) (data (i32.const 0) "{data}") {body})''',name=name,
          manifest=f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\n')

    def test_settings_typed_snapshot_revision_and_owner_isolation(self):
        self.settings_package('''(func (export "crml_init")
          call $caps i32.const 65536 i32.ne if unreachable end
          i32.const 0 i32.const 360 call $define i32.const 1 i32.ne if unreachable end
          i32.const 0 i32.const 360 call $define i32.const -4 i32.ne if unreachable end
          i32.const 1 f64.const 3 i64.const 1 call $set i32.const 1 i32.ne if unreachable end
          i32.const 1 f64.const 4 i64.const 1 call $set i32.const -4 i32.ne if unreachable end
          i32.const 512 i32.const 24 call $read i32.const 1 i32.ne if unreachable end
          i32.const 520 f64.load f64.const 3 f64.ne if unreachable end
          i32.const 528 i64.load i64.const 2 i64.ne if unreachable end)''',name='first')
        self.settings_package('''(func (export "crml_init")
          i32.const 0 i32.const 360 call $define drop
          i32.const 512 i32.const 24 call $read drop
          i32.const 520 f64.load f64.const 2 f64.ne if unreachable end)''',name='second')
        self.assertIn('Active: 2; failures: 0',self.run_host())

    def test_settings_permission_memory_and_definition_validation(self):
        self.settings_package('(func (export "crml_init"))',caps='log')
        self.assertIn('unknown import',self.run_host(1))
        for i,(call,message) in enumerate([
            ('i32.const 0 i32.const 359 call $define drop','Invalid setting definition size'),
            ('i32.const 65535 i32.const 360 call $define drop','outside guest memory'),
            ('i32.const 0 i32.const 23 call $read drop','Invalid settings output size'),
            ('i32.const -1 i32.const 24 call $read drop','outside guest memory')]):
            self.mods=self.root/f'settings-bad-{i}';self.mods.mkdir()
            self.settings_package(f'(func (export "crml_init") {call})')
            self.assertIn(message,self.run_host(1))
        self.mods=self.root/'settings-invalid-definition';self.mods.mkdir()
        self.settings_package('''(func (export "crml_init")
            i32.const 0 i32.const 360 call $define i32.const -3 i32.ne if unreachable end)''',payload=bytes(360))
        self.assertIn('failures: 0',self.run_host())

    def test_settings_budget_and_invalid_values(self):
        calls='i32.const 512 i32.const 0 call $read drop '*64
        self.settings_package(f'''(func (export "crml_init") {calls} call $caps drop)
            (func (export "crml_tick") (param f32) {calls})''')
        self.assertIn('failures: 0',self.run_host())
        self.mods=self.root/'settings-budget';self.mods.mkdir()
        self.settings_package(f'(func (export "crml_init") {calls} i32.const 512 i32.const 0 call $read drop)')
        self.assertIn('Settings call budget exceeded',self.run_host(1))
        self.mods=self.root/'settings-values';self.mods.mkdir()
        self.settings_package('''(func (export "crml_init")
            i32.const 0 i32.const 360 call $define drop
            i32.const 1 f64.const nan i64.const 0 call $set i32.const -3 i32.ne if unreachable end
            i32.const 1 f64.const 0.1 i64.const 0 call $set i32.const -3 i32.ne if unreachable end
            i32.const 2 f64.const 1 i64.const 0 call $set i32.const -2 i32.ne if unreachable end)''')
        self.assertIn('failures: 0',self.run_host())

    def text_settings_package(self, body, name='test', caps='settings', payload=None):
        payload = payload if payload is not None else struct.pack('<II32s96s192s256s',1,16,b'name',b'Name',b'Short name',b'First')
        data=''.join(f'\\{b:02x}' for b in payload)
        return self.package(f'''(module
          (import "crml_v1" "settings_text_register" (func $define (param i32 i32) (result i32)))
          (import "crml_v1" "settings_text_read" (func $read (param i32 i32 i32) (result i32)))
          (import "crml_v1" "settings_text_set" (func $set (param i32 i32 i32 i64) (result i32)))
          (import "crml_v1" "settings_read" (func $numeric (param i32 i32) (result i32)))
          {BASE} (memory (export "memory") 1) (data (i32.const 0) "{data}") {body})''',name=name,
          manifest=f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\nmin_runtime=0.1.0-alpha.4.4.dev.1\n')

    def test_text_settings_real_guest_roundtrip_and_owner_isolation(self):
        self.text_settings_package('''(data (i32.const 800) "Next") (func (export "crml_init")
          i32.const 0 i32.const 584 call $define i32.const 1 i32.ne if unreachable end
          i32.const 1 i32.const 1024 i32.const 280 call $read i32.const 1 i32.ne if unreachable end
          i32.const 1044 i32.load i32.const 5 i32.ne if unreachable end
          i32.const 1 i32.const 800 i32.const 4 i64.const 1 call $set i32.const 1 i32.ne if unreachable end
          i32.const 1 i32.const 800 i32.const 4 i64.const 2 call $set i32.const 0 i32.ne if unreachable end
          i32.const 1 i32.const 0 i32.const 0 i64.const 1 call $set i32.const -4 i32.ne if unreachable end
          i32.const 1 i32.const 1024 i32.const 280 call $read drop
          i32.const 1032 i64.load i64.const 2 i64.ne if unreachable end
          i32.const 1044 i32.load i32.const 4 i32.ne if unreachable end
          i32.const 1048 i32.load i32.const 0x7478654e i32.ne if unreachable end
          i32.const 1400 i32.const 24 call $numeric i32.const 1 i32.ne if unreachable end
          i32.const 1404 i32.load i32.const 4 i32.ne if unreachable end
          i32.const 1408 f64.load f64.const 0 f64.ne if unreachable end)''',name='first')
        self.text_settings_package('''(func (export "crml_init")
          i32.const 0 i32.const 584 call $define drop
          i32.const 1 i32.const 1024 i32.const 280 call $read drop
          i32.const 1044 i32.load i32.const 5 i32.ne if unreachable end
          i32.const 1032 i64.load i64.const 1 i64.ne if unreachable end)''',name='second')
        self.assertIn('Active: 2; failures: 0',self.run_host())

    def test_text_settings_permission_memory_and_budget(self):
        self.text_settings_package('(func (export "crml_init"))',caps='log')
        self.assertIn('unknown import',self.run_host(1))
        cases=[
          ('i32.const 0 i32.const 583 call $define drop','Invalid text setting definition size'),
          ('i32.const -1 i32.const 584 call $define drop','outside guest memory'),
          ('i32.const 1 i32.const 1024 i32.const 279 call $read drop','Invalid text setting output size'),
          ('i32.const 1 i32.const 65535 i32.const 280 call $read drop','outside guest memory'),
          ('i32.const 1 i32.const 0 i32.const 256 i64.const 0 call $set drop','Text setting length exceeds limit'),
          ('i32.const 1 i32.const -1 i32.const 1 i64.const 0 call $set drop','outside guest memory'),
          ('i32.const 1 i32.const -1 i32.const 0 i64.const 0 call $set drop','outside guest memory'),
          ('i32.const 1 i32.const 1024 i32.const 280 call $read drop '*65,'Settings call budget exceeded')]
        for i,(call,error) in enumerate(cases):
            self.mods=self.root/f'text-bounds-{i}';self.mods.mkdir()
            self.text_settings_package(f'(func (export "crml_init") {call})')
            self.assertIn(error,self.run_host(1))

    def test_text_settings_invalid_payload_and_failed_read_preservation(self):
        self.text_settings_package('''(data (i32.const 800) "\\ff\\00") (func (export "crml_init")
          i32.const 0 i32.const 584 call $define drop
          i32.const 1 i32.const 800 i32.const 1 i64.const 0 call $set i32.const -3 i32.ne if unreachable end
          i32.const 1 i32.const 800 i32.const 2 i64.const 0 call $set i32.const -3 i32.ne if unreachable end
          i32.const 1 i32.const 800 i32.const 17 i64.const 0 call $set i32.const -3 i32.ne if unreachable end
          i32.const 1024 i32.const 0x12345678 i32.store
          i32.const 2 i32.const 1024 i32.const 280 call $read i32.const -2 i32.ne if unreachable end
          i32.const 1024 i32.load i32.const 0x12345678 i32.ne if unreachable end
          i32.const 1 i32.const 65536 i32.const 0 i64.const 1 call $set i32.const 1 i32.ne if unreachable end
          i32.const 1 i32.const 1024 i32.const 280 call $read drop
          i32.const 1044 i32.load i32.const 0 i32.ne if unreachable end
          i32.const 1048 i32.load8_u i32.const 0 i32.ne if unreachable end)''')
        self.assertIn('failures: 0',self.run_host())
        self.mods=self.root/'text-invalid-definition';self.mods.mkdir()
        self.text_settings_package('''(func (export "crml_init")
          i32.const 0 i32.const 584 call $define i32.const -3 i32.ne if unreachable end)''',payload=bytes(584))
        self.assertIn('failures: 0',self.run_host())

    def storage_package(self, body, name='test', caps='storage', memory='(memory (export "memory") 1)'):
        return self.package(f'''(module
          (import "crml_v1" "storage_read" (func $read (param i32 i32) (result i32)))
          (import "crml_v1" "storage_write" (func $write (param i32 i32) (result i32)))
          (import "crml_v1" "storage_status" (func $status (result i32)))
          (import "crml_v1" "capabilities" (func $caps (result i32)))
          {BASE} {memory} {body})''', name=name,
          manifest=f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\n')

    def test_storage_process_restart_and_private_namespace(self):
        folder = self.storage_package('''(data (i32.const 0) "a\\00bc")
          (func (export "crml_init")
            call $caps i32.const 32768 i32.ne if unreachable end
            i32.const 32 i32.const 4 call $read i32.const -2 i32.ne if unreachable end
            i32.const 0 i32.const 4 call $write if unreachable end
            i32.const 0 i32.const 4 call $write i32.const -4 i32.ne if unreachable end)''')
        self.assertIn('Active: 1; failures: 0', self.run_host())
        # A distinct host process must read precisely the accepted bytes.
        (folder/'mod.ini').unlink(); (folder/'mod.wasm').unlink(); folder.rmdir()
        self.storage_package('''(func (export "crml_init")
            i32.const 32 i32.const 3 call $read i32.const -3 i32.ne if unreachable end
            i32.const 32 i32.const 4 call $read i32.const 4 i32.ne if unreachable end
            i32.const 32 i32.load i32.const 0x63620061 i32.ne if unreachable end
            call $status if unreachable end)''')
        self.storage_package('''(func (export "crml_init")
            i32.const 0 i32.const 4 call $read i32.const -2 i32.ne if unreachable end)''', name='other')
        self.assertIn('Active: 2; failures: 0', self.run_host())

    def test_storage_permissions_and_unavailable_provider(self):
        self.storage_package('(func (export "crml_init"))', caps='log')
        self.assertIn('unknown import', self.run_host(1))
        self.mods=self.root/'no-service'; self.mods.mkdir()
        self.storage_package('''(func (export "crml_init")
            call $caps if unreachable end
            i32.const 0 i32.const 1 call $read i32.const -1 i32.ne if unreachable end
            i32.const 0 i32.const 1 call $write i32.const -1 i32.ne if unreachable end
            call $status i32.const -1 i32.ne if unreachable end)''')
        # Gameplay-only test host deliberately supplies no storage service.
        self.assertIn('failures: 0', self.run_gameplay())

    def test_storage_equivalent_directory_spellings(self):
        self.storage_package('''(func (export "crml_init")
            call $caps i32.const 32768 i32.ne if unreachable end)''')
        spellings=[str(self.mods),str(self.mods)+'/',str(self.mods)+'\\',str(self.mods)+'/../mods/.']
        for spelling in spellings:
            result=subprocess.run([str(BIN/'crml_host.exe'),spelling,'0'],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('Active: 1; failures: 0',result.stdout)
            self.assertFalse((self.mods/'data').exists())
            self.assertTrue((self.root/'data/storage.lock').exists())

    def test_storage_invalid_memory_and_size(self):
        cases=[(-1,1,'outside guest memory'),(65536,1,'outside guest memory'),
               (0,65537,'exceeds 65536'),(0,-1,'exceeds 65536')]
        for i,(offset,size,message) in enumerate(cases):
            self.mods=self.root/f'bad-storage-{i}'; self.mods.mkdir()
            self.storage_package(f'(func (export "crml_init") i32.const {offset} i32.const {size} call $write drop)')
            self.assertIn(message,self.run_host(1))
        self.mods=self.root/'no-memory'; self.mods.mkdir()
        self.storage_package('(func (export "crml_init") i32.const 0 i32.const 0 call $read drop)', memory='')
        self.assertIn('Missing guest memory',self.run_host(1))

    def test_storage_rejects_junction_root_and_ancestor(self):
        target=self.root/'physical'; target.mkdir()
        self.storage_package('''(func (export "crml_init")
            call $caps if unreachable end
            call $status i32.const -1 i32.ne if unreachable end)''')
        # Directory junctions need no symbolic-link privilege on Windows.
        link=self.root/'data'
        created=subprocess.run(['cmd.exe','/d','/c','mklink','/J',str(link),str(target)],capture_output=True,text=True)
        self.assertEqual(created.returncode,0,created.stdout+created.stderr)
        try:
            self.assertIn('Active: 1; failures: 0',self.run_host())
            self.assertFalse((target/'storage.lock').exists())
        finally:
            link.rmdir()  # Removes the junction itself, never its target tree.
        alias=self.root/'alias'
        created=subprocess.run(['cmd.exe','/d','/c','mklink','/J',str(alias),str(self.root)],capture_output=True,text=True)
        self.assertEqual(created.returncode,0,created.stdout+created.stderr)
        try:
            result=subprocess.run([str(BIN/'crml_host.exe'),str(alias/'mods'),'0'],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('Active: 1; failures: 0',result.stdout)
            self.assertFalse((self.root/'data').exists())
        finally:
            alias.rmdir()

    def test_storage_budget_is_separate_and_resets(self):
        calls='call $status drop '*16
        self.storage_package(f'''(func (export "crml_init") {calls} call $caps drop)
            (func (export "crml_tick") (param f32) {calls})''')
        self.assertIn('failures: 0',self.run_host())
        self.mods=self.root/'excess-storage'; self.mods.mkdir()
        self.storage_package(f'(func (export "crml_init") {calls} call $status drop)')
        self.assertIn('Storage call budget exceeded',self.run_host(1))

    def test_storage_accepted_write_survives_guest_trap(self):
        self.storage_package('''(data (i32.const 0) "kept")
            (func (export "crml_init") i32.const 0 i32.const 4 call $write drop unreachable)''')
        self.assertIn('unreachable',self.run_host(1))
        payload=(self.root/'data/mod-test.bin').read_bytes()
        self.assertEqual(payload[16:], b'kept')

    def test_storage_corruption_is_reported_without_overwrite(self):
        (self.root/'data').mkdir(); (self.root/'data/mod-test.bin').write_bytes(b'broken')
        self.storage_package('''(data (i32.const 0) "sentinel")
            (func (export "crml_init")
              i32.const 0 i32.const 8 call $read i32.const -5 i32.ne if unreachable end
              i32.const 0 i32.load8_u i32.const 115 i32.ne if unreachable end)''')
        self.assertIn('failures: 0',self.run_host())
        self.assertEqual((self.root/'data/mod-test.bin').read_bytes(),b'broken')

    def test_runtime_version_and_manifest_minimum(self):
        version = subprocess.check_output([str(BIN/'crml_host.exe'), '--version'], text=True).strip()
        self.assertEqual(version, (Path(__file__).resolve().parents[1]/'VERSION').read_text().strip())
        for i, required in enumerate(['0.0.0', version, version+'+build.001']):
            self.mods = self.root/f'compatible-{i}'; self.mods.mkdir()
            self.package(f'(module {BASE} (func (export "crml_init")))',
                         manifest=f'id=test\nabi=1\nmodule=mod.wasm\nmin_runtime={required}\n')
            self.assertIn('Active: 1; failures: 0', self.run_host())

    def test_feedback_permission_memory_and_budget(self):
        imports = '(import "crml_v1" "feedback_show" (func $show (param i32 i32 i32 i32) (result i64)))'
        cases = [
            ('', '(memory (export "memory") 1)', '', 'unknown import'),
            ('feedback', '', 'i32.const 0 i32.const 1 i32.const 0 i32.const 1000 call $show drop', 'Missing guest memory'),
            ('feedback', '(memory (export "memory") 1)', 'i32.const 65536 i32.const 1 i32.const 0 i32.const 1000 call $show drop', 'Feedback buffer outside'),
            ('feedback', '(memory (export "memory") 1)', 'i32.const 0 i32.const 241 i32.const 0 i32.const 1000 call $show drop', 'Invalid feedback text size'),
        ]
        for i, (caps, memory, code, error) in enumerate(cases):
            self.mods=self.root/f'feedback-bad-{i}';self.mods.mkdir()
            self.package(f'(module {imports} {memory} {BASE} (func (export "crml_init") {code}))',
                manifest=f'id=test\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\n')
            self.assertIn(error,self.run_host(1))
        self.mods=self.root/'feedback-budget';self.mods.mkdir()
        query='i64.const 1 call $status drop '
        self.package(f'''(module (import "crml_v1" "feedback_status" (func $status (param i64) (result i32)))
            {BASE} (func (export "crml_init") {query*17}))''',
            manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=feedback\n')
        self.assertIn('Feedback call budget exceeded',self.run_host(1))

    def test_feedback_standalone_unavailable_and_budget_reset(self):
        check='i64.const 1 call $status i32.const -2 i32.ne if unreachable end '
        self.package(f'''(module
            (import "crml_v1" "feedback_show" (func $show (param i32 i32 i32 i32) (result i64)))
            (import "crml_v1" "feedback_status" (func $status (param i64) (result i32)))
            (import "crml_v1" "capabilities" (func $caps (result i32)))
            (memory (export "memory") 1) (data (i32.const 0) "text") {BASE}
            (func (export "crml_init")
                call $caps i32.const 131072 i32.and if unreachable end
                i32.const 0 i32.const 4 i32.const 0 i32.const 1000 call $show i64.const -1 i64.ne if unreachable end)
            (func (export "crml_tick") (param f32) {check*16}))''',
            manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=feedback\n')
        self.assertIn('failures: 0',self.run_host())

    def test_newer_runtime_requirement_precedes_module_execution(self):
        self.package(f'(module {BASE} (func $start unreachable) (start $start) (func (export "crml_init")))',
                     manifest='id=test\nabi=1\nmodule=mod.wasm\nmin_runtime=999.0.0\ncapabilities=player.motion\n')
        output = self.run_host(1)
        self.assertIn('Requires CRML 999.0.0; installed ', output)
        self.assertIn('Control-Resonant-Mod-Loader/releases', output)
        self.assertNotIn('unreachable', output)
        self.assertIn('Prepared capabilities: 0', self.run_gameplay())
        # Even an absent binary cannot hide the more useful version rejection.
        (self.mods/'test/mod.wasm').unlink()
        self.assertIn('Requires CRML 999.0.0; installed ', self.run_host(1))

    def test_package_metadata_utf8_and_bounds(self):
        base = 'id=test\nabi=1\nmodule=mod.wasm\n'
        self.package(f'(module {BASE} (func (export "crml_init")))',
                     manifest=base+'name=Mod é\nversion=1.2.3-beta.1+build.5\nauthor=An author\n')
        output = self.run_host()
        self.assertIn('Package test: name=Mod é; version=1.2.3-beta.1+build.5; author=An author', output)
        invalid = [b'name=\xff', b'name=bad\x00label', b'author=bad\tlabel',
                   b'name='+b'x'*96, b'author='+b'x'*96, b'version=v1.0.0',
                   b'version=01.0.0', b'version=1.0', b'name=', b'version=', b'author=']
        for i, fields in enumerate(invalid):
            self.mods = self.root/f'metadata-{i}'; self.mods.mkdir()
            package = self.package(f'(module {BASE} (func $start unreachable) (start $start) (func (export "crml_init")))')
            (package/'mod.ini').write_bytes(base.encode()+fields+b'\n')
            output = self.run_host(1)
            self.assertIn('manifest metadata', output)
            self.assertNotIn('unreachable', output)

    def test_invalid_runtime_requirement(self):
        for i, version in enumerate(['', 'v1.0.0', '1.0', '01.0.0', '1.0.0-alpha.01', '1.0.0+', '9'*97]):
            self.mods = self.root/f'invalid-version-{i}'; self.mods.mkdir()
            self.package(f'(module {BASE} (func (export "crml_init")))',
                         manifest=f'id=test\nabi=1\nmodule=mod.wasm\nmin_runtime={version}\n')
            self.assertIn('min_runtime must be a semantic version', self.run_host(1))

    def snapshot_package(self, kind, body, caps=None, memory='(memory (export "memory") 1)', extra=''):
        capability={'navigation':'navigation.read','navigation_v2':'navigation.read','player':'player.read','camera':'camera.read','physics':'physics.damping','ui':'ui.read','media':'media.read','visibility':'player.visibility'}[kind]
        signature='i64 i32 i32' if kind=='physics' else 'i32 i32'
        import_name='navigation_read_v2' if kind=='navigation_v2' else f'{kind}_read'
        self.package(f'''(module
          (import "crml_v1" "{import_name}" (func $read (param {signature}) (result i32)))
          {extra} {BASE} {memory} (func (export "crml_init") {body}))''',
          manifest=f'id=test\nabi=1\nmodule=mod.wasm\ncapabilities={capability if caps is None else caps}\n')

    def ui_present_package(self, body, caps='ui.presentation', memory='(memory (export "memory") 1)', data='(data (i32.const 0) "splash")', name='test'):
        self.package(f'''(module
          (import "crml_v1" "ui_present" (func $present (param i64 i32 i32 i32 i32 i32) (result i32)))
          {BASE} {memory} {data} {body})''', name=name,
          manifest=f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\n')

    def test_ui_presentation_routes_exact_name_full_generation_private_owner(self):
        for name in ['first','second']:
            self.ui_present_package('''(func (export "crml_init")
                i64.const 0xf123456789abcdef i32.const 2 i32.const 0 i32.const 6 i32.const 1 i32.const 750
                call $present if unreachable end)''',name=name)
        output=self.run_gameplay()
        rows=re.findall(r'UI present: owner ([0-9]+) generation ([0-9]+) kind 2 name splash hidden 1 duration 750',output)
        self.assertEqual(len(rows),2,output)
        self.assertEqual(len({r[0] for r in rows}),2)
        self.assertTrue(all(int(r[1])==0xf123456789abcdef for r in rows))
        self.assertIn('Gameplay calls: 2; failures: 0; owners: 0',output)

    def test_ui_presentation_permission_and_no_native_service(self):
        self.ui_present_package('(func (export "crml_init"))',caps='ui.read,ui.activate')
        self.assertIn('unknown import',self.run_host(1))
        self.mods=self.root/'allowed-presentation';self.mods.mkdir()
        self.ui_present_package('''(func (export "crml_init")
            i64.const 1 i32.const 1 i32.const 0 i32.const 6 i32.const 0 i32.const 0
            call $present i32.const -1 i32.ne if unreachable end)''')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_ui_presentation_capability_bit(self):
        self.package(f'''(module (import "crml_v1" "capabilities" (func $caps (result i32))) {BASE}
          (func (export "crml_init") call $caps i32.const 16384 i32.ne if unreachable end))''',
          manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=ui.presentation\n')
        self.assertIn('failures: 0; owners: 0',self.run_gameplay())

    def test_ui_presentation_rejects_arguments_before_provider(self):
        valid=[0xf123456789abcdef,2,0,6,1,750]
        for i,(slot,value) in enumerate([(0,0),(1,0),(1,3),(3,0),(3,65),(3,-1),(4,2),(4,-1),(5,0),(5,1001),(5,-1)]):
            self.mods=self.root/f'bad-present-{i}';self.mods.mkdir()
            args=valid.copy();args[slot]=value
            code=' '.join(f'{"i64" if j==0 else "i32"}.const {v}' for j,v in enumerate(args))
            self.ui_present_package(f'(func (export "crml_init") {code} call $present drop)')
            output=self.run_gameplay()
            self.assertIn('Invalid UI presentation arguments',output)
            self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',output)
        self.mods=self.root/'bad-release-duration';self.mods.mkdir()
        self.ui_present_package('(func (export "crml_init") i64.const 1 i32.const 2 i32.const 0 i32.const 6 i32.const 0 i32.const 1 call $present drop)')
        self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',self.run_gameplay())

    def test_ui_presentation_name_bounds_and_grammar(self):
        for i,(offset,length,memory,data,error) in enumerate([
            (65531,6,'(memory (export "memory") 1)','', 'outside guest memory'),
            (-1,6,'(memory (export "memory") 1)','', 'outside guest memory'),
            (0,6,'','', 'Missing guest memory'),
            (0,6,'(global (export "memory") i32 (i32.const 0))','', 'Invalid guest memory'),
            (0,6,'(memory (export "memory") 1)','(data (i32.const 0) "spl sh")', 'Invalid UI presentation name'),
            (0,6,'(memory (export "memory") 1)','(data (i32.const 0) "sp\\00ash")', 'Invalid UI presentation name'),
            (0,6,'(memory (export "memory") 1)','(data (i32.const 0) "#splat")', 'Invalid UI presentation name'),
            (0,6,'(memory (export "memory") 1)','(data (i32.const 0) "splas\\ff")', 'Invalid UI presentation name')]):
            self.mods=self.root/f'present-name-{i}';self.mods.mkdir()
            self.ui_present_package(f'(func (export "crml_init") i64.const 1 i32.const 2 i32.const {offset} i32.const {length} i32.const 1 i32.const 1 call $present drop)',memory=memory,data=data)
            output=self.run_gameplay()
            self.assertIn(error,output)
            self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',output)
        self.mods=self.root/'present-last-byte';self.mods.mkdir()
        self.ui_present_package('''(func (export "crml_init")
            i64.const 0xf123456789abcdef i32.const 1 i32.const 65472 i32.const 64 i32.const 1 i32.const 1000
            call $present if unreachable end)''',data='(data (i32.const 65472) "'+('Aa0_-'*12+'Aa0_')+'")')
        self.assertIn('Gameplay calls: 1; failures: 0; owners: 0',self.run_gameplay())

    def test_ui_presentation_budget_and_trap_cleanup(self):
        self.ui_present_package('''(func (export "crml_init") (loop
            i64.const 0xf123456789abcdef i32.const 2 i32.const 0 i32.const 6 i32.const 1 i32.const 750
            call $present drop br 0))''')
        output=self.run_gameplay()
        self.assertIn('Gameplay call budget exceeded',output)
        self.assertIn('Gameplay calls: 8; failures: 1; owners: 0',output)

    def presentation_example_rows(self, mode):
        result=subprocess.run([str(BIN/'crml_gameplay_tests.exe'),str(self.mods),mode],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('failures: 0; owners: 0',result.stdout)
        return [tuple(map(int,r)) for r in re.findall(r'UI present: owner \d+ generation (\d+) kind 2 name splash hidden (\d+) duration (\d+) screen (\d+) tick (\d+)',result.stdout)]

    def test_startup_presentation_only_eligible_screens_and_without_actions(self):
        self.startup_example()
        for screen in range(10):
            rows=self.presentation_example_rows(f'ui-screen-{screen}')
            self.assertEqual(len(rows),64 if screen in [1,5,8] else 0)
            self.assertTrue(all(r[1]==1 and r[2]==750 and r[3]==screen for r in rows))
        self.assertEqual(len(self.presentation_example_rows('ui-no-actions')),64)

    def test_startup_presentation_releases_on_other_screens_and_retries_failure(self):
        self.startup_example()
        rows=self.presentation_example_rows('ui-changes')
        self.assertEqual([r[4] for r in rows if r[1]==1],list(range(24)))
        self.assertEqual([(r[1],r[2],r[3],r[4]) for r in rows if not r[1]],[(0,0,2,24)])
        rows=self.presentation_example_rows('ui-present-release-rejected')
        self.assertEqual([r[4] for r in rows if not r[1]],[8,9,10])

    def test_ui_snapshot_layout_last_bytes_and_failed_provider_zeroing(self):
        self.snapshot_package('ui','''
          i32.const 65504 i32.const 32 call $read i32.const 1 i32.ne if unreachable end
          i32.const 65504 i32.load i32.const 32 i32.ne if unreachable end
          i32.const 65508 i32.load i32.const 1 i32.ne if unreachable end
          i32.const 65512 i32.load i32.const 1 i32.ne if unreachable end
          i32.const 65516 i32.load i32.const 1 i32.ne if unreachable end
          i32.const 65520 i64.load i64.const 0xf123456789abcdef i64.ne if unreachable end
          i32.const 65528 i32.load i32.const 12 i32.ne if unreachable end
          i32.const 65532 i32.load if unreachable end
          i32.const 65504 i32.const 32 call $read if unreachable end
          i32.const 65504 i64.load i32.const 65512 i64.load i64.or
          i32.const 65520 i64.load i64.or i32.const 65528 i64.load i64.or
          i64.eqz i32.eqz if unreachable end''')
        self.assertIn('Gameplay calls: 2; failures: 0; owners: 0',self.run_gameplay())

    def test_ui_unavailable_zeroes_unaligned_output(self):
        self.snapshot_package('ui','''
          i32.const 1 i64.const -1 i64.store
          i32.const 9 i64.const -1 i64.store
          i32.const 17 i64.const -1 i64.store
          i32.const 25 i64.const -1 i64.store
          i32.const 1 i32.const 32 call $read i32.const -1 i32.ne if unreachable end
          i32.const 1 i64.load i32.const 9 i64.load i64.or
          i32.const 17 i64.load i64.or i32.const 25 i64.load i64.or
          i64.eqz i32.eqz if unreachable end''')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_ui_snapshot_checks_bounds_before_native_access(self):
        for i,(offset,size,memory,error) in enumerate([
            (0,31,'(memory (export "memory") 1)','size does not match'),
            (0,33,'(memory (export "memory") 1)','size does not match'),
            (65505,32,'(memory (export "memory") 1)','outside guest memory'),
            (-1,32,'(memory (export "memory") 1)','outside guest memory'),
            (0,32,'','Missing guest memory'),
            (0,32,'(global (export "memory") i32 (i32.const 0))','Invalid guest memory')]):
            with self.subTest(offset=offset,size=size,memory=memory):
                self.mods=self.root/f'ui-bounds-{i}';self.mods.mkdir()
                self.snapshot_package('ui',f'i32.const {offset} i32.const {size} call $read drop',memory=memory)
                output=self.run_gameplay()
                self.assertIn(error,output)
                self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',output)

    def ui_action_package(self, body, caps='ui.activate', extra='', name='test'):
        self.package(f'''(module
          (import "crml_v1" "ui_activate" (func $activate (param i64 i32) (result i32)))
          {extra} {BASE} {body})''',name=name,
          manifest=f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\n')

    def test_ui_permissions_are_separate(self):
        self.snapshot_package('ui','',caps='ui.activate')
        self.assertIn('unknown import',self.run_host(1))
        self.mods=self.root/'denied-action';self.mods.mkdir()
        self.ui_action_package('(func (export "crml_init"))',caps='ui.read')
        self.assertIn('unknown import',self.run_host(1))

    def ui_receipt_package(self, body, caps='ui.activate', name='test'):
        self.package(f'''(module
          (import "crml_v1" "ui_action_submit" (func $submit (param i64 i32) (result i64)))
          (import "crml_v1" "ui_action_status" (func $status (param i64) (result i32)))
          (import "crml_v1" "release" (func $release))
          {BASE} {body})''',name=name,
          manifest=f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\nmin_runtime=0.1.0-alpha.4.3.dev.0\n')

    def test_ui_receipts_real_provider_owner_and_cleanup(self):
        self.ui_receipt_package('''(func (export "crml_init") (local $ticket i64)
          i64.const 1 i32.const 1 call $submit local.tee $ticket i64.const 1 i64.ne if unreachable end
          local.get $ticket call $status i32.const 1 i32.ne if unreachable end
          i64.const 1 i32.const 1 call $submit i64.const -2 i64.ne if unreachable end
          i64.const 0x100000001 call $status i32.const -2 i32.ne if unreachable end
          call $release
          local.get $ticket call $status i32.const 8 i32.ne if unreachable end)''',name='a-owner')
        self.ui_receipt_package('''(func (export "crml_init")
          i64.const 1 call $status i32.const -2 i32.ne if unreachable end
          i64.const 0x100000001 i32.const 1 call $submit i64.const -3 i64.ne if unreachable end
          i64.const 1 i32.const 1 call $submit i64.const 2 i64.ne if unreachable end
          i64.const 2 call $status i32.const 1 i32.ne if unreachable end)''',name='b-other')
        self.assertIn('failures: 0; owners: 0',self.run_gameplay('ui-receipts'))

    def test_ui_receipts_unavailable_and_permission(self):
        self.ui_receipt_package('''(func (export "crml_init")
          i64.const 1 i32.const 1 call $submit i64.const -1 i64.ne if unreachable end
          i64.const 1 call $status i32.const -1 i32.ne if unreachable end)''')
        self.assertIn('Active: 1; failures: 0',self.run_host())
        self.mods=self.root/'receipt-permission';self.mods.mkdir()
        self.ui_receipt_package('(func (export "crml_init"))',caps='ui.read')
        self.assertIn('unknown import',self.run_host(1))

    def test_ui_receipts_argument_and_budget_failures(self):
        for i,(body,error) in enumerate([
            ('i64.const 0 i32.const 1 call $submit drop','Invalid UI action arguments'),
            ('i64.const 1 i32.const 2 call $submit drop','Invalid UI action arguments'),
            ('(loop i64.const 1 call $status drop br 0)','Observation call budget exceeded'),
            ('(loop i64.const 1 i32.const 1 call $submit drop br 0)','Gameplay call budget exceeded')]):
            self.mods=self.root/f'receipt-invalid-{i}';self.mods.mkdir()
            self.ui_receipt_package(f'(func (export "crml_init") {body})')
            self.assertIn(error,self.run_gameplay('ui-receipts'))

    def test_ui_unknown_capability_is_not_a_wildcard(self):
        self.package(f'(module {BASE} (func (export "crml_init")))',
            manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=ui.*\n')
        self.assertIn('Unsupported capability',self.run_host(1))

    def test_ui_capabilities_report_availability_and_declaration(self):
        self.package(f'''(module
          (import "crml_v1" "capabilities" (func $caps (result i32))) {BASE}
          (func (export "crml_init") call $caps i32.const 3072 i32.ne if unreachable end))''',
          manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=ui.read,ui.activate\n')
        self.assertIn('failures: 0; owners: 0',self.run_gameplay())
        self.mods=self.root/'unavailable-ui-caps';self.mods.mkdir()
        self.package(f'''(module
          (import "crml_v1" "capabilities" (func $caps (result i32))) {BASE}
          (func (export "crml_init") call $caps if unreachable end))''',
          manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=ui.read,ui.activate\n')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_ui_activate_routes_full_generation_and_private_owner(self):
        for name in ['first','second']:
            self.ui_action_package('''(func (export "crml_init")
              i64.const 0xf123456789abcdef i32.const 1 call $activate if unreachable end
              i64.const 42 i32.const 1 call $activate i32.const -3 i32.ne if unreachable end)''',name=name)
        output=self.run_gameplay()
        self.assertIn('Gameplay calls: 4; failures: 0; owners: 0',output)
        for owner in [1,2]:
            self.assertIn(f'UI activate: owner {owner} generation {0xf123456789abcdef} action 1',output)

    def test_ui_activate_invalid_arguments_never_reach_native(self):
        for i,(generation,action) in enumerate([(0,1),(1,0),(1,2),(1,-1)]):
            with self.subTest(generation=generation,action=action):
                self.mods=self.root/f'ui-action-{i}';self.mods.mkdir()
                self.ui_action_package(f'''(func (export "crml_init")
                  i64.const {generation} i32.const {action} call $activate drop)''')
                output=self.run_gameplay()
                self.assertIn('Invalid UI action arguments',output)
                self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',output)

    def test_ui_activate_without_native_service(self):
        self.ui_action_package('''(func (export "crml_init")
          i64.const 1 i32.const 1 call $activate i32.const -1 i32.ne if unreachable end)''')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_ui_observation_budget_and_trap_cleanup(self):
        self.ui_action_package('''(memory (export "memory") 1)
          (func (export "crml_init") (loop
            i32.const 0 i32.const 32 call $read drop call $caps drop
            i64.const 0xf123456789abcdef i32.const 1 call $activate drop br 0))''',
          caps='ui.read,ui.activate',extra='''
          (import "crml_v1" "ui_read" (func $read (param i32 i32) (result i32)))
          (import "crml_v1" "capabilities" (func $caps (result i32)))''')
        output=self.run_gameplay()
        self.assertIn('Observation call budget exceeded',output)
        self.assertIn('Gameplay calls: 8; failures: 1; owners: 0',output)

    def startup_example(self):
        example=Path(__file__).resolve().parents[1]/'examples/startup-skip'
        self.package((example/'startup-skip.wat').read_text(),
            manifest=(example/'mod.ini').read_text().replace('startup-skip.wasm','mod.wasm'))

    def startup_run(self, mode):
        result=subprocess.run([str(BIN/'crml_gameplay_tests.exe'),str(self.mods),mode],
            capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('failures: 0; owners: 0',result.stdout)
        return [tuple(map(int,match)) for match in re.findall(
            r'UI activate: owner (\d+) generation (\d+) action (\d+) screen (\d+) tick (\d+)',result.stdout)]

    def test_startup_skip_handles_unavailable_runtime(self):
        self.startup_example()
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_startup_skip_only_allowed_screens_and_bounded_retries(self):
        self.startup_example()
        for screen in range(10):
            with self.subTest(screen=screen):
                requests=self.startup_run(f'ui-screen-{screen}')
                self.assertEqual(len(requests),10 if screen in [1,5,8] else 0)
                if requests:
                    self.assertEqual([row[4] for row in requests],list(range(1,11)))
                    self.assertTrue(all(row[2]==1 and row[3]==screen for row in requests))

    def test_startup_skip_resets_on_screen_or_generation_change(self):
        self.startup_example()
        for mode in ['ui-changes','ui-generation','ui-screen-change']:
            with self.subTest(mode=mode):
                requests=self.startup_run(mode)
                self.assertEqual([row[4] for row in requests],[base+i for base in [0,8,16] for i in range(1,8)])

    def test_startup_skip_requires_action_valid_layout_and_stable_generation(self):
        self.startup_example()
        for mode in ['ui-no-actions','ui-malformed','ui-short']:
            with self.subTest(mode=mode):
                self.assertEqual(self.startup_run(mode),[])

    def test_startup_skip_unavailable_intervals_do_not_refill_attempt_budget(self):
        self.startup_example()
        requests=self.startup_run('ui-transient')
        self.assertEqual([row[4] for row in requests],[2,3,4,5,6,7,8,11,12,13])
        self.assertEqual(len(self.startup_run('ui-rejected')),10)

    def test_media_snapshot_layout_name_and_full_failed_output_zeroing(self):
        self.snapshot_package('media','''(local $offset i32) (local $bits i64)
          i32.const 65248 i32.const 288 call $read i32.const 1 i32.ne if unreachable end
          i32.const 65248 i32.load i32.const 288 i32.ne if unreachable end
          i32.const 65252 i32.load i32.const 1 i32.ne if unreachable end
          i32.const 65256 i32.load i32.const 7 i32.ne if unreachable end
          i32.const 65260 i32.load i32.const 2501 i32.ne if unreachable end
          i32.const 65264 i64.load i64.const 0xe123456789abcdef i64.ne if unreachable end
          i32.const 65272 i32.load i32.const 43 i32.ne if unreachable end
          i32.const 65280 i32.load8_u i32.const 116 i32.ne if unreachable end
          i32.const 65323 i32.load8_u if unreachable end
          i32.const 65535 i32.load8_u if unreachable end
          i32.const 65248 i32.const 288 call $read i32.const -1 i32.ne if unreachable end
          (loop $zero
            i32.const 65248 local.get $offset i32.add i64.load
            local.get $bits i64.or local.set $bits
            local.get $offset i32.const 8 i32.add local.tee $offset
            i32.const 288 i32.lt_u br_if $zero)
          local.get $bits i64.eqz i32.eqz if unreachable end''')
        self.assertIn('Gameplay calls: 2; failures: 0; owners: 0',self.run_gameplay())

    def test_media_unavailable_zeroes_unaligned_output(self):
        self.snapshot_package('media','''(local $offset i32) (local $bits i64)
          i32.const 1 i32.const 255 i32.const 288 memory.fill
          i32.const 1 i32.const 288 call $read i32.const -1 i32.ne if unreachable end
          (loop $zero i32.const 1 local.get $offset i32.add i64.load
            local.get $bits i64.or local.set $bits
            local.get $offset i32.const 8 i32.add local.tee $offset
            i32.const 288 i32.lt_u br_if $zero)
          local.get $bits i64.eqz i32.eqz if unreachable end''')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_media_snapshot_validates_before_native_call(self):
        for i,(offset,size,memory,error) in enumerate([
            (0,287,'(memory (export "memory") 1)','size does not match'),
            (0,289,'(memory (export "memory") 1)','size does not match'),
            (65249,288,'(memory (export "memory") 1)','outside guest memory'),
            (-1,288,'(memory (export "memory") 1)','outside guest memory'),
            (0,288,'','Missing guest memory'),
            (0,288,'(global (export "memory") i32 (i32.const 0))','Invalid guest memory')]):
            with self.subTest(offset=offset,size=size,memory=memory):
                self.mods=self.root/f'media-bounds-{i}';self.mods.mkdir()
                self.snapshot_package('media',f'i32.const {offset} i32.const {size} call $read drop',memory=memory)
                output=self.run_gameplay()
                self.assertIn(error,output)
                self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',output)

    def media_skip_package(self, body, caps='media.skip', extra='', name='test'):
        self.package(f'''(module
          (import "crml_v1" "media_skip" (func $skip (param i64) (result i32)))
          {extra} {BASE} {body})''',name=name,
          manifest=f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\n')

    def test_media_permissions_do_not_imply_ui_or_each_other(self):
        for i,(kind,caps) in enumerate([('media','media.skip'),('media','ui.read'),('ui','media.read')]):
            self.mods=self.root/f'media-denied-{i}';self.mods.mkdir()
            self.snapshot_package(kind,'',caps=caps)
            self.assertIn('unknown import',self.run_host(1))
        self.mods=self.root/'media-denied-skip';self.mods.mkdir()
        self.media_skip_package('(func (export "crml_init"))',caps='media.read')
        self.assertIn('unknown import',self.run_host(1))

    def test_media_capabilities_available_and_unknown_rejected(self):
        self.package(f'''(module
          (import "crml_v1" "capabilities" (func $caps (result i32))) {BASE}
          (func (export "crml_init") call $caps i32.const 12288 i32.ne if unreachable end))''',
          manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=media.read,media.skip\n')
        self.assertIn('failures: 0; owners: 0',self.run_gameplay())
        self.mods=self.root/'media-unknown';self.mods.mkdir()
        self.package(f'(module {BASE} (func (export "crml_init")))',
            manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=media.*\n')
        self.assertIn('Unsupported capability',self.run_host(1))

    def test_media_skip_routes_generation_and_owner_then_releases(self):
        for name in ['first','second']:
            self.media_skip_package('''(func (export "crml_init")
              i64.const 0xe123456789abcdef call $skip if unreachable end
              i64.const 42 call $skip i32.const -1 i32.ne if unreachable end)''',name=name)
        output=self.run_gameplay()
        self.assertIn('Gameplay calls: 4; failures: 0; owners: 0',output)
        for owner in [1,2]:
            self.assertIn(f'Media skip: owner {owner} generation {0xe123456789abcdef}',output)

    def test_media_skip_zero_traps_before_native_and_unavailable_returns(self):
        self.media_skip_package('(func (export "crml_init") i64.const 0 call $skip drop)')
        output=self.run_gameplay()
        self.assertIn('Invalid media generation',output)
        self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',output)
        self.mods=self.root/'media-unavailable';self.mods.mkdir()
        self.media_skip_package('''(func (export "crml_init")
          i64.const 1 call $skip i32.const -1 i32.ne if unreachable end)''')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_media_ui_share_observation_budget_and_trap_cleanup(self):
        self.media_skip_package('''(memory (export "memory") 1)
          (func (export "crml_init") (loop
            i32.const 0 i32.const 288 call $read drop
            i64.const 0xe123456789abcdef call $skip drop
            i32.const 512 i32.const 32 call $ui drop call $caps drop br 0))''',
          caps='media.read,media.skip,ui.read',extra='''
          (import "crml_v1" "media_read" (func $read (param i32 i32) (result i32)))
          (import "crml_v1" "ui_read" (func $ui (param i32 i32) (result i32)))
          (import "crml_v1" "capabilities" (func $caps (result i32)))''')
        output=self.run_gameplay()
        self.assertIn('Observation call budget exceeded',output)
        self.assertIn('Gameplay calls: 9; failures: 1; owners: 0',output)

    def startup_media_run(self, mode):
        result=subprocess.run([str(BIN/'crml_gameplay_tests.exe'),str(self.mods),mode],
            capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('failures: 0; owners: 0',result.stdout)
        return [tuple(map(int,match)) for match in re.findall(
            r'Media skip: owner (\d+) generation (\d+) tick (\d+)',result.stdout)]

    def test_startup_media_matches_engine_paths_and_name_provenance(self):
        self.startup_example()
        for mode in ['media-valid','media-slashes','media-mapped','media-rejected','media-both']:
            with self.subTest(mode=mode):
                requests=self.startup_media_run(mode)
                self.assertEqual([row[2] for row in requests],list(range(2,21,2)))

    def test_startup_media_never_skips_unknown_or_unqualified_assets(self):
        self.startup_example()
        for mode in ['media-unknown','media-suffix','media-nul','media-unterminated','media-length',
                     'media-unnamed','media-inactive','media-unskippable','media-young','media-malformed','media-short']:
            with self.subTest(mode=mode):
                self.assertEqual(self.startup_media_run(mode),[])

    def test_startup_media_timing_and_generation_budgets(self):
        self.startup_example()
        self.assertEqual([row[2] for row in self.startup_media_run('media-clock')],list(range(17,36,2)))
        self.assertEqual([row[2] for row in self.startup_media_run('media-generation')],
            [base+i for base in [0,16,32] for i in range(2,15,2)])
        self.assertEqual([row[2] for row in self.startup_media_run('media-transient')],
            [3,5,9,11,15,17,21,23,27,29])

    def test_navigation_snapshot_layout_and_dirty_failure_zeroing(self):
        offset=65536-56
        checks=[]
        for relative,typ,value in [(0,'i32',1),(4,'i32',12),(8,'i64','0x123456789abcdef'),
            (16,'i64','0xf123456789abcdef'),(24,'i32',3),(28,'i32',0),
            (32,'f32',1),(36,'f32',2),(40,'f32',3),(44,'f32',-1),(48,'f32',0),(52,'f32',0)]:
            checks.append(f'i32.const {offset+relative} {typ}.load {typ}.const {value} {typ}.ne if unreachable end')
        zero=' '.join(f'i32.const {offset+i} i64.load i64.eqz i32.eqz if unreachable end' for i in range(0,56,8))
        self.snapshot_package('navigation',f"""
          i32.const {offset} i32.const 56 call $read i32.const 1 i32.ne if unreachable end
          {' '.join(checks)}
          i32.const {offset} i32.const 56 call $read if unreachable end {zero}
          i32.const {offset} i32.const 56 call $read i32.const -1 i32.ne if unreachable end {zero}""")
        self.assertIn('Gameplay calls: 3; failures: 0; owners: 0',self.run_gameplay())

    def test_navigation_snapshot_absent_provider_zeroes_buffer(self):
        zero=' '.join(f'i32.const {1+i} i64.load i64.eqz i32.eqz if unreachable end' for i in range(0,56,8))
        self.snapshot_package('navigation',f"""i32.const 1 i32.const 255 i32.const 56 memory.fill
          i32.const 1 i32.const 56 call $read i32.const -1 i32.ne if unreachable end {zero}""")
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_navigation_snapshot_capability_and_shared_budget(self):
        self.snapshot_package('navigation','',caps='player.read')
        self.assertIn('unknown import',self.run_host(1))
        self.mods=self.root/'navigation-budget';self.mods.mkdir()
        self.snapshot_package('navigation',"""call $caps i32.const 524288 i32.ne if unreachable end
          (loop i32.const 1 i32.const 56 call $read drop br 0)""",
          extra='(import "crml_v1" "capabilities" (func $caps (result i32)))')
        output=self.run_gameplay()
        self.assertIn('Observation call budget exceeded',output)
        self.assertIn('Gameplay calls: 7; failures: 1; owners: 0',output)

    def test_navigation_snapshot_invalid_memory_never_calls_provider(self):
        for index,(offset,size,memory,error) in enumerate([
            (0,55,'(memory (export "memory") 1)','size does not match'),
            (0,57,'(memory (export "memory") 1)','size does not match'),
            (-1,56,'(memory (export "memory") 1)','outside guest memory'),
            (65536-55,56,'(memory (export "memory") 1)','outside guest memory'),
            (0,56,'','Missing guest memory'),
            (0,56,'(global (export "memory") i32 (i32.const 0))','Invalid guest memory')]):
            self.mods=self.root/f'navigation-invalid-{index}';self.mods.mkdir()
            self.snapshot_package('navigation',f'i32.const {offset} i32.const {size} call $read drop',memory=memory)
            output=self.run_gameplay()
            self.assertIn(error,output)
            self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',output)

    def test_navigation_v2_capability_and_unavailable_zeroing(self):
        self.snapshot_package('navigation_v2','',caps='player.read')
        self.assertIn('unknown import',self.run_host(1))
        self.mods=self.root/'navigation-v2-unavailable';self.mods.mkdir()
        zero=' '.join(f'i32.const {1+i} i64.load i64.eqz i32.eqz if unreachable end' for i in range(0,64,8))
        self.snapshot_package('navigation_v2',f'''i32.const 1 i32.const 255 i32.const 64 memory.fill
          i32.const 1 i32.const 64 call $read i32.const -1 i32.ne if unreachable end {zero}''')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_navigation_v2_invalid_memory_never_calls_provider(self):
        for index,(offset,size,error) in enumerate([
            (0,56,'size does not match'),(0,65,'size does not match'),
            (-1,64,'outside guest memory'),(65536-63,64,'outside guest memory')]):
            self.mods=self.root/f'navigation-v2-invalid-{index}';self.mods.mkdir()
            self.snapshot_package('navigation_v2',f'i32.const {offset} i32.const {size} call $read drop')
            output=self.run_gameplay()
            self.assertIn(error,output)
            self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',output)

    def test_player_snapshot_layout_and_failure_zeroing(self):
        self.snapshot_package('player','''
          i32.const 65504 i32.const 32 call $read i32.const 1 i32.ne if unreachable end
          i32.const 65504 i32.load i32.const 1 i32.ne if unreachable end
          i32.const 65508 i32.load i32.const 12 i32.ne if unreachable end
          i32.const 65512 i64.load i64.const 0x123456789abcdef i64.ne if unreachable end
          i32.const 65520 f32.load f32.const 1 f32.ne if unreachable end
          i32.const 65528 f32.load f32.const 3 f32.ne if unreachable end
          i32.const 65504 i32.const 32 call $read if unreachable end
          i32.const 65504 i64.load i32.const 65512 i64.load i64.or
          i32.const 65520 i64.load i64.or i32.const 65528 i64.load i64.or
          i64.eqz i32.eqz if unreachable end''')
        self.assertIn('Gameplay calls: 2; failures: 0; owners: 0',self.run_gameplay())

    def test_camera_snapshot_layout_and_unaligned_output(self):
        self.snapshot_package('camera','''
          i32.const 1 i32.const 80 call $read i32.const 1 i32.ne if unreachable end
          i32.const 9 i64.load i64.const 0x23456789abcdef0 i64.ne if unreachable end
          i32.const 17 i32.load i32.const 1 i32.ne if unreachable end
          i32.const 25 f32.load f32.const 4 f32.ne if unreachable end
          i32.const 69 f32.load f32.const 1 f32.ne if unreachable end
          i32.const 73 f32.load f32.const 1.25 f32.ne if unreachable end
          i32.const 77 f32.load f32.const 1.75 f32.ne if unreachable end''')
        self.assertIn('Gameplay calls: 1; failures: 0; owners: 0',self.run_gameplay())

    def test_physics_snapshot_owner_and_readback(self):
        self.snapshot_package('physics','''
          call $select if unreachable end
          call $target i32.const 0 i32.const 32 call $read i32.const 1 i32.ne if unreachable end
          i32.const 8 i32.load i32.const 3 i32.ne if unreachable end
          i32.const 16 f32.load f32.const 0.25 f32.ne if unreachable end
          i32.const 20 f32.load f32.const 0.5 f32.ne if unreachable end
          i32.const 24 f32.load f32.const 3 f32.ne if unreachable end
          i64.const 42 i32.const 0 i32.const 32 call $read i32.const -3 i32.ne if unreachable end
          i32.const 0 i64.load i32.const 8 i64.load i64.or
          i32.const 16 i64.load i64.or i32.const 24 i64.load i64.or
          i64.eqz i32.eqz if unreachable end''',extra='''
          (import "crml_v1" "physics_select" (func $select (result i32)))
          (import "crml_v1" "physics_target" (func $target (result i64)))''')
        self.assertIn('Gameplay calls: 4; failures: 0; owners: 0',self.run_gameplay())

    def test_snapshot_capabilities_are_independent(self):
        for kind,caps in [('player','player.motion'),('camera','player.read'),('physics','camera.read')]:
            self.mods=self.root/kind;self.mods.mkdir()
            self.snapshot_package(kind,'',caps=caps)
            self.assertIn('unknown import',self.run_host(1))

    def test_snapshot_argument_validation_precedes_native_access(self):
        for i,(kind,offset,size,memory,error) in enumerate([
            ('player',0,31,'(memory (export "memory") 1)','size does not match'),
            ('camera',0,81,'(memory (export "memory") 1)','size does not match'),
            ('physics',65505,32,'(memory (export "memory") 1)','outside guest memory'),
            ('player',-1,32,'(memory (export "memory") 1)','outside guest memory'),
            ('camera',65500,80,'(memory (export "memory") 1)','outside guest memory'),
            ('player',0,32,'','Missing guest memory'),
            ('camera',0,80,'(global (export "memory") i32 (i32.const 0))','Invalid guest memory')]):
            with self.subTest(kind=kind,offset=offset,size=size):
                self.mods=self.root/f'bad-snapshot-{i}';self.mods.mkdir()
                prefix='i64.const 1 ' if kind=='physics' else ''
                self.snapshot_package(kind,f'{prefix}i32.const {offset} i32.const {size} call $read drop',memory=memory)
                output=self.run_gameplay()
                self.assertIn(error,output)
                self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',output)

    def test_snapshot_and_capability_share_observation_budget(self):
        self.snapshot_package('player','(loop call $caps drop i32.const 0 i32.const 32 call $read drop br 0)',
            extra='(import "crml_v1" "capabilities" (func $caps (result i32)))')
        output=self.run_gameplay()
        self.assertIn('Observation call budget exceeded',output)
        self.assertIn('Gameplay calls: 4; failures: 1; owners: 0',output)

    def test_physics_snapshot_foreign_mod_cannot_read_known_token(self):
        self.package(f'''(module
          (import "crml_v1" "physics_select" (func $select (result i32))) {BASE}
          (func (export "crml_init") call $select if unreachable end))''',name='aaa',
          manifest='id=aaa\nabi=1\nmodule=mod.wasm\ncapabilities=physics.damping\n')
        self.snapshot_package('physics','''
          i64.const 0x123456789abcdef i32.const 0 i32.const 32 call $read
          i32.const -3 i32.ne if unreachable end
          i32.const 0 i64.load i32.const 8 i64.load i64.or
          i32.const 16 i64.load i64.or i32.const 24 i64.load i64.or
          i64.eqz i32.eqz if unreachable end''')
        self.assertIn('Gameplay calls: 2; failures: 0; owners: 0',self.run_gameplay())

    def test_state_watch_example_runs_without_game_or_mutation_capabilities(self):
        example=Path(__file__).resolve().parents[1]/'examples/state-watch'
        self.package((example/'state-watch.wat').read_text(),
            manifest=(example/'mod.ini').read_text().replace('state-watch.wasm','mod.wasm'))
        output=self.run_host()
        self.assertIn('Active: 1; failures: 0',output)
        result=subprocess.run([str(BIN/'crml_gameplay_tests.exe'),str(self.mods),'4'],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('State watch: player position available.',result.stdout)
        self.assertIn('State watch: selected camera changed.',result.stdout)
        self.assertIn('Gameplay calls: 2; failures: 0; owners: 0',result.stdout)

    def test_snapshot_unavailable_host_zeros_output(self):
        self.snapshot_package('player','''
          i32.const 0 i32.const -1 i32.const 32 memory.fill
          i32.const 0 i32.const 32 call $read i32.const -1 i32.ne if unreachable end
          i32.const 0 i64.load i32.const 8 i64.load i64.or
          i32.const 16 i64.load i64.or i32.const 24 i64.load i64.or
          i64.eqz i32.eqz if unreachable end''')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_services_prepared_from_all_valid_manifests_before_guest_init(self):
        body=f'(module {BASE} (func (export "crml_init")))'
        self.package(body,'first',manifest='id=first\nabi=1\nmodule=mod.wasm\ncapabilities=player.motion\n')
        self.package(body,'second',manifest='id=second\nabi=1\nmodule=mod.wasm\ncapabilities=physics.damping,player.visibility\n')
        self.package(body,'bad',manifest='id=bad\nabi=1\nmodule=mod.wasm\ncapabilities=player.noclip\nunknown=true\n')
        output=self.run_gameplay()
        self.assertIn('Prepared capabilities: 88',output)
        self.assertLess(output.index('Prepared capabilities:'),output.index('Loaded first'))
        self.assertIn('failures: 1; owners: 0',output)

    def action_package(self, body, caps='input.actions', bindings='action.0=F10\n', name='test'):
        imports='''(import "crml_v1" "capabilities" (func $caps (result i32)))
        (import "crml_v1" "input_actions" (func $actions (result i32)))'''
        self.package(f'(module {imports} {BASE} {body})',name,
                     manifest=f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities={caps}\n{bindings}')

    def test_capabilities_do_not_grant_imports(self):
        self.action_package('(func (export "crml_init"))',caps='log',bindings='')
        self.assertIn('unknown import',self.run_host(1))

    def test_capabilities_report_declared_and_available_only(self):
        self.package(f'''(module (import "crml_v1" "capabilities" (func $caps (result i32))) {BASE}
          (func (export "crml_init") call $caps i32.const 1 i32.ne if unreachable end))''',
          manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=log,player.motion,input.actions\n')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_capabilities_query_without_permissions(self):
        self.package(f'''(module (import "crml_v1" "capabilities" (func $caps (result i32))) {BASE}
          (func (export "crml_init") call $caps if unreachable end))''',
          manifest='id=test\nabi=1\nmodule=mod.wasm\n')
        self.assertIn('failures: 0; owners: 0',self.run_gameplay())

    def test_actions_unavailable_in_standalone_host(self):
        self.action_package('(func (export "crml_init") call $caps if unreachable end call $actions if unreachable end)')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_action_bindings_and_permissions_are_per_mod(self):
        self.action_package('''(func (export "crml_init") call $caps i32.const 128 i32.ne if unreachable end
          call $actions i32.const 32769 i32.ne if unreachable end)''',
          bindings='action.0=F10\naction.1=None\naction.15=F10\n',name='first')
        self.action_package('''(func (export "crml_init") call $actions if unreachable end)''',
          bindings='action.0=F8\n',name='second')
        output=self.run_gameplay()
        self.assertIn('Loaded first',output);self.assertIn('Loaded second',output)
        self.assertIn('failures: 0; owners: 0',output)

    def test_action_binding_requires_its_permission(self):
        self.action_package('(func (export "crml_init"))',caps='log')
        self.assertIn('Action bindings require input.actions',self.run_host(1))

    def test_action_manifest_rejects_invalid_fields_and_keys(self):
        for i,binding in enumerate(('action.16=F10','action.01=F10','action.-1=F10','action.0=121','action.0=Escape','action.0=F10\naction.0=F8')):
            with self.subTest(binding=binding):
                self.action_package('(func (export "crml_init"))',bindings=binding+'\n',name=f'invalid{i}')
                self.assertIn(f'Rejected invalid{i}:',self.run_host(1))

    def test_capability_query_and_actions_share_observation_budget(self):
        self.action_package('''(func (export "crml_init")
          (loop call $caps drop call $actions drop br 0))''')
        self.assertIn('Observation call budget exceeded',self.run_host(1))

    def test_release_is_owner_scoped_and_mod_keeps_running(self):
        self.package(f'''(module
          (import "crml_v1" "noclip_poll" (func $lease (param f32) (result i32)))
          (import "crml_v1" "physics_status" (func $status (result i32)))
          (import "crml_v1" "release" (func $release)) {BASE}
          (func (export "crml_init") f32.const 1 call $lease drop)
          (func (export "crml_tick") (param f32)
            call $status i32.const 3 i32.ne if unreachable end
            call $release call $status if unreachable end)
          )''',name='first',manifest='id=first\nabi=1\nmodule=mod.wasm\ncapabilities=player.noclip,physics.damping\n')
        self.package(f'''(module (import "crml_v1" "release" (func $release)) {BASE}
          (func (export "crml_init") call $release))''',name='second',
          manifest='id=second\nabi=1\nmodule=mod.wasm\n')
        self.assertIn('failures: 0; owners: 0',self.run_gameplay())

    def test_release_budget_applies_without_gameplay_provider(self):
        self.package(f'''(module (import "crml_v1" "release" (func $release)) {BASE}
          (func (export "crml_init") (loop call $release br 0)))''')
        self.assertIn('Gameplay call budget exceeded',self.run_host(1))

    def test_input_actions_example_handles_standalone_host(self):
        example=Path(__file__).resolve().parents[1]/'examples/input-actions'
        self.package((example/'input-actions.wat').read_text(),manifest=(example/'mod.ini').read_text().replace('module=input-actions.wasm','module=mod.wasm'))
        output=self.run_host()
        self.assertIn('Input actions unavailable',output)
        self.assertNotIn('pressed',output)

    def test_visibility_snapshot_requires_permission(self):
        self.snapshot_package('visibility','',caps='')
        self.assertIn('unknown import',self.run_host(1))

    def test_visibility_snapshot_unsupported_zeroes_last_valid_bytes(self):
        self.snapshot_package('visibility','''i32.const 65520 i32.const 255 i32.const 16 memory.fill
          i32.const 65520 i32.const 16 call $read i32.const -1 i32.ne if unreachable end
          i32.const 65520 i64.load i64.eqz i32.eqz if unreachable end
          i32.const 65528 i64.load i64.eqz i32.eqz if unreachable end''')
        self.run_host()

    def test_visibility_snapshot_memory_and_size_guards(self):
        for i,(offset,size,memory,error) in enumerate([
            (0,15,'(memory (export "memory") 1)','size does not match'),
            (0,17,'(memory (export "memory") 1)','size does not match'),
            (65521,16,'(memory (export "memory") 1)','outside guest memory'),
            (-1,16,'(memory (export "memory") 1)','outside guest memory'),
            (0,16,'','Missing guest memory'),
            (0,16,'(global (export "memory") i32 (i32.const 0))','Invalid guest memory')]):
            with self.subTest(case=i):
                self.mods=self.root/f'visibility-memory-{i}';self.mods.mkdir()
                self.snapshot_package('visibility',f'i32.const {offset} i32.const {size} call $read drop',memory=memory)
                self.assertIn(error,self.run_host(1))

    def test_visibility_snapshot_observation_budget(self):
        self.snapshot_package('visibility','(loop i32.const 0 i32.const 16 call $read drop br 0)')
        self.assertIn('Observation call budget exceeded',self.run_host(1))

    def motion_snapshot_package(self, body, capability='player.motion', memory='(memory (export "memory") 1)'):
        self.package(f'''(module
          (import "crml_v1" "motion_read" (func $read (param i32 i32) (result i32)))
          {BASE} {memory} (func (export "crml_init") {body}))''',
          manifest=f'id=test\nabi=1\nmodule=mod.wasm\ncapabilities={capability}\n')

    def test_motion_snapshot_requires_permission(self):
        self.motion_snapshot_package('', capability='')
        self.assertIn('unknown import', self.run_host(1))

    def test_motion_snapshot_unsupported_clears_entire_buffer_at_end_of_memory(self):
        self.motion_snapshot_package('''i32.const 65520 i64.const -1 i64.store
          i32.const 65528 i64.const -1 i64.store
          i32.const 65520 i32.const 16 call $read i32.const -1 i32.ne if unreachable end
          i32.const 65520 i64.load i64.eqz i32.eqz if unreachable end
          i32.const 65528 i64.load i64.eqz i32.eqz if unreachable end''')
        self.run_host()

    def test_motion_snapshot_rejects_invalid_size(self):
        self.motion_snapshot_package('i32.const 0 i32.const 15 call $read drop')
        self.assertIn('Snapshot output size does not match ABI', self.run_host(1))

    def test_motion_snapshot_rejects_invalid_range(self):
        self.motion_snapshot_package('i32.const 65521 i32.const 16 call $read drop')
        self.assertIn('Snapshot output outside guest memory', self.run_host(1))

    def test_motion_snapshot_requires_memory(self):
        self.motion_snapshot_package('i32.const 0 i32.const 16 call $read drop', memory='')
        self.assertIn('Missing guest memory', self.run_host(1))

    def test_motion_snapshot_shares_observation_budget(self):
        self.motion_snapshot_package('(loop i32.const 0 i32.const 16 call $read drop br 0)')
        self.assertIn('Observation call budget exceeded', self.run_host(1))

    def movement_package(self, body, capability='player.motion,input.motion'):
        imports='''(import "crml_v1" "input_motion" (func $input (result i32)))
        (import "crml_v1" "motion_camera" (func $camera (param i32) (result i32)))
        (import "crml_v1" "motion_set" (func $set (param i32 f32 f32 f32) (result i32)))'''
        self.package(f'(module {imports} {BASE} {body})',manifest=f'id=test\nabi=1\nmodule=mod.wasm\ncapabilities={capability}\n')

    def test_movement_example_guest_calculates_velocity(self):
        source=(Path(__file__).resolve().parents[1]/'examples/movement/movement.wat').read_text()
        self.package(source,manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=player.motion,input.motion\n')
        result=subprocess.run([str(BIN/'crml_gameplay_tests.exe'),str(self.mods),'4'],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('Motion request: 1 velocity 5,0,0',result.stdout)
        self.assertIn('Motion request: 1 velocity 15,0,0',result.stdout)
        self.assertIn('Gameplay calls: 5; failures: 0; owners: 0',result.stdout)

    def test_movement_input_separate_capability(self):
        self.movement_package('(func (export "crml_init"))','player.motion')
        self.assertIn('unknown import',self.run_host(1))

    def test_movement_speed_budget_and_cleanup(self):
        self.movement_package('''(func (export "crml_init")
          i32.const 1 f32.const 5 f32.const 0 f32.const 0 call $set drop
          i32.const 1 f32.const 20 f32.const 20 f32.const 0 call $set drop)''')
        self.assertIn('Gameplay calls: 1; failures: 1; owners: 0',self.run_gameplay())

    def test_movement_nan_rejected(self):
        self.movement_package('''(func (export "crml_init")
          i32.const 1 f32.const nan f32.const 0 f32.const 0 call $set drop)''')
        self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',self.run_gameplay())

    def test_movement_observation_exhaustion_still_cleans_up(self):
        self.movement_package('''(func (export "crml_init")
          i32.const 1 f32.const 0 f32.const 0 f32.const 0 call $set drop
          (loop call $input drop br 0))''')
        output=self.run_gameplay()
        self.assertIn('Observation call budget exceeded',output)
        self.assertIn('Gameplay calls: 1; failures: 1; owners: 0',output)

    def test_movement_camera_memory_bounds(self):
        self.movement_package('''(memory (export "memory") 1)
          (func (export "crml_init") i32.const 1 f32.const 0 f32.const 0 f32.const 0 call $set drop
          i32.const 65532 call $camera drop)''')
        output=self.run_gameplay()
        self.assertIn('Camera output outside guest memory',output)
        self.assertIn('Gameplay calls: 1; failures: 1; owners: 0',output)

    def test_movement_unavailable_camera_zeroes_output(self):
        self.movement_package('''(memory (export "memory") 1)
          (func (export "crml_init") i32.const 0 f32.const 99 f32.store
          i32.const 4 f32.const 99 f32.store
          i32.const 0 call $camera i32.const -1 i32.ne if unreachable end
          i32.const 0 i64.load i64.eqz i32.eqz if unreachable end
          call $input if unreachable end)''')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_movement_camera_last_valid_bytes(self):
        self.movement_package('''(memory (export "memory") 1)
          (func (export "crml_init") i32.const 65528 call $camera i32.const 1 i32.ne if unreachable end
          i32.const 65528 f32.load f32.const 0 f32.ne if unreachable end
          i32.const 65532 f32.load f32.const -1 f32.ne if unreachable end)''')
        self.assertIn('failures: 0; owners: 0',self.run_gameplay())

    def physics_package(self, body, capability='physics.damping'):
        imports='''(import "crml_v1" "physics_select" (func $select (result i32)))
        (import "crml_v1" "physics_target" (func $target (result i64)))
        (import "crml_v1" "physics_apply" (func $apply (param i64 f32 i32) (result i32)))
        (import "crml_v1" "physics_status" (func $status (result i32)))
        (import "crml_v1" "physics_restore" (func $restore (result i32)))'''
        self.package(f'(module {imports} {BASE} {body})', manifest=f'id=test\nabi=1\nmodule=mod.wasm\ncapabilities={capability}\n')

    def run_gameplay(self,argument=None):
        command=[str(BIN/'crml_gameplay_tests.exe'),str(self.mods)]
        if argument is not None: command.append(argument)
        result=subprocess.run(command,capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        return result.stdout

    def test_physics_guest_bridge_and_cleanup(self):
        self.physics_package('''(func (export "crml_init")
          call $select if unreachable end
          call $status i32.const 3 i32.ne if unreachable end
          call $target f32.const 8 i32.const 5000 call $apply if unreachable end)
          (func (export "crml_tick") (param f32) unreachable)''')
        output=self.run_gameplay()
        self.assertIn('Physics apply: owner-scoped token',output)
        self.assertIn('Gameplay calls: 4; failures: 1; owners: 0',output)

    def region_package(self, args, capability='physics.damping', body=''):
        self.package(f'''(module
          (import "crml_v1" "physics_select_near" (func $select (param f32 f32 f32 f32) (result i32)))
          (import "crml_v1" "physics_target" (func $target (result i64)))
          (import "crml_v1" "physics_apply" (func $apply (param i64 f32 i32) (result i32)))
          {BASE}
          (func (export "crml_init") {args} call $select if unreachable end {body}))''',
          manifest=f'id=test\nabi=1\nmodule=mod.wasm\ncapabilities={capability}\n')

    def test_physics_region_and_damping_are_guest_arguments(self):
        self.region_package('f32.const 1 f32.const -2 f32.const 3 f32.const 4',
            body='call $target f32.const 0.25 i32.const 1234 call $apply if unreachable end')
        output=self.run_gameplay()
        self.assertIn('Physics query: 1,-2,3 radius 4',output)
        self.assertIn('damping 0.25, duration 1234',output)
        self.assertIn('Gameplay calls: 3; failures: 0; owners: 0',output)

    def test_physics_region_rejects_invalid_guest_arguments(self):
        for index,(x,y,z,r) in enumerate([('nan','0','0','2'),('0','inf','0','2'),('0','0','-inf','2'),
                         ('0','0','0','nan'),('0','0','0','0'),('0','0','0','-1'),
                         ('0','0','0','20.1'),('15','15','0','2')]):
            with self.subTest(args=(x,y,z,r)):
                self.mods=self.root/f'invalid-region-{index}'
                self.mods.mkdir()
                self.region_package(f'f32.const {x} f32.const {y} f32.const {z} f32.const {r}')
                output=self.run_gameplay()
                self.assertIn('selection region out of range',output)
                self.assertIn('Gameplay calls: 0; failures: 1; owners: 0',output)

    def test_physics_region_capability_required(self):
        self.region_package('f32.const 0 f32.const 0 f32.const 0 f32.const 2',capability='')
        self.assertIn('unknown import',self.run_host(1))

    def test_physics_capability_required(self):
        self.physics_package('(func (export "crml_init"))', '')
        self.assertIn('unknown import',self.run_host(1))

    def test_physics_unavailable_host(self):
        self.physics_package('''(func (export "crml_init")
          call $select i32.const -1 i32.ne if unreachable end
          call $target i64.eqz i32.eqz if unreachable end
          i64.const 1 f32.const 8 i32.const 5000 call $apply i32.const -1 i32.ne if unreachable end
          call $restore i32.const -1 i32.ne if unreachable end)''')
        self.assertIn('Active: 1; failures: 0',self.run_host())

    def test_physics_invalid_duration_traps_and_releases(self):
        self.physics_package('''(func (export "crml_init") call $select drop
          call $target f32.const 8 i32.const 5001 call $apply drop)''')
        self.assertIn('Gameplay calls: 2; failures: 1; owners: 0',self.run_gameplay())

    def test_physics_nonfinite_damping_traps(self):
        self.physics_package('''(func (export "crml_init")
          i64.const 1 f32.const nan i32.const 5000 call $apply drop)''')
        self.assertIn('out of range',self.run_host(1))

    def test_physics_observation_exhaustion_still_cleans_up(self):
        self.physics_package('''(func (export "crml_init") call $select drop
          (loop call $status drop br 0))''')
        output=self.run_gameplay()
        self.assertIn('Observation call budget exceeded',output)
        self.assertIn('Gameplay calls: 9; failures: 1; owners: 0',output)

    def test_physics_example_uses_guest_input(self):
        source=(Path(__file__).resolve().parents[1]/'examples/physics-damping/physics-damping.wat').read_text()
        self.package(source,manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=log,input.actions,physics.damping\naction.0=Home\naction.1=End\n')
        result=subprocess.run([str(BIN/'crml_gameplay_tests.exe'),str(self.mods),'4'],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('Physics apply: owner-scoped token',result.stdout)
        self.assertIn('Physics query: 0,0,0 radius 2',result.stdout)
        self.assertIn('Physics: Home select, End damping, F11 restore.',result.stdout)
        self.assertIn('Physics: search queued.',result.stdout)
        self.assertIn('Physics: damping queued.',result.stdout)
        self.assertIn('Physics: sampled damping matches request.',result.stdout)
        self.assertIn('Gameplay calls: 8; failures: 0; owners: 0',result.stdout)

    def test_physics_owner_cleanup_after_start_trap(self):
        self.physics_package('(func $start call $select drop unreachable) (start $start) (func (export "crml_init"))')
        self.assertIn('Gameplay calls: 1; failures: 1; owners: 0',self.run_gameplay())

    def test_physics_restore_bridge(self):
        self.physics_package('''(func (export "crml_init") call $select drop call $restore if unreachable end
          call $target i64.eqz i32.eqz if unreachable end)''')
        self.assertIn('Gameplay calls: 3; failures: 0; owners: 0',self.run_gameplay())

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
        (folder / 'mod.ini').write_text(manifest or f'id={name}\nabi=1\nmodule=mod.wasm\ncapabilities=log\n', encoding='utf-8')
        wat = self.root / f'{name}.wat'
        wat.write_text(body)
        subprocess.run([str(BIN / 'crml_wat.exe'), str(wat), str(folder / 'mod.wasm')], check=True, capture_output=True)
        return folder

    def run_host(self, code=0):
        result = subprocess.run([str(BIN / 'crml_host.exe'), str(self.mods), '2'], capture_output=True, text=True, encoding='utf-8', timeout=10)
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

    def test_input_observation_limit_with_visibility_commands(self):
        self.package(f'(module (import "crml_v1" "input_buttons" (func $input (result i32))) (import "crml_v1" "visibility_set" (func $set (param i32) (result i32))) {BASE} (func (export "crml_init") (loop call $input drop i32.const 0 call $set drop br 0)))',manifest='id=test\nabi=1\nmodule=mod.wasm\ncapabilities=input.buttons,player.visibility\n')
        self.assertIn('Observation call budget exceeded',self.run_host(1))

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
        self.assertEqual(re.findall(r'\[test\] \[info\] ([ITS])', output), ['I', 'T', 'T', 'S'])

    def test_guest_flood_cannot_hide_later_fault_or_other_mod_logs(self):
        # More than the old 4 MiB session cap, but within each callback's guest
        # limits. Lifecycle diagnostics and another mod retain their own space.
        calls = 'i32.const 1 i32.const 0 i32.const 4096 call $log ' * 4
        self.package(f'''(module {LOG} {BASE}
            (memory (export "memory") 1) (data (i32.const 0) "{'X' * 4096}")
            (func (export "crml_init"))
            (func (export "crml_tick") (param f32) {calls}))''', name='a-spam')
        self.package(f'''(module {BASE} (global $n (mut i32) (i32.const 0))
            (func (export "crml_init")) (func (export "crml_tick") (param f32)
            global.get $n i32.const 1 i32.add global.set $n
            global.get $n i32.const 300 i32.eq if unreachable end))''', name='z-fault')
        self.package(f'''(module {LOG} {BASE} (global $n (mut i32) (i32.const 0))
            (memory (export "memory") 1) (data (i32.const 0) "survivor")
            (func (export "crml_init")) (func (export "crml_tick") (param f32)
            global.get $n i32.const 1 i32.add global.set $n
            global.get $n i32.const 300 i32.eq if
              i32.const 2 i32.const 0 i32.const 8 call $log end))''', name='z-healthy')
        result = subprocess.run([str(BIN / 'crml_host.exe'), str(self.mods), '300'],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('Disabled z-fault:', result.stdout)
        self.assertIn('[z-healthy] [warning] survivor', result.stdout)
        self.assertIn('Active: 2; failures: 1', result.stdout)
        self.assertEqual(result.stdout.count('Guest log limit reached for a-spam'), 1)
        self.assertLess(len(result.stdout), 70 * 1024)

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
        self.assertIn('[noclip] [warning] Experimental noclip: unavailable\n', self.run_host())

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
