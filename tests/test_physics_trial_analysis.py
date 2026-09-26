import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_physics_trial import analyze


class AnalysisTests(unittest.TestCase):
    def read(self, events):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'trial.jsonl'
            header = {'type': 'header', 'schema': 1, 'mode': 'native-physics-trial'}
            path.write_text('\n'.join(json.dumps(e) for e in [header, *events]) + '\n')
            return analyze(path)

    def event(self, sequence, action, result, before=0, after=0):
        return dict(type='event', sequence=sequence, action=action, result=result,
                    tick_ms=sequence, thread=1, selection=1, scene='1', entity='2', body='3', before=before, after=after)

    def test_apply_restore_evidence(self):
        report = self.read([self.event(1, 0, 0, .25, .5), self.event(2, 2, 256, .25, 8),
                            self.event(3, 1, 6), self.event(4, 2, 256, 8, .25), self.event(5, 1, 9)])
        self.assertTrue(report['selections']['1']['restoration_observed'])
        self.assertFalse(report['gameplay_effect_verified'])
        self.assertIsNone(report['terminal_reason'])

    def test_uncertain_write_not_proof(self):
        report = self.read([self.event(1, 0, 0, .25, .5), self.event(2, 2, 265, .25, 8), self.event(3, 1, 8)])
        self.assertFalse(report['selections']['1']['restoration_observed'])
        self.assertTrue(report['selections']['1']['writes'][0]['attempted'])

    def test_baseline_is_captured_at_application(self):
        report = self.read([self.event(1, 0, 0, .25, .5), self.event(2, 2, 256, .75, 8),
                            self.event(3, 1, 6), self.event(4, 2, 256, 8, .75), self.event(5, 1, 9)])
        self.assertTrue(report['selections']['1']['restoration_observed'])

    def test_application_alone_cannot_prove_restoration(self):
        events = [self.event(1, 0, 0, 8, .5), self.event(2, 2, 256, .75, 8), self.event(3, 1, 9)]
        self.assertFalse(self.read(events)['selections']['1']['restoration_observed'])
        events = [self.event(1, 0, 0, .25, .5), self.event(2, 2, 256, .25, 8),
                  self.event(3, 2, 256, 8, .25), self.event(4, 1, 9),
                  dict(type='stats', dropped=1, dispatches=50, context_rejections=0, retirements=0)]
        self.assertFalse(self.read(events)['selections']['1']['restoration_observed'])

    def test_identity_change_rejected(self):
        a, b = self.event(1, 0, 0), self.event(2, 1, 4)
        b['body'] = '4'
        with self.assertRaises(ValueError): self.read([a, b])

    def test_batched_selection_diagnostics(self):
        stats = dict(type='stats', dropped=0, dispatches=100, context_rejections=0, retirements=0,
                     selection_slots=16609, selection_scanned=16384)
        report = self.read([stats, self.event(1, 0, 6)])
        self.assertEqual(report['max_selection_slots'], 16609)
        self.assertEqual(report['max_selection_scanned'], 16384)
        self.assertEqual(report['selection_failures'], {'6': 1})
        self.assertIsNone(report['latest_search'])
        for bad in [-1, 2**20 + 1, True]:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                self.read([{**stats, 'selection_slots': bad}])

    def test_nearest_selection_diagnostics(self):
        stats = dict(type='stats', dropped=0, dispatches=100, context_rejections=0, retirements=0,
                     selection_candidates=3, nearest_distance=.5, second_distance=1)
        self.assertEqual(self.read([stats])['latest_search']['selection_candidates'], 3)
        for field, bad in [('selection_candidates', -1), ('nearest_distance', float('nan')),
                           ('second_distance', 3), ('second_distance', -.5)]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.read([{**stats, field: bad}])

    def test_invalid_records(self):
        for bad in [[], self.event(2, 0, 0), self.event(1, 8, 0), self.event(1, 1, 99), self.event(1, 2, 0, float('nan'))]:
            with self.subTest(bad=bad), self.assertRaises(ValueError): self.read([bad])


if __name__ == '__main__': unittest.main()
