"""Offline verifier regression fixtures; these do not simulate dialogue gameplay."""
import copy
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from test_engine_research import pe_fixture
from verify_dialogue_observation import verify_trace


class DialogueTraceTests(unittest.TestCase):
    def setUp(self):
        self.data = pe_fixture()
        signature = b'void __cdecl example::system(void)'
        self.data[0x440:0x440 + len(signature) + 1] = signature + b'\0'
        self.profile = dict(schema=1, executable_sha256=hashlib.sha256(self.data).hexdigest(),
                            checks=[dict(kind='lea_rax_rip', rva='0x1000', target='0x2000')],
                            instruction_checks=[dict(rva='0x1000', bytes=self.data[0x200:0x207].hex())],
                            declarations=[dict(name='example::system', signature_rva='0x2040',
                                               signature_sha256=hashlib.sha256(signature).hexdigest())],
                            ui_contract=dict(slots=[0], fields=['line', 'speaker'],
                                             visibility='visible', options=['names']))
        self.ui = 'ui_subtitle_0_line.value ui_subtitle_0_speaker.value visible.value names.value'

    def test_valid_static_trace(self):
        self.assertEqual(verify_trace(self.data, self.profile, self.ui), [])

    def test_build_drift_rejected_before_interpretation(self):
        self.data[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'fingerprint differs'):
            verify_trace(self.data, self.profile)

    def test_instruction_operand_and_section_guard(self):
        for changes in [dict(bytes='90'), dict(rva='0x2000', bytes='6e')]:
            profile = copy.deepcopy(self.profile)
            profile['instruction_checks'][0].update(changes)
            self.assertEqual(len(verify_trace(self.data, profile)), 1)

    def test_declaration_hash_and_name_guard(self):
        for changes in [dict(signature_sha256='0' * 64), dict(name='wrong::system')]:
            profile = copy.deepcopy(self.profile)
            profile['declarations'][0].update(changes)
            self.assertEqual(len(verify_trace(self.data, profile)), 1)

    def test_ui_missing_speaker_or_new_slot_is_reported(self):
        self.assertEqual(len(verify_trace(self.data, self.profile,
                                         self.ui.replace('ui_subtitle_0_speaker.value', ''))), 1)
        self.assertEqual(len(verify_trace(self.data, self.profile,
                                         self.ui + ' ui_subtitle_1_line.value')), 1)

    def test_absent_checks_cannot_report_success(self):
        for key in ['declarations', 'instruction_checks']:
            profile = copy.deepcopy(self.profile)
            profile[key] = []
            with self.assertRaises(ValueError):
                verify_trace(self.data, profile)


if __name__ == '__main__':
    unittest.main()
