import io
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import analyze_capability_capture as capture


def record(topic='location', **fields):
    row = dict(schema=1, type='sample', status='ok', sequence=1, time_ms=100)
    row.update(fields)
    return f'Capability {topic}: {json.dumps(row)}\n'.encode()


class CaptureTests(unittest.TestCase):
    def test_same_call_manager_write_requires_complete_matching_evidence(self):
        fields = dict(returned=True, bytes_valid=True, enabled=True, mask=1,
                      before_flags=4, after_flags=5, before_byte0=16, after_byte0=16,
                      manager_reasons_valid=True, manager_counts=[2, 0, 1, 0, 0, 0],
                      manager_active_mask=5, manager_enabled_matches=True, manager_world_matches=True,
                      writer_context_unchanged=True, writer_player_context_valid=True,
                      writer_player_matches=True, structural_observed=True)
        report = capture.summarize(io.BytesIO(record('story_writer', **fields)))
        entry = report['streams']['story_writer']
        self.assertEqual(entry['same_call_manager_writes'], 1)
        self.assertEqual(entry['same_call_reason_masks'], {'5': 1})
        # Retain concurrent conversation in the mask; never collapse it to area-only.
        for key, value in [('manager_counts', [2, 0, True, 0, 0, 0]),
                           ('manager_active_mask', 1), ('manager_enabled_matches', False),
                           ('manager_world_matches', 1), ('writer_context_unchanged', False),
                           ('structural_observed', False), ('after_byte0', 4), ('mask', 4),
                           ('after_flags', 4), ('writer_player_matches', False)]:
            with self.subTest(key=key):
                bad = dict(fields, **{key: value})
                entry = capture.summarize(io.BytesIO(record('story_writer', **bad)))['streams']['story_writer']
                self.assertNotIn('same_call_manager_writes', entry)
                self.assertEqual(entry['unqualified_manager_writes'], 1)
        fields['returned'] = False
        entry = capture.summarize(io.BytesIO(record('story_writer', **fields)))['streams']['story_writer']
        self.assertNotIn('same_call_manager_writes', entry)

    def test_reserved_writer_allowance_reports_sampling_gaps_without_exhaustion(self):
        data = record('story_writer', type='totals', status='complete', pending=0,
                      calls=300, samples=8, throttled=292, budget_exhausted=0)
        entry = capture.summarize(io.BytesIO(data))['streams']['story_writer']
        self.assertEqual(entry['coverage_limits'], ['native_samples_throttled'])
        self.assertEqual(entry['totals']['throttled'], 292)

    def test_structural_observations_are_bounded_aggregates_not_lifetime_proof(self):
        data = record('structural_lifecycle', type='totals', status='stopped', pending=0,
                      flushes=12, teardowns=2, active=0, unwinds=1, sequence=30, world='private')
        data += record('restriction', player_context_valid=True, structural_observed=True,
                       player_entity=123456789, structural_sequence=20)
        data += record('restriction', sequence=2, player_context_valid=True, structural_observed=1)
        data += record('restriction', sequence=3, player_context_valid=False, structural_observed=True)
        report = capture.summarize(io.BytesIO(data))
        self.assertEqual(report['malformed_records'], 0)
        self.assertEqual(report['streams']['structural_lifecycle']['totals'],
                         dict(pending=0, flushes=12, teardowns=2, active=0, unwinds=1))
        self.assertTrue(report['streams']['structural_lifecycle']['capture_closed'])
        self.assertEqual(report['streams']['restriction']['structural_observed_samples'], 1)
        for hidden in ('player_entity', 'structural_sequence', '123456789', 'private'):
            self.assertNotIn(hidden, json.dumps(report))

    def test_structural_drain_is_not_a_completed_capture(self):
        data = record('structural_lifecycle', type='totals', status='draining', pending=2, active=2)
        entry = capture.summarize(io.BytesIO(data))['streams']['structural_lifecycle']
        self.assertFalse(entry['capture_closed'])
        self.assertEqual(entry['totals']['pending'], 2)

    def test_restore_gates_are_not_restoration_or_identity_proof(self):
        data = b''.join(record(phase='restore_coordinate', sequence=i+1,
            restore_inputs_observed=True, returned=True, restore_reason=reason,
            saved_bundle='private identifier') for i, reason in enumerate((
                'eligible', 'context_disabled', 'missing_saved_bundle',
                'missing_current_bundle', 'bundle_mismatch')))
        data += record(phase='restore_coordinate', sequence=6, status='unwind',
            restore_inputs_observed=True, returned=False, restore_reason='eligible')
        data += record(phase='restore_coordinate', sequence=7,
            restore_inputs_observed=1, returned=1, restore_reason='eligible')
        data += record(phase='restore_coordinate', sequence=8,
            restore_inputs_observed=True, returned=True, restore_reason=['eligible'])
        data += record(type='totals', status='complete', pending=0,
            restore_calls=8, restore_unwinds=1, restore_dropped=0)
        report = capture.summarize(io.BytesIO(data))
        entry = report['streams']['location']
        self.assertEqual(sum(entry['restore_entry_reasons'].values()), 5)
        self.assertEqual(entry['restore_entry_reasons']['eligible'], 1)
        self.assertEqual(entry['matched_save_samples'], 0)
        self.assertEqual(entry['totals']['restore_unwinds'], 1)
        self.assertNotIn('private identifier', json.dumps(report))
        self.assertIn('not proof of persistent identity', report['meaning'])

    def test_map_predictions_require_native_comparison(self):
        fields = dict(readable=True, transform=True, stable=True, predicted=True, compared=True, matched=True)
        data = record('map_projection', **fields)
        data += record('map_projection', sequence=2, **dict(fields, matched=False))
        data += record('map_projection', sequence=3, **dict(fields, compared=False, max_error=0))
        data += record('map_projection', sequence=4, **dict(fields, stable=1))
        data += record('map_projection', sequence=5, status='unreadable', **fields)
        entry = capture.summarize(io.BytesIO(data))['streams']['map_projection']
        self.assertEqual(entry['compared_samples'], 2)
        self.assertEqual(entry['matched_samples'], 1)
        self.assertEqual(entry['mismatched_samples'], 1)
        self.assertEqual(entry['uncompared_samples'], 3)
        self.assertNotIn('max_error', entry)

    def test_reason_requests_keep_concurrent_locks_and_reject_bad_values(self):
        data = record('story_reason', status='selected', counts=[2,0,1,0,0,0], active_mask=5, enabled=True,
                      dictionary_values_match=True, world_matches=1, dictionary_matches=True)
        data += record('story_reason', status='selected', sequence=2, counts=[0]*6, active_mask=0, enabled=False)
        for seq, values in enumerate(([True,0,0,0,0,0], [1.5,0,0,0,0,0], [1]*7, [-1]*6), 3):
            data += record('story_reason', status='selected', sequence=seq, counts=values, active_mask=1, enabled=True)
        data += record('story_reason', status='selected', sequence=7, counts=[0]*6, active_mask=1, enabled=True)
        report = capture.summarize(io.BytesIO(data))
        entry = report['streams']['story_reason']
        self.assertEqual(entry['requested_reason_masks'], {'5':1, '0':1})
        self.assertEqual(entry['readable_samples'], 2)
        self.assertEqual(entry['invalid_reason_samples'], 5)
        self.assertEqual(entry['dictionary_values_match_samples'], 1)
        self.assertEqual(entry['dictionary_matches_samples'], 1)
        self.assertNotIn('world_matches_samples', entry)
        self.assertNotIn('applied', entry)

    def test_writer_results_are_bit_updates_not_permission(self):
        fields = dict(returned=True, bytes_valid=True, enabled=True, mask=1,
                      before_flags=4, after_flags=5, occurrence=912, caller_rva='private', thread=22,
                      writer_player_context_valid=True, writer_player_matches=True)
        data = record('story_writer', **fields)
        data += record('story_writer', sequence=2, **dict(fields, enabled=False, before_flags=7,
                                                        after_flags=6, writer_player_matches=1))
        data += record('story_writer', sequence=3, **dict(fields, after_flags=1))
        data += record('story_writer', sequence=4, status='unwind', **dict(fields, returned=False))
        report = capture.summarize(io.BytesIO(data))
        entry = report['streams']['story_writer']
        self.assertEqual(entry['matching_bit_updates'], 2)
        self.assertEqual(entry['mismatching_bit_updates'], 1)
        self.assertEqual(entry['player_matching_bit_updates'], 1)
        self.assertEqual(entry['written_masks'], {'1': 3})
        for hidden in ('caller_rva', 'occurrence', 'thread', 'ability_success', 'applied', 'private'):
            self.assertNotIn(hidden, json.dumps(report))

    def test_writer_evidence_requires_literal_types_and_readable_return(self):
        fields = dict(returned=True, bytes_valid=True, enabled=True, mask=1, before_flags=0, after_flags=1)
        invalid = ({'returned': 1}, {'returned': False}, {'bytes_valid': 1}, {'enabled': 1},
                   {'mask': True}, {'mask': 256}, {'before_flags': -1}, {'after_flags': 1.0})
        data = b''.join(record('story_writer', sequence=i + 1, **dict(fields, **change))
                        for i, change in enumerate(invalid))
        report = capture.summarize(io.BytesIO(data))
        self.assertEqual(report['qualification'], 'rejected_observations_only')
        self.assertEqual(report['streams']['story_writer']['invalid_writer_samples'], len(invalid))
        self.assertNotIn('written_masks', report['streams']['story_writer'])

    def test_restriction_decisions_are_not_ability_success(self):
        data = record('restriction', stable=True, conflict=True, left_id='67108864', pointer=1234)
        data += record('restriction', sequence=2, stable=True, conflict=False)
        data += record('restriction', sequence=3, stable=False, conflict=True)
        data += record('restriction', sequence=4, stable=True, conflict=1)
        data += record('restriction', sequence=5, status='unwind', stable=True, conflict=True)
        data += record('restriction', type='totals', status='stopped', pending=0, selected=5, caller_rejected=3)
        report = capture.summarize(io.BytesIO(data))
        entry = report['streams']['restriction']
        self.assertEqual(entry['conflict_samples'], 1)
        self.assertEqual(entry['nonconflict_samples'], 1)
        self.assertEqual(entry['totals']['caller_rejected'], 3)
        self.assertTrue(entry['capture_closed'])
        self.assertNotIn('left_id', json.dumps(report))
        self.assertNotIn('pointer', json.dumps(report))
        self.assertNotIn('ability_success', entry)

    def test_both_sources_and_redaction(self):
        data = record(phase='save_coordinate', matched=True, secret='game text') + record(
            'dialogue', text_length=42, text='not for report', text_hash='123')
        report = capture.summarize(io.BytesIO(data))
        self.assertEqual(report['qualification'], 'observations_present')
        self.assertEqual(report['streams']['location']['matched_save_samples'], 1)
        self.assertEqual(report['streams']['dialogue']['nonempty_text_samples'], 1)
        self.assertNotIn('game text', json.dumps(report))
        self.assertNotIn('not for report', json.dumps(report))

    def test_applied_reason_match_is_qualified_and_not_permission(self):
        fields = dict(stable=True, conflict=True, player_context_valid=True, player_matches=True,
                      structural_observed=True, player_mode=4, player_flags=1, applied_reasons_match=True)
        changes = ({}, {'applied_reasons_match': 1}, {'player_flags': True}, {'player_flags': 3},
                   {'player_mode': 8}, {'player_matches': False}, {'structural_observed': False}, {'stable': False})
        data = b''.join(record('restriction', sequence=i+1, **dict(fields, **change))
                        for i, change in enumerate(changes))
        result = capture.summarize(io.BytesIO(data))
        self.assertEqual(result['streams']['restriction']['applied_reason_matches'], 1)
        self.assertNotIn('permission', result['streams']['restriction'])

    def test_status_lifecycle_has_aggregate_only_coverage(self):
        data = record('status_lifecycle', type='totals', status='active', available=True,
                      pending=2, initializations=3, copies=4, active=2, sequence=456, pointer=1234)
        entry = capture.summarize(io.BytesIO(data))['streams']['status_lifecycle']
        self.assertFalse(entry['capture_closed'])
        self.assertEqual(entry['totals'], {'pending': 2, 'active': 2, 'initializations': 3, 'copies': 4})
        self.assertNotIn('sequence', entry)

    def test_native_budget_and_drops_are_distinct_from_parse_limits(self):
        fields = dict(returned=True, bytes_valid=True, enabled=True, mask=1, before_flags=0, after_flags=1)
        data = record('story_writer', **fields)
        data += record('story_writer', type='totals', status='active', calls=1190,
                       samples=34, dropped=0, budget_exhausted=1156)
        report = capture.summarize(io.BytesIO(data))
        self.assertEqual(report['qualification'], 'observations_present')
        self.assertEqual(report['limits_reached'], [])
        self.assertEqual(report['streams']['story_writer']['coverage_limits'],
                         ['native_sample_budget_exhausted', 'capture_closure_unproven'])
        data += record('location', phase='restore_coordinate')
        data += record('location', type='totals', status='complete', pending=0, restore_dropped=2)
        entry = capture.summarize(io.BytesIO(data))['streams']['location']
        self.assertEqual(entry['coverage_limits'], ['native_samples_dropped'])

    def test_coverage_limits_do_not_treat_bool_counters_as_evidence(self):
        data = record() + record(type='totals', status='stopped', pending=0,
                                dropped=True, budget_exhausted=True)
        self.assertEqual(capture.summarize(io.BytesIO(data))['streams']['location']['coverage_limits'], [])
        data = record(sequence=1) + record(sequence=3)
        entry = capture.summarize(io.BytesIO(data))['streams']['location']
        self.assertIn('sample_sequence_discontinuity', entry['coverage_limits'])

    def test_no_events_never_passes(self):
        self.assertEqual(capture.summarize(io.BytesIO(b'ordinary log\n'))['qualification'], 'no_observations')

    def test_guest_prefix_cannot_fake_native_record(self):
        data = b'[+1ms] [fake] [info] ' + record()
        self.assertFalse(capture.summarize(io.BytesIO(data))['streams'])
        data = b'[+1ms] [host] ' + record()
        self.assertEqual(capture.summarize(io.BytesIO(data))['streams']['location']['samples'], 1)

    def test_malformed_and_non_integer_metadata(self):
        data = record(sequence=True) + record(time_ms=-1) + record(schema=True)
        data += b'Capability location: {bad json}\n'
        report = capture.summarize(io.BytesIO(data))
        self.assertEqual(report['malformed_records'], 4)
        self.assertEqual(report['qualification'], 'incomplete_capture')

    def test_sequence_observation_without_completion_claim(self):
        report = capture.summarize(io.BytesIO(record(sequence=2) + record(sequence=5) + record(sequence=4)))
        entry = report['streams']['location']
        self.assertEqual(entry['sequence_gaps'], 2)
        self.assertEqual(entry['non_increasing_sequences'], 1)
        self.assertIn('not proof', report['meaning'])

    def test_totals_allowlist_and_no_sample_falsepass(self):
        report = capture.summarize(io.BytesIO(record(type='totals', calls=7, dropped=2, pointer=12345)))
        self.assertEqual(report['streams']['location']['totals'], {'calls': 7, 'dropped': 2})
        self.assertEqual(report['qualification'], 'no_observations')

    def test_native_phase_totals_and_incomplete_closure(self):
        data = record(phase='loading_request') + record(type='totals', status='active', loading_calls=4, save_dropped=2)
        entry = capture.summarize(io.BytesIO(data))['streams']['location']
        self.assertFalse(entry['capture_closed'])
        self.assertEqual(entry['totals'], {'loading_calls': 4, 'save_dropped': 2})
        data += record(type='totals', status='complete', pending=0)
        self.assertTrue(capture.summarize(io.BytesIO(data))['streams']['location']['capture_closed'])
        stopped = record('dialogue', type='totals', status='stopped', pending=0)
        self.assertTrue(capture.summarize(io.BytesIO(stopped))['streams']['dialogue']['capture_closed'])

    def test_rejected_reads_are_not_usable_evidence(self):
        report = capture.summarize(io.BytesIO(record(status='memory')))
        self.assertEqual(report['qualification'], 'rejected_observations_only')

    def test_oversized_line_stops_before_fragments(self):
        report = capture.summarize(io.BytesIO(b'x' * (capture.MAX_LINE + 1) + record()))
        self.assertEqual(report['limits_reached'], ['line'])
        self.assertFalse(report['streams'])

    def test_distinct_statuses_are_bounded(self):
        data = b''.join(record(status=f's{i}', sequence=i+1) for i in range(100))
        counts = capture.summarize(io.BytesIO(data))['streams']['location']['statuses']
        self.assertLessEqual(len(counts), 33)
        self.assertEqual(sum(counts.values()), 100)


if __name__ == '__main__':
    unittest.main()
