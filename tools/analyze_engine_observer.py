"""Summarize bounded observe-only captures; never interprets observation as API safety."""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import struct

PHASES = ('movement', 'command_flush', 'script_fixed', 'renderer_sync',
          'physics_begin', 'physics_wait', 'physics_complete')
MAX_BYTES = 64 * 1024 * 1024
MAX_LINE = 8192
MAX_TRACKED = 16384
MAX_PHYSICS_BOUNDARIES = 500000
BODY_REJECTIONS = ('ok', 'arguments', 'bounds', 'generation', 'missing_actor',
                   'actor_type', 'actor_identity', 'representation', 'scalar', 'changed', 'memory')
LINK_REJECTIONS = ('ok', 'arguments', 'world', 'bounds', 'association', 'scene',
                   'instance', 'entity', 'changed', 'memory')


def physics_windows(boundaries):
    """Describe observations between submission entries, not authoritative frame IDs.

    Sort by QPC because concurrent publication order is not execution order. Only
    six-boundary windows with unique timestamps and matched spans are classified.
    Extra substeps, ties and missing records remain explicitly unclassified.
    """
    groups = defaultdict(list)
    for boundary in boundaries:
        groups[boundary[4]].append(boundary)
    reports = {}
    for obj, rows in groups.items():
        rows.sort()
        report = Counter(windows=0, six_boundary_windows=0, unclassified_windows=0,
                         boundary_shape_rejections=0, timestamp_tie_rejections=0,
                         span_identity_or_order_rejections=0,
                         boundaries_before_first_begin=0, completion_inside_wait=0,
                         completion_inside_wait_same_thread=0,
                         completion_inside_wait_other_thread=0,
                         wait_return_before_completion_return=0)
        window = []

        def finish():
            if not window:
                return
            report['windows'] += 1
            expected = {(name, edge) for name in PHASES if name.startswith('physics_') for edge in (1, 2)}
            by_edge = {(r[1], r[2]): r for r in window}
            valid = len(window) == 6 and set(by_edge) == expected
            reason = 'boundary_shape_rejections'
            if valid:
                valid = len({r[0] for r in window}) == 6
                reason = 'timestamp_tie_rejections'
            if valid:
                reason = 'span_identity_or_order_rejections'
                for name in ('physics_begin', 'physics_wait', 'physics_complete'):
                    enter, leave = by_edge[name, 1], by_edge[name, 2]
                    valid = valid and enter[3] == leave[3] and enter[5] == leave[5] and enter[0] < leave[0]
            if not valid:
                report['unclassified_windows'] += 1
                report[reason] += 1
                return
            report['six_boundary_windows'] += 1
            we, wr = by_edge['physics_wait', 1], by_edge['physics_wait', 2]
            ce, cr = by_edge['physics_complete', 1], by_edge['physics_complete', 2]
            if we[0] < ce[0] < cr[0] < wr[0]:
                report['completion_inside_wait'] += 1
                report['completion_inside_wait_same_thread' if we[5] == ce[5]
                       else 'completion_inside_wait_other_thread'] += 1
            if wr[0] < cr[0]:
                report['wait_return_before_completion_return'] += 1

        for row in rows:
            if row[1:3] == ('physics_begin', 1):
                finish()
                window = [row]
            elif window:
                window.append(row)
            else:
                report['boundaries_before_first_begin'] += 1
        finish()
        reports[obj] = dict(report)
    return reports


def require(value, message):
    if not value:
        raise ValueError(message)


def number(row, key, maximum=(1 << 64) - 1):
    value = row[key]
    require(type(value) is int and 0 <= value <= maximum, f'Invalid {key}')
    return value


def identifier(row, key):
    value = row[key]
    require(isinstance(value, str) and value.isascii() and value.isdecimal()
            and len(value) <= 20 and int(value) < 1 << 64, f'Invalid {key} identity')
    return value


def records(stream):
    total = 0
    while True:
        line = stream.readline(MAX_LINE + 1)
        if not line:
            return
        total += len(line)
        require(total <= MAX_BYTES and len(line) <= MAX_LINE, 'Capture exceeds size bounds')
        if not line.endswith(b'\n'):
            # A process exit can interrupt the final write. Preserve prior complete records.
            yield {'type': 'truncated_tail'}
            return
        row = json.loads(line)
        require(isinstance(row, dict), 'Expected a JSON object')
        yield row


