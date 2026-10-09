"""Summarize bounded native capability observations without claiming API safety.

Reads a CRML session log. Game text, native pointers and unrecognized fields are
not copied into the report. A successful parse establishes observations only.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

MAX_BYTES = 64 * 1024 * 1024
MAX_LINE = 16384
MAX_RECORDS = 100000
PREFIX = re.compile(r'^(?:\[\+\d+ms\] \[host\] )?Capability (location|dialogue|restriction|story_reason|story_writer|structural_lifecycle|status_lifecycle|map_projection): (.*)$')
NAME = re.compile(r'^[a-z][a-z0-9_]{0,39}$')
TOTAL_FIELDS = ('calls', 'changes', 'samples', 'dropped', 'rejected', 'unreadable', 'unwinds',
                'in_flight', 'sample_budget_used', 'admitted', 'published', 'pending',
                'selected', 'caller_rejected', 'caller_unmapped', 'throttled', 'budget_exhausted',
                'flushes', 'teardowns', 'active', 'initializations', 'copies') + tuple(
    phase + '_' + field for phase in ('loading', 'save', 'restore') for field in
    ('calls', 'caller_rejected', 'sampled', 'unreadable', 'unwinds', 'matched', 'dropped', 'sample_budget_used'))


def integer(value):
    return type(value) is int and 0 <= value <= (1 << 64) - 1


def reason_mask(counts):
    if (not isinstance(counts, list) or len(counts) != 6
            or not all(integer(value) and value <= 0xffffffff for value in counts)):
        return None
    return sum(1 << i for i, value in enumerate(counts) if value)


def summarize(stream):
    result = {'schema': 1, 'qualification': 'no_observations', 'malformed_records': 0,
              'streams': {}, 'limits_reached': [],
              'meaning': 'Observed native calls only; not proof of persistent identity, presentation, completion or safe mutation.'}
    total_bytes = records = 0
    previous = {}
    while True:
        raw = stream.readline(MAX_LINE + 1)
        if not raw:
            break
        total_bytes += len(raw)
        if total_bytes > MAX_BYTES:
            result['limits_reached'].append('bytes')
            break
        if len(raw) > MAX_LINE:
            # Reject the entire oversized input rather than accidentally parsing
            # a marker from a later fragment of the same line.
            result['limits_reached'].append('line')
            break
        line = raw.decode('utf-8', errors='replace').rstrip('\r\n')
        match = PREFIX.fullmatch(line)
        if not match:
            continue
        if records >= MAX_RECORDS:
            result['limits_reached'].append('records')
            break
        records += 1
        name, payload = match.groups()
        try:
            row = json.loads(payload)
            if (not isinstance(row, dict) or type(row.get('schema')) is not int or row['schema'] != 1
                    or row.get('type') not in ('sample', 'totals')
                    or not isinstance(row.get('status'), str) or not NAME.fullmatch(row['status'])):
                raise ValueError('Invalid envelope')
            if row['type'] == 'sample':
                if not integer(row.get('sequence')) or not row['sequence'] or not integer(row.get('time_ms')):
                    raise ValueError('Invalid sequence/time')
                if 'phase' in row and (not isinstance(row['phase'], str) or not NAME.fullmatch(row['phase'])):
                    raise ValueError('Invalid phase')
        except (ValueError, TypeError, RecursionError):
            result['malformed_records'] += 1
            continue
        entry = result['streams'].setdefault(name, {
            'samples': 0, 'readable_samples': 0, 'statuses': Counter(), 'phases': Counter(),
            'non_increasing_sequences': 0, 'sequence_gaps': 0,
            'matched_save_samples': 0, 'nonempty_text_samples': 0, 'totals': {}, 'capture_closed': False})
        if row['type'] == 'totals':
            entry['totals_status'] = row['status']
            entry['capture_closed'] = row['status'] in ('complete', 'stopped') and row.get('pending', 0) == 0
            # Explicit aggregate allowlist keeps new raw fields out of reports.
            entry['totals'] = {key: row[key] for key in TOTAL_FIELDS if integer(row.get(key))}
            continue
        entry['samples'] += 1
        entry['readable_samples'] += row['status'] in ('ok', 'selected')
        # Names are validated and bounded, but retain at most 32 categories.
        for key, value in (('statuses', row['status']), ('phases', row.get('phase', 'unspecified'))):
            counts = entry[key]
            counts[value if value in counts or len(counts) < 32 else 'other'] += 1
        seq = row['sequence']
        if name in previous:
            entry['non_increasing_sequences'] += seq <= previous[name]
            entry['sequence_gaps'] += max(0, seq - previous[name] - 1)
        previous[name] = seq
        entry.setdefault('first_time_ms', row['time_ms'])
        entry['last_time_ms'] = row['time_ms']
        if name in ('restriction', 'story_reason', 'story_writer'):
            # Matching observer endpoints cover only captured structural hooks.
            # Do not export process-local entity handles or infer safe mutation.
            context_key = 'writer_player_context_valid' if name == 'story_writer' else 'player_context_valid'
            if row.get('structural_observed') is True and row.get(context_key) is True:
                entry['structural_observed_samples'] = entry.get('structural_observed_samples', 0) + 1
        if name == 'location' and row.get('phase') == 'save_coordinate' and row.get('matched') is True:
            entry['matched_save_samples'] += 1
        if name == 'location' and row.get('phase') == 'restore_coordinate':
            # Count entry-gate observations only. Returning from the native
            # callback does not establish transform application or save lineage.
            reason = row.get('restore_reason')
            if (row['status'] == 'ok' and row.get('restore_inputs_observed') is True
                    and row.get('returned') is True and isinstance(reason, str)
                    and reason in ('eligible', 'context_disabled', 'missing_saved_bundle',
                                   'missing_current_bundle', 'bundle_mismatch')):
                counts = entry.setdefault('restore_entry_reasons', Counter())
                counts[reason] += 1
        if name == 'dialogue' and integer(row.get('text_length')) and row['text_length'] > 0:
            entry['nonempty_text_samples'] += 1
        if name == 'map_projection':
            # A prediction alone is not a comparison with engine output.
            compared = row['status'] == 'ok' and all(row.get(key) is True for key in
                ('readable', 'transform', 'stable', 'predicted', 'compared'))
            if compared and type(row.get('matched')) is bool:
                entry['compared_samples'] = entry.get('compared_samples', 0) + 1
                key = 'matched_samples' if row['matched'] else 'mismatched_samples'
                entry[key] = entry.get(key, 0) + 1
            else:
                entry['uncompared_samples'] = entry.get('uncompared_samples', 0) + 1
        if name == 'restriction':
            # A pairwise conflict is not the final result of an ability request.
            # Only stable copies and literal booleans count as decision evidence.
            if row['status'] == 'ok' and row.get('stable') is True and type(row.get('conflict')) is bool:
                key = 'conflict_samples' if row['conflict'] else 'nonconflict_samples'
                entry[key] = entry.get(key, 0) + 1
                if row.get('player_context_valid') is True and row.get('player_matches') is True:
                    entry['player_matched_samples'] = entry.get('player_matched_samples', 0) + 1
                    if (row.get('applied_reasons_match') is True and row.get('structural_observed') is True
                            and type(row.get('player_mode')) is int and row['player_mode'] == 4
                            and type(row.get('player_flags')) is int and row['player_flags'] == 1):
                        # Retained observation agrees at this diagnostic read;
                        # this is not a permission decision or ability success.
                        entry['applied_reason_matches'] = entry.get('applied_reason_matches', 0) + 1
        if name == 'story_writer' and row['status'] == 'ok':
            # A returned bit update is not a reason snapshot, scheduler barrier,
            # or successful ability activation. Keep addresses and ordering
            # identifiers in the local capture, not the aggregate report.
            valid = (row.get('returned') is True and row.get('bytes_valid') is True
                     and type(row.get('enabled')) is bool
                     and all(integer(row.get(key)) and row[key] <= 255
                             for key in ('mask', 'before_flags', 'after_flags')))
            if valid:
                expected = (row['before_flags'] | row['mask']) if row['enabled'] else (
                    row['before_flags'] & (~row['mask'] & 255))
                key = 'matching_bit_updates' if row['after_flags'] == expected else 'mismatching_bit_updates'
                entry[key] = entry.get(key, 0) + 1
                if (row['after_flags'] == expected and row.get('writer_player_context_valid') is True
                        and row.get('writer_player_matches') is True):
                    entry['player_matching_bit_updates'] = entry.get('player_matching_bit_updates', 0) + 1
                masks = entry.setdefault('written_masks', Counter())
                # There are only 256 possible byte masks, even for hostile logs.
                masks[str(row['mask'])] += 1
                if row.get('manager_reasons_valid') is True:
                    reason_bits = reason_mask(row.get('manager_counts'))
                    # Same-call attribution only, never a retained reason lease
                    # or evidence that an ability was activated successfully.
                    same_call = (
                        row['mask'] == 1 and row['after_flags'] == expected
                        and reason_bits is not None and type(row.get('manager_active_mask')) is int
                        and row['manager_active_mask'] == reason_bits and row['enabled'] == bool(reason_bits)
                        and all(row.get(key) is True for key in (
                            'manager_enabled_matches', 'manager_world_matches', 'writer_context_unchanged',
                            'writer_player_context_valid', 'writer_player_matches', 'structural_observed'))
                        and all(integer(row.get(key)) and row[key] <= 255 for key in ('before_byte0', 'after_byte0'))
                        and row['before_byte0'] == row['after_byte0'])
                    key = 'same_call_manager_writes' if same_call else 'unqualified_manager_writes'
                    entry[key] = entry.get(key, 0) + 1
                    if same_call:
                        masks = entry.setdefault('same_call_reason_masks', Counter())
                        masks[str(reason_bits)] += 1
            else:
                entry['readable_samples'] -= 1
                entry['invalid_writer_samples'] = entry.get('invalid_writer_samples', 0) + 1
        if name == 'story_reason' and row['status'] == 'selected':
            counts = row.get('counts')
            valid = (isinstance(counts, list) and len(counts) == 6
                     and all(integer(value) and value <= 0xffffffff for value in counts))
            mask = sum(1 << i for i, value in enumerate(counts) if value) if valid else -1
            if (valid and type(row.get('active_mask')) is int and row['active_mask'] == mask
                    and type(row.get('enabled')) is bool and row['enabled'] == bool(mask)):
                # Requests only: no claim that the binding succeeded, that this
                # belongs to a sampled action, or that area overrides are safe.
                masks = entry.setdefault('requested_reason_masks', Counter())
                masks[str(mask)] += 1
                for key in ('world_matches', 'dictionary_matches', 'dictionary_values_match', 'player_context_valid'):
                    if row.get(key) is True:
                        entry[key + '_samples'] = entry.get(key + '_samples', 0) + 1
            else:
                entry['readable_samples'] -= 1
                entry['invalid_reason_samples'] = entry.get('invalid_reason_samples', 0) + 1
    # Samples remain useful even when the bounded observer stopped recording.
    # Surface that limitation separately from parser limits and read validity;
    # a zero-drop ring is not evidence of complete native-call coverage.
    for entry in result['streams'].values():
        totals = entry['totals']
        coverage = []
        if totals.get('budget_exhausted', 0):
            coverage.append('native_sample_budget_exhausted')
        if totals.get('throttled', 0):
            coverage.append('native_samples_throttled')
        if any(value for key, value in totals.items() if key == 'dropped' or key.endswith('_dropped')):
            coverage.append('native_samples_dropped')
        if entry['sequence_gaps'] or entry['non_increasing_sequences']:
            coverage.append('sample_sequence_discontinuity')
        if not entry['capture_closed']:
            coverage.append('capture_closure_unproven')
        entry['coverage_limits'] = coverage
    observed = sum(entry['samples'] for entry in result['streams'].values())
    if result['malformed_records'] or result['limits_reached']:
        result['qualification'] = 'incomplete_capture'
    elif observed and not any(entry['readable_samples'] for entry in result['streams'].values()):
        result['qualification'] = 'rejected_observations_only'
    elif observed:
        result['qualification'] = 'observations_present'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        with args.log.open('rb') as stream:
            result = summarize(stream)
        rendered = json.dumps(result, indent=2) + '\n'
        if args.output:
            # Never destroy an existing capture or earlier report.
            with args.output.open('x', encoding='utf-8') as output:
                output.write(rendered)
        else:
            print(rendered, end='')
        return 0 if result['qualification'] == 'observations_present' else 1
    except OSError as error:
        parser.exit(2, f'{error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
