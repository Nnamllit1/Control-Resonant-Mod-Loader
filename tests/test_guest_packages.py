"""Acceptance of maintained guest packages through real Wasm and author services."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('crml_mod', ROOT/'tools/mod.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
BIN = None
PACKAGES = ('photo-visibility', 'startup-preferences')


def envelope(payload):
    checksum = 2166136261
    for byte in payload:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    return b'CRMLDB01'+struct.pack('<II',len(payload),checksum)+payload


class GuestPackages(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        cls.mods = cls.root/'mods'
        for name in PACKAGES:
            mod.build(ROOT/'examples'/name,cls.mods/name)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=self.root)
        self.addCleanup(self.directory.cleanup)
        self.data = Path(self.directory.name)/'data'
        self.data.mkdir()

    def run_commands(self, commands):
        process = subprocess.run([str(BIN/'crml_author_services_tests.exe'),str(self.mods),str(self.data)],
                                 input='\n'.join(commands)+'\n',capture_output=True,text=True,timeout=15)
        self.assertEqual(process.returncode,0,process.stdout+'\n'+process.stderr)
        self.assertIn('Author services passed',process.stdout)
        return process.stdout

    def test_preferences_survive_real_host_restart_and_stay_owner_local(self):
        self.run_commands(['expect photo-visibility enabled 1','expect startup-preferences enabled 1',
                           'set photo-visibility enabled 0', *(['tick 250']*8), 'settle photo-visibility 2'])
        self.run_commands(['expect photo-visibility enabled 0','expect startup-preferences enabled 1',
                           'set startup-preferences skip_save_warning 0', *(['tick 250']*8), 'settle startup-preferences 2'])
        self.run_commands(['expect photo-visibility enabled 0','expect startup-preferences enabled 1',
                           'expect startup-preferences skip_save_warning 0'])

    def test_clock_preserves_full_width_and_clamps_backward_provider(self):
        source=Path(self.directory.name)/'clock-source'
        source.mkdir()
        (source/'mod.ini').write_text('id=clock-test\nabi=1\nmodule=clock.wasm\ncapabilities=\n')
        (source/'main.c').write_text('''#include "crml.h"
uint32_t crml_abi_version(void){return 1;}
static unsigned step;
void crml_init(void){if(crml_clock_ms()!=0) __builtin_trap();}
void crml_tick(float dt){
 (void)dt; const uint64_t t=UINT64_C(0xf123456789abcdef);
 const uint64_t expected[]={t,t,t+1};
 if(crml_clock_ms()!=expected[step++] || crml_clock_ms()!=expected[step-1]) __builtin_trap();
}
void crml_shutdown(void){if(crml_clock_ms()!=UINT64_C(0xf123456789abcdf0)) __builtin_trap();}
''')
        clock_mods=Path(self.directory.name)/'clock-mods'
        mod.build(source,clock_mods/'clock')
        high=0xf123456789abcdef
        result=subprocess.run([str(BIN/'crml_author_services_tests.exe'),str(clock_mods),str(self.data)],
                              input=f'clock {high}\ntick 0\nclock 10\ntick 0\nclock {high+1}\ntick 0\n',
                              capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_unknown_record_is_preserved_until_explicit_reset(self):
        for name in PACKAGES:
            record = self.data/f'mod-{name}.bin'
            original = envelope(b'future-record-schema-99')
            record.write_bytes(original)
            self.run_commands([f'set {name} enabled 0', *(['tick 250']*8)])
            self.assertEqual(record.read_bytes(),original,name)
            self.run_commands([f'set {name} reset_preferences 1', *(['tick 250']*8), f'settle {name} 2',
                               f'expect {name} reset_preferences 0',f'expect {name} enabled 1'])
            self.assertNotEqual(record.read_bytes(),original,name)
            self.run_commands([f'expect {name} enabled 1'])

    def test_failed_replacement_does_not_claim_durable_preferences(self):
        for name in PACKAGES:
            with self.subTest(package=name):
                target = self.data/f'mod-{name}.bin'
                target.mkdir()
                self.run_commands([f'set {name} reset_preferences 1', *(['tick 250']*8), f'settle {name} -5'])
                self.assertTrue(target.is_dir())
                self.assertFalse((self.data/f'mod-{name}.pending').exists())

    def test_new_notice_and_preference_debounce_start_after_long_stall(self):
        self.run_commands(['clock 60000','set photo-visibility reset_preferences 1','tick 0',
                           'storage_status photo-visibility 0',
                           'feedback photo-visibility "Photo preferences reset. Saving is pending."',
                           'tick 599','storage_status photo-visibility 0','tick 1','settle photo-visibility 2',
                           'tick 5000','no_feedback photo-visibility'])

    def test_packaged_scenarios_run_deterministically(self):
        for name in PACKAGES:
            scenarios = list((ROOT/'examples'/name).glob('*.json'))
            self.assertTrue(scenarios,f'{name} needs guest behavior scenarios')
            for scenario in scenarios:
                with self.subTest(scenario=scenario.name):
                    # Per-package scenarios declare their own service selection.
                    isolated = self.root/('scenario-'+name)
                    isolated.mkdir(exist_ok=True)
                    import shutil
                    shutil.copytree(self.mods/name,isolated/name,dirs_exist_ok=True)
                    first = mod.simulate(isolated,scenario,BIN/'crml_host.exe')
                    second = mod.simulate(isolated,scenario,BIN/'crml_host.exe')
                    self.assertEqual(first,second)


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--bin',type=Path,required=True)
    args,remaining=parser.parse_known_args()
    BIN=args.bin.resolve()
    unittest.main(argv=[__file__,*remaining])
