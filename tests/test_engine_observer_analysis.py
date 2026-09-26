import copy
import io
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_engine_observer import analyze, PHASES, MAX_LINE


def fixture():
    rows = [{'type': 'header', 'schema': 1, 'mode': 'observe-only', 'mods_suspended': True,
             'sha256': 'a' * 64, 'qpc_frequency': 1000, 'qpc_origin': 0,
             'hooks': [{'name': name, 'rva': i} for i, name in enumerate(PHASES)]}]
    for i, name in enumerate(PHASES):
        for edge in (1, 2):
            rows.append({'type': 'event', 'sequence': len(rows), 'qpc': len(rows) * 10,
                         'thread': 2 if name == 'physics_complete' else 1, 'kind': name,
                         'edge': edge, 'span': i + 1, 'object': '123', 'entity': '0',
                         'value': '0', 'detail': '0', 'flags': 0})
    rows.append({'type': 'event', 'sequence': len(rows), 'qpc': 200, 'thread': 1,
                 'kind': 'player', 'edge': 0, 'span': 100, 'object': '9',
                 'entity': '4294967300', 'value': '0', 'detail': '0', 'flags': 1})
    rows.append({'type': 'event', 'sequence': len(rows), 'qpc': 201, 'thread': 1,
                 'kind': 'resource', 'edge': 0, 'span': 9, 'object': '44',
                 'entity': '4294967300', 'value': '4006503802', 'detail': '555', 'flags': 3})
    rows.append({'type': 'end', 'reason': 'time_limit', 'dropped': 0})
    return rows


def stream(rows):
    return io.BytesIO(b''.join(json.dumps(row).encode() + b'\n' for row in rows))


class CaptureTests(unittest.TestCase):
    def test_phase_durations_thread_ownership_and_resource_identity(self):
        report = analyze(stream(fixture()))
        self.assertEqual(report['status'], 'ready_for_manual_review')
        self.assertTrue(report['not_a_gameplay_or_api_validation'])
        self.assertEqual(report['phases']['physics_begin']['mean_ms'], 10)
        self.assertEqual(report['physics_wrapper_threads']['123']['physics_complete'], [2])
        self.assertEqual(report['physics_wrapper_threads']['123']['physics_wait'], [1])
        self.assertEqual(report['resource_identities'], 1)

    def test_loss_missing_phases_and_hook_failures_cannot_look_complete(self):
        for reason in ('drop', 'phase', 'failure', 'player', 'resource'):
            rows = fixture()
            if reason == 'drop': rows[-1]['dropped'] = 9
            if reason == 'phase': rows = [r for r in rows if r.get('kind') != 'physics_complete']
            if reason == 'failure': rows[-1]['reason'] = 'hook_enable_failed'
            if reason == 'player': rows = [r for r in rows if r.get('kind') != 'player']
            if reason == 'resource': rows = [r for r in rows if r.get('kind') != 'resource']
            with self.subTest(reason=reason):
                self.assertEqual(analyze(stream(rows))['status'], 'incomplete')

    def test_partial_exit_preserves_completed_records_with_explicit_warning(self):
        data = stream(fixture()[:-1]).getvalue() + b'{"type":'
        report = analyze(io.BytesIO(data))
        self.assertEqual(report['player_samples'], 1)
        self.assertIsNone(report['end_reason'])
        self.assertTrue(any('partial' in note for note in report['notes']))

    def test_reload_is_observation_change_not_destruction_claim(self):
        rows = fixture()
        player = copy.deepcopy(rows[-3])
        player.update(sequence=20, qpc=5000, entity='8589934596', object='10')
        resource = copy.deepcopy(rows[-2])
        resource.update(sequence=21, qpc=5001, object='45', detail='556')
        rows[-1:-1] = [player, resource]
        report = analyze(stream(rows))
        self.assertEqual(report['player_identities'], 2)
        self.assertEqual(report['resource_replacements_by_component'], {'4006503802': 1})
        self.assertTrue(any('not prove destruction' in note for note in report['notes']))

    def test_bad_spans_schema_and_numeric_identity_rejected(self):
        for field, value in [('thread', 8), ('object', '45'), ('span', 0), ('qpc', 0), ('sequence', 1), ('entity', 22)]:
            rows = fixture()
            rows[2][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                analyze(stream(rows))
        rows = fixture(); rows[0]['mods_suspended'] = False
        with self.assertRaises(ValueError): analyze(stream(rows))
        rows = fixture(); rows[0]['schema'] = 999
        with self.assertRaises(ValueError): analyze(stream(rows))

    def test_bounded_and_malformed_input(self):
        for data in (b'', b'x' * (MAX_LINE + 1), b'[]\n', b'{broken}\n'):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                analyze(io.BytesIO(data))


if __name__ == '__main__':
    unittest.main()