def analyze(stream):
    header = None
    end = None
    sequence = 0
    dropped = 0
    notes = set()
    pending = {}
    phases = {name: {'enters': 0, 'returns': 0, 'threads': set(),
                     'paired': 0, 'duration_ticks_total': 0, 'duration_ticks_max': 0}
              for name in PHASES}
    players = set()
    transitions = []
    current_player = None
    last_player_time = None
    player_samples = 0
    resource_ids = set()
    resource_changes = Counter()
    resource_last = {}
    resource_samples = Counter()
    physics = defaultdict(lambda: defaultdict(set))
    physics_boundaries = []
    resource_observations = {}
    last_stats_elapsed = None
    body_observations = {}
    body_slots = {}
    body_replacements = 0
    body_scans = 0
    body_scan_rejections = Counter()
    entity_observations = {}
    entity_scans = 0
    entity_scan_rejections = Counter()
    orphan_returns = 0
    first_time = last_time = None
    for row in records(stream):
        kind = row['type']
        require(end is None, 'Records after terminal capture marker')
        if kind == 'truncated_tail':
            notes.add('Final partial line ignored; capture tail is incomplete.')
            break
        if header is None:
            require(kind == 'header' and row.get('schema') in (1, 2, 3) and row.get('mode') == 'observe-only'
                    and row.get('mods_suspended') is True, 'Unsupported or non-observational capture')
            require(number(row, 'qpc_frequency') > 0, 'Missing clock frequency')
            number(row, 'qpc_origin')
            require(isinstance(row.get('sha256'), str) and len(row['sha256']) == 64, 'Invalid fingerprint')
            expected_phases = PHASES + (('post_physics',) if row['schema'] >= 2 else ())
            require({h['name'] for h in row['hooks']} == set(expected_phases), 'Incomplete hook manifest')
            if row['schema'] >= 2:
                require(isinstance(row.get('physx_sha256'), str) and len(row['physx_sha256']) == 64, 'Missing physics backend fingerprint')
                phases['post_physics'] = {'enters': 0, 'returns': 0, 'threads': set(),
                                          'paired': 0, 'duration_ticks_total': 0, 'duration_ticks_max': 0}
            header = row
            continue
        if kind in ('stats', 'end'):
            dropped = max(dropped, number(row, 'dropped'))
            if kind == 'stats' and 'elapsed_ms' in row:
                last_stats_elapsed = number(row, 'elapsed_ms')
            if kind == 'end':
                require(isinstance(row.get('reason'), str), 'Invalid end reason')
                end = row
            continue
        require(kind == 'event', 'Unknown record type')
        seq, time, thread = number(row, 'sequence'), number(row, 'qpc'), number(row, 'thread', (1 << 32) - 1)
        require(seq > sequence, 'Non-increasing publication sequence')
        if sequence and seq != sequence + 1:
            notes.add('Publication sequence has gaps.')
        sequence = seq
        first_time = time if first_time is None else min(first_time, time)
        last_time = time if last_time is None else max(last_time, time)
        name, edge, span = row['kind'], number(row, 'edge', 2), number(row, 'span')
        obj, entity = identifier(row, 'object'), identifier(row, 'entity')
        value, detail, flags = identifier(row, 'value'), identifier(row, 'detail'), number(row, 'flags', (1 << 32) - 1)
        if name in phases:
            require(edge in (1, 2) and span > 0, 'Invalid phase boundary')
            phase = phases[name]
            phase['threads'].add(thread)
            if edge == 1:
                require(span not in pending, 'Duplicate open span')
                require(len(pending) < MAX_TRACKED, 'Too many incomplete spans')
                pending[span] = (name, time, thread, obj)
                phase['enters'] += 1
            else:
                phase['returns'] += 1
                begin = pending.pop(span, None)
                if begin is None:
                    orphan_returns += 1
                else:
                    require((begin[0], begin[2], begin[3]) == (name, thread, obj), 'Mismatched span identity')
                    require(time >= begin[1], 'Negative span duration')
                    elapsed = time - begin[1]
                    phase['paired'] += 1
                    phase['duration_ticks_total'] += elapsed
                    phase['duration_ticks_max'] = max(phase['duration_ticks_max'], elapsed)
            if name.startswith('physics_') and obj != '0':
                require(obj in physics or len(physics) < MAX_TRACKED, 'Too many physics identities')
                physics[obj][name].add(thread)
                require(len(physics_boundaries) < MAX_PHYSICS_BOUNDARIES, 'Too many physics boundaries')
                physics_boundaries.append((time, name, edge, span, obj, thread))
        elif name == 'player':
            require(edge == 0 and flags & 1 and entity != '0' and obj != '0', 'Invalid player sample')
            player_samples += 1
            key = (obj, entity)
            require(key in players or len(players) < MAX_TRACKED, 'Too many player identities')
            players.add(key)
            if last_player_time is not None and time - last_player_time > header['qpc_frequency'] * 2:
                notes.add('Player sampling has gaps over two seconds; pause/loading/absence are not distinguished.')
            if key != current_player:
                require(len(transitions) < MAX_TRACKED, 'Too many player transitions')
                transitions.append({'qpc': time, 'world': obj, 'entity': entity})
                current_player = key
            last_player_time = time
        elif name == 'resource':
            require(edge == 0 and flags <= 7, 'Invalid resource sample')
            resource_samples[value] += 1
            require(not flags & 4 or flags & 2, 'Resource state requires readable resource')
            if flags & 2:
                require(flags & 1 and obj != '0', 'Readable resource requires a component and identity')
                key = (str(span), entity, value, obj)
                require(key in resource_observations or len(resource_observations) < MAX_TRACKED,
                        'Too many resource observations')
                observed = resource_observations.setdefault(key, {
                    'world': str(span), 'entity': entity, 'component': value, 'object': obj,
                    'id_records': 0, 'state_records': 0, 'ids': set(), 'raw_states': set(),
                    'refs_min': None, 'refs_max': None})
                if flags & 4:
                    refs, state = int(detail) >> 32, int(detail) & 0xffffffff
                    observed['state_records'] += 1
                    observed['raw_states'].add(state)
                    require(len(observed['raw_states']) <= MAX_TRACKED, 'Too many resource states')
                    observed['refs_min'] = refs if observed['refs_min'] is None else min(refs, observed['refs_min'])
                    observed['refs_max'] = refs if observed['refs_max'] is None else max(refs, observed['refs_max'])
                else:
                    observed['id_records'] += 1
                    observed['ids'].add(detail)
            if flags & 2 and not flags & 4:
                require(flags & 1 and obj != '0', 'Readable resource requires a component and identity')
                resource_ids.add((obj, detail))
                require(len(resource_ids) <= MAX_TRACKED, 'Too many resource identities')
                key = (span, entity, value)
                require(key in resource_last or len(resource_last) < MAX_TRACKED, 'Too many resource owners')
                identity = (obj, detail)
                if key in resource_last and resource_last[key] != identity:
                    resource_changes[value] += 1
                resource_last[key] = identity
        elif name == 'body' and header['schema'] >= 2:
            require(edge == 0 and span > 0 and flags in (1, 3) and obj != '0' and detail != '0', 'Invalid body sample')
            linear, angular = struct.unpack('<ff', int(value).to_bytes(8, 'little'))
            require(all(math.isfinite(v) and v >= 0 for v in (linear, angular)), 'Invalid damping values')
            key = (detail, entity, obj)
            require(key in body_observations or len(body_observations) < MAX_TRACKED, 'Too many body observations')
            observed = body_observations.setdefault(key, {'scene': detail, 'handle': entity, 'actor': obj,
                'samples': 0, 'alternate_samples': 0, 'linear_min': linear, 'linear_max': linear,
                'angular_min': angular, 'angular_max': angular})
            observed['samples'] += 1
            observed['alternate_samples'] += flags == 3
            for prefix, v in (('linear', linear), ('angular', angular)):
                observed[prefix + '_min'] = min(observed[prefix + '_min'], v)
                observed[prefix + '_max'] = max(observed[prefix + '_max'], v)
            slot = (detail, int(entity) & 0xffffffff)
            current = (entity, obj)
            if slot in body_slots and body_slots[slot] != current:
                body_replacements += 1
            body_slots[slot] = current
        elif name == 'body_scan' and header['schema'] >= 2:
            require(edge == 0 and span > 0 and obj != '0' and int(value) <= 128
                    and int(entity) <= min(8, int(value)) and int(detail) <= 1 << 20
                    and flags < 1 << len(BODY_REJECTIONS) and not flags & 1, 'Invalid body scan')
            body_scans += 1
            for i, reason in enumerate(BODY_REJECTIONS):
                if flags & (1 << i):
                    body_scan_rejections[reason] += 1
        elif name == 'entity_body' and header['schema'] == 3:
            require(edge == 0 and span > 0 and flags == 1 and obj != '0' and detail != '0'
                    and entity not in ('0', str((1 << 64) - 1))
                    and int(value) & 0xffffffff < 1 << 20, 'Invalid body/entity association')
            key = (str(span), obj, value, detail, entity)
            require(key in entity_observations or len(entity_observations) < MAX_TRACKED,
                    'Too many body/entity associations')
            observed = entity_observations.setdefault(key, {'world': str(span), 'scene': obj,
                'handle': value, 'actor': detail, 'entity': entity, 'samples': 0,
                'first_qpc': time, 'last_qpc': time})
            observed['samples'] += 1
            observed['first_qpc'] = min(observed['first_qpc'], time)
            observed['last_qpc'] = max(observed['last_qpc'], time)
        elif name == 'entity_scan' and header['schema'] == 3:
            mask = ((1 << len(BODY_REJECTIONS)) - 2) | (((1 << len(LINK_REJECTIONS)) - 2) << 16)
            require(edge == 0 and span > 0 and int(value) <= 64 and int(entity) <= min(4, int(value))
                    and int(detail) <= 1 << 20 and not flags & ~mask
                    and (obj != '0' or (value == entity == detail == '0' and flags == 1 << 18)),
                    'Invalid entity scan')
            entity_scans += 1
            for offset, prefix, reasons in ((0, 'body.', BODY_REJECTIONS), (16, 'link.', LINK_REJECTIONS)):
                for i, reason in enumerate(reasons):
                    if flags & (1 << (offset + i)):
                        entity_scan_rejections[prefix + reason] += 1
        else:
            raise ValueError('Unknown event kind')
    require(header is not None, 'No capture header')
    missing = [name for name, info in phases.items() if not info['paired']]
    if dropped:
        notes.add('Producer contention or overflow dropped records; absence and ordering conclusions are incomplete.')
    if end is None:
        notes.add('No terminal marker; normal process exit can also leave this capture incomplete.')
    if pending or orphan_returns:
        notes.add('Some phase spans are unpaired; capture boundaries, lost records or unwinding may explain them.')
    notes.add('Pointer-derived identities are session-local and may be reused; observations do not prove destruction or safe mutation.')
    windows = physics_windows(physics_boundaries)
    if windows:
        notes.add('Physics windows are QPC-grouped observations between submission entries, not engine frame IDs or proof of an exclusive mutation window; ties, losses and extra substeps limit classification.')
    for observed in resource_observations.values():
        observed['ids'] = sorted(observed['ids'])
        observed['raw_states'] = sorted(observed['raw_states'])
    for info in phases.values():
        info['threads'] = sorted(info['threads'])
        paired = info['paired']
        info['mean_ms'] = info.pop('duration_ticks_total') * 1000 / header['qpc_frequency'] / paired if paired else None
        info['max_ms'] = info.pop('duration_ticks_max') * 1000 / header['qpc_frequency'] if paired else None
    failed = end is not None and end['reason'] in ('hook_enable_failed', 'io_error')
    if header['schema'] >= 2 and not body_observations:
        failed = True
        notes.add('No accepted dynamic-body snapshots; body observation remains unvalidated.')
    if header['schema'] == 3 and not entity_observations:
        failed = True
        notes.add('No accepted body/entity associations; prop identity remains unvalidated.')
    if entity_observations:
        notes.add('Body/entity associations are guarded same-callback observations, not retained handles, selectable props, or scheduler exclusion.')
    return {'schema': 1, 'sha256': header['sha256'], 'status': 'incomplete' if failed or missing or not player_samples or not resource_ids or dropped else 'ready_for_manual_review',
            'not_a_gameplay_or_api_validation': True,
            'duration_seconds': (last_time - first_time) / header['qpc_frequency'] if first_time is not None else 0,
            'last_stats_elapsed_seconds': last_stats_elapsed / 1000 if last_stats_elapsed is not None else None,
            'missing_paired_phases': missing, 'dropped': dropped, 'open_spans': len(pending), 'orphan_returns': orphan_returns,
            'open_spans_by_phase': dict(Counter(item[0] for item in pending.values())),
            'end_reason': end['reason'] if end else None, 'phases': phases,
            'player_samples': player_samples, 'player_identities': len(players), 'player_transitions': transitions,
            'resource_identities': len(resource_ids), 'resource_records_by_component': dict(resource_samples),
            'resource_replacements_by_component': dict(resource_changes),
            'resource_observations': list(resource_observations.values()),
            'physics_wrapper_threads': {obj: {kind: sorted(threads) for kind, threads in groups.items()} for obj, groups in physics.items()},
            'physics_windows': windows,
            'body_probe': {'available': header['schema'] >= 2, 'scans': body_scans,
                           'status': 'observed' if body_observations else 'no_accepted_samples' if header['schema'] >= 2 else 'not_recorded',
                           'observed_slot_replacements': body_replacements,
                           'scans_with_rejection': dict(body_scan_rejections),
                           'bodies': list(body_observations.values()),
                           'ownership_or_mutation_verified': False},
            'entity_probe': {'available': header['schema'] == 3, 'scans': entity_scans,
                             'status': 'observed' if entity_observations else 'no_accepted_samples' if header['schema'] == 3 else 'not_recorded',
                             'scans_with_rejection': dict(entity_scan_rejections),
                             'associations': list(entity_observations.values()),
                             'ownership_or_mutation_verified': False},
            'notes': sorted(notes)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        require(not args.output or args.output.resolve() != args.capture.resolve(), 'Output would overwrite capture')
        with args.capture.open('rb') as stream:
            report = analyze(stream)
        text = json.dumps(report, indent=2) + '\n'
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text, encoding='utf-8')
            print(f"{report['status']}; report: {args.output}")
        else:
            print(text, end='')
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Capture analysis failed: {error}\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
