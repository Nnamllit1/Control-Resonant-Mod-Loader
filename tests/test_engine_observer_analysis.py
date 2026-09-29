import copy
import io
import json
import struct
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_engine_observer import analyze, physics_windows, records, PHASES, CAMERA_PHASES, MAX_LINE


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
    def camera_fixture(self):
        rows = self.accessor_fixture()
        rows[0]['schema'] = 5
        rows[0]['hooks'].extend({'name': name, 'rva': i} for i, name in enumerate(CAMERA_PHASES))
        end = rows.pop()
        def add(**fields):
            row = dict(type='event', sequence=max(r.get('sequence', 0) for r in rows)+1, qpc=1000+len(rows), thread=1,
                       kind='camera_switch', edge=0, span=900, object='55', entity='0', value='0', detail='0', flags=0)
            row.update(fields); rows.append(row)
        add(edge=1)
        add(kind='camera_state', entity='66', value='1', detail='77', flags=256)
        add(kind='camera_slot', object='66', entity='77', value='88', flags=256 | (7 << 2))
        add(kind='camera_state', entity='66', value='2', detail='99', flags=512)
        add(kind='camera_slot', object='66', entity='99', value='88', detail='123', flags=512 | (15 << 2) | 1)
        add(edge=2)
        add(kind='camera_update', span=901, edge=1)
        add(kind='camera_update', span=901, edge=2)
        rows.append(end)
        return rows

    def test_camera_schema_and_selection_pairing(self):
        report = analyze(stream(self.camera_fixture()))
        camera = report['camera_probe']
        self.assertEqual(camera['snapshot_pairs'], {'pairs': 1, 'readable_pairs': 1})
        self.assertEqual(camera['transitions'][0]['before']['mode'], 0)
        self.assertEqual(camera['transitions'][0]['after']['mode'], 1)
        self.assertEqual(camera['transitions'][0]['phase'], 'camera_switch')
        self.assertFalse(camera['ownership_or_mutation_verified'])
        self.assertEqual(camera['unobserved_phases'], ['camera_select', 'camera_init', 'camera_remove'])
        self.assertEqual(report['accessor_probe']['available'], True)
        self.assertEqual(report['accessor_probe']['matched_samples'], 1)

    def test_camera_malformed_records_and_incomplete_capture(self):
        for field, value in [('flags', 256 | 7), ('value', '5'), ('entity', '0'), ('thread', 99)]:
            rows = self.camera_fixture()
            next(r for r in rows if r.get('kind') == 'camera_state')[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): analyze(stream(rows))
        rows = self.camera_fixture()
        slot = next(r for r in rows if r.get('kind') == 'camera_slot')
        slot['flags'] = 256 | (4 << 2) # Pose without a live entity.
        with self.assertRaises(ValueError): analyze(stream(rows))
        rows = self.camera_fixture()
        for row in rows:
            if row.get('kind') == 'camera_state':
                row.update(flags=(row['flags'] & ~255) | 5, entity='0', value='0', detail='0')
        rows = [r for r in rows if r.get('kind') != 'camera_slot']
        report = analyze(stream(rows))
        self.assertEqual(report['camera_probe']['reads'], {'memory': 2})
        self.assertEqual(report['camera_probe']['transitions'], [])
        self.assertEqual(report['camera_probe']['status'], 'no_readable_pairs')
        self.assertEqual(report['status'], 'incomplete')
        rows = self.camera_fixture()
        rows = [r for r in rows if not (r.get('kind') == 'camera_state' and r['flags'] == 512)]
        self.assertEqual(analyze(stream(rows))['camera_probe']['unpaired_snapshots'], 1)

    def test_windows_record_terminators_preserve_capture_bounds(self):
        data = stream(fixture()).getvalue()
        with patch('analyze_engine_observer.MAX_BYTES', len(data)):
            self.assertEqual(analyze(io.BytesIO(data)),
                             analyze(io.BytesIO(data.replace(b'\n', b'\r\n'))))
            for terminator in (b'\n', b'\r\n'):
                oversized = data.replace(b'\n', terminator) + b'{}' + terminator
                with self.assertRaises(ValueError):
                    list(records(io.BytesIO(oversized)))
        # Only one CR in an actual CRLF terminator is discounted, not payload CRs.
        with patch('analyze_engine_observer.MAX_BYTES', 3):
            self.assertEqual(list(records(io.BytesIO(b'{}\r\n'))), [{}])
            with self.assertRaises(ValueError):
                list(records(io.BytesIO(b'{}\r\r\n')))
        for terminator in (b'\n', b'\r\n'):
            at_limit = b'{}' + b' ' * (MAX_LINE - 3) + terminator
            self.assertEqual(list(records(io.BytesIO(at_limit))), [{}])
            with self.assertRaises(ValueError):
                list(records(io.BytesIO(b' ' + at_limit)))

    def body_fixture(self):
        rows = fixture()
        rows[0]['schema'] = 2
        rows[0]['physx_sha256'] = 'b' * 64
        rows[0]['hooks'].append({'name': 'post_physics', 'rva': 0x2ce5310})
        base = dict(rows[1], kind='post_physics', span=99, object='777')
        packed = str(int.from_bytes(struct.pack('<ff', .25, .5), 'little'))
        rows[-1:-1] = [dict(base, sequence=20, qpc=220, edge=1),
            dict(base, kind='body', sequence=21, qpc=221, edge=0, object='888',
                 entity=str((7 << 32) | 1), value=packed, detail='777', flags=1),
            dict(base, kind='body_scan', sequence=22, qpc=222, edge=0,
                 entity='1', value='2', detail='10', flags=1 << 5),
            dict(base, sequence=23, qpc=223, edge=2)]
        return rows

    def test_body_generation_observations_and_scalar_decoding(self):
        rows = self.body_fixture()
        next_body = dict(rows[-4], sequence=24, qpc=225, entity=str((9 << 32) | 1), object='999', flags=3)
        rows[-1:-1] = [next_body]
        report = analyze(stream(rows))
        self.assertEqual(report['status'], 'ready_for_manual_review')
        probe = report['body_probe']
        self.assertEqual(probe['status'], 'observed')
        self.assertFalse(probe['ownership_or_mutation_verified'])
        self.assertEqual(probe['observed_slot_replacements'], 1)
        self.assertEqual(probe['scans_with_rejection'], {'actor_type': 1})
        self.assertEqual(probe['bodies'][0]['linear_min'], .25)
        self.assertEqual(probe['bodies'][0]['angular_max'], .5)
        self.assertEqual(probe['bodies'][1]['alternate_samples'], 1)

    def test_body_schema_and_malformed_values_are_rejected(self):
        for field, value in [('flags', 4), ('value', str(0x7fc00000)), ('detail', '0'), ('span', 0)]:
            rows = self.body_fixture(); rows[-4][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): analyze(stream(rows))
        for field, value in [('entity', '9'), ('value', '129'), ('detail', str((1 << 20) + 1)), ('flags', 1)]:
            rows = self.body_fixture(); rows[-3][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): analyze(stream(rows))
        rows = self.body_fixture(); del rows[0]['physx_sha256']
        with self.assertRaises(ValueError): analyze(stream(rows))
        rows = [r for r in self.body_fixture() if r.get('kind') != 'body']
        self.assertEqual(analyze(stream(rows))['status'], 'incomplete')
        self.assertEqual(analyze(stream(fixture()))['body_probe']['status'], 'not_recorded')

    def entity_fixture(self):
        rows = self.body_fixture()
        rows[0]['schema'] = 3
        base = dict(rows[-4], kind='entity_body', sequence=24, qpc=230, span=9,
                    object='777', entity='4294967300', value=str((7 << 32) | 1), detail='888', flags=1)
        rows[-1:-1] = [base, dict(base, kind='entity_scan', sequence=25, qpc=231,
                                  entity='1', value='4', detail='10', flags=(1 << 5) | (1 << 23))]
        return rows

    def test_entity_association_and_rejection_observations(self):
        rows = self.entity_fixture()
        rows[-1:-1] = [dict(rows[-3], sequence=26, qpc=232, entity='12884901892')]
        report = analyze(stream(rows))
        probe = report['entity_probe']
        self.assertEqual(report['status'], 'ready_for_manual_review')
        self.assertEqual(probe['status'], 'observed')
        self.assertEqual(len(probe['associations']), 2)
        self.assertEqual(probe['associations'][0]['world'], '9')
        self.assertEqual(probe['associations'][0]['handle'], str((7 << 32) | 1))
        self.assertEqual(probe['scans_with_rejection'], {'body.actor_type': 1, 'link.entity': 1})
        self.assertFalse(probe['ownership_or_mutation_verified'])
        self.assertEqual(analyze(stream(self.body_fixture()))['entity_probe']['status'], 'not_recorded')
        rows = [r for r in self.entity_fixture() if r.get('kind') != 'entity_body']
        self.assertEqual(analyze(stream(rows))['status'], 'incomplete')

    def test_entity_schema_and_bounds(self):
        for field, value in [('flags', 0), ('span', 0), ('entity', '0'), ('detail', '0'),
                             ('value', str((7 << 32) | (1 << 20)))]:
            rows = self.entity_fixture(); rows[-3][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): analyze(stream(rows))
        for field, value in [('flags', 1 << 16), ('flags', 1 << 12), ('value', '65'),
                             ('entity', '5'), ('object', '0')]:
            rows = self.entity_fixture(); rows[-2][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): analyze(stream(rows))
        rows = self.entity_fixture(); rows[0]['schema'] = 2
        with self.assertRaises(ValueError): analyze(stream(rows))
        rows = self.entity_fixture()
        rows[-2].update(object='0', entity='0', value='0', detail='0', flags=1 << 18)
        self.assertEqual(analyze(stream(rows))['entity_probe']['scans_with_rejection'], {'link.world': 1})

    def accessor_fixture(self):
        rows = self.entity_fixture(); rows[0]['schema'] = 4
        base = dict(rows[-3], kind='body_accessor', sequence=26, qpc=232, span=100,
                    object='888', entity=str((7 << 32) | 1), detail='777',
                    value=str(int.from_bytes(struct.pack('<ff', .25, .5), 'little')), flags=1)
        rows[-1:-1] = [base, dict(base, kind='accessor_scan', sequence=27, qpc=233,
                                  object='777', entity='1', value='1', detail='0', flags=0)]
        return rows

    def test_accessor_comparison_reports_and_missing_data(self):
        rows = self.accessor_fixture(); report = analyze(stream(rows)); probe = report['accessor_probe']
        self.assertEqual(report['status'], 'ready_for_manual_review')
        self.assertEqual(probe['status'], 'agreement_observed')
        self.assertEqual(probe['matched_samples'], 1)
        self.assertFalse(probe['ownership_or_mutation_verified'])
        self.assertEqual(probe['bodies'][0]['linear_min'], .25)
        rows[-3]['flags'] = 6; report = analyze(stream(rows))
        self.assertEqual(report['status'], 'incomplete')
        self.assertEqual(report['accessor_probe']['mismatched_samples'], 1)
        self.assertEqual(report['accessor_probe']['bodies'][0]['alternate_samples'], 1)
        rows = [r for r in self.accessor_fixture() if r.get('kind') != 'body_accessor']
        self.assertEqual(analyze(stream(rows))['accessor_probe']['status'], 'no_accepted_samples')
        rows = self.accessor_fixture(); rows[-2]['flags'] = 1 << 6
        self.assertEqual(analyze(stream(rows))['accessor_probe']['status'], 'mismatch_observed')
        self.assertEqual(analyze(stream(rows))['status'], 'incomplete')
        self.assertEqual(analyze(stream(self.entity_fixture()))['accessor_probe']['status'], 'not_recorded')

    def test_accessor_records_validate_schema_values_and_bounds(self):
        for field, value in [('flags', 0), ('flags', 5), ('span', 0), ('object', '0'),
                             ('value', str(0x7fc00000)), ('detail', '0')]:
            rows = self.accessor_fixture(); rows[-3][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): analyze(stream(rows))
        for field, value in [('entity', '2'), ('value', '5'), ('flags', 1), ('flags', 1 << 8), ('detail', '1')]:
            rows = self.accessor_fixture(); rows[-2][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): analyze(stream(rows))
        rows = self.accessor_fixture(); rows[0]['schema'] = 3
        with self.assertRaises(ValueError): analyze(stream(rows))

    def physics_cycle(self, complete_thread=1):
        return [(10, 'physics_begin', 1, 1, '123', 1),
                (20, 'physics_begin', 2, 1, '123', 1),
                (30, 'physics_wait', 1, 2, '123', 1),
                (40, 'physics_complete', 1, 3, '123', complete_thread),
                (50, 'physics_complete', 2, 3, '123', complete_thread),
                (60, 'physics_wait', 2, 2, '123', 1)]

    def test_physics_nested_completion_and_concurrent_publication(self):
        rows = self.physics_cycle()
        # Publication can reorder timestamps from different producers.
        result = physics_windows(list(reversed(rows)))['123']
        self.assertEqual(result['six_boundary_windows'], 1)
        self.assertEqual(result['completion_inside_wait_same_thread'], 1)
        result = physics_windows(self.physics_cycle(2))['123']
        self.assertEqual(result['completion_inside_wait_same_thread'], 0)
        self.assertEqual(result['completion_inside_wait_other_thread'], 1)
        rows = self.physics_cycle(2)
        rows[-1] = (45, *rows[-1][1:])
        result = physics_windows(rows)['123']
        self.assertEqual(result['wait_return_before_completion_return'], 1)
        self.assertEqual(result['completion_inside_wait'], 0)

    def test_physics_loss_ties_substeps_and_wrappers_do_not_false_pair(self):
        rows = self.physics_cycle()
        tied = rows.copy(); tied[-1] = (50, *rows[-1][1:])
        mismatched = rows.copy(); mismatched[-1] = (*rows[-1][:3], 999, *rows[-1][4:])
        for case in (rows[:-1], tied, mismatched, rows + [rows[3]]):
            with self.subTest(case=case):
                result = physics_windows(case)['123']
                self.assertEqual(result['six_boundary_windows'], 0)
                self.assertEqual(result['unclassified_windows'], 1)
        other = [(t, kind, edge, span, '456', thread) for t, kind, edge, span, obj, thread in rows]
        result = physics_windows(rows + other)
        self.assertEqual(set(result), {'123', '456'})
        self.assertTrue(all(r['six_boundary_windows'] == 1 for r in result.values()))
        orphan = (1, 'physics_wait', 2, 999, '123', 1)
        result = physics_windows([orphan] + rows)['123']
        self.assertEqual(result['boundaries_before_first_begin'], 1)

    def test_physics_partial_window_does_not_poison_later_submission(self):
        first = self.physics_cycle()[:-1]
        second = [(t + 100, kind, edge, span + 10, obj, thread)
                  for t, kind, edge, span, obj, thread in self.physics_cycle(2)]
        result = physics_windows(first + second)['123']
        self.assertEqual(result['windows'], 2)
        self.assertEqual(result['six_boundary_windows'], 1)
        self.assertEqual(result['boundary_shape_rejections'], 1)
        self.assertEqual(result['completion_inside_wait_other_thread'], 1)

    def test_resources_distinguish_reloaded_owners_and_raw_state(self):
        rows = fixture()
        state = dict(rows[-2], sequence=20, qpc=210, flags=7, detail=str((3 << 32) | 5))
        reloaded = dict(rows[-2], sequence=21, qpc=220, entity='8589934596', object='45')
        rows[-1:-1] = [state, reloaded, {'type': 'stats', 'elapsed_ms': 300, 'dropped': 0}]
        report = analyze(stream(rows))
        old, new = report['resource_observations']
        self.assertEqual(old['ids'], new['ids'])
        self.assertNotEqual(old['object'], new['object'])
        self.assertNotEqual(old['entity'], new['entity'])
        self.assertEqual((old['refs_min'], old['refs_max'], old['raw_states']), (3, 3, [5]))
        self.assertIsNone(new['refs_min'])
        self.assertEqual(report['resource_replacements_by_component'], {})
        self.assertEqual(report['last_stats_elapsed_seconds'], .3)
        rows[-3]['flags'] = 4
        with self.assertRaises(ValueError): analyze(stream(rows))

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
