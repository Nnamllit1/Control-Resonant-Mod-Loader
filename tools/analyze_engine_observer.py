"""Summarize bounded observe-only captures; never interprets observation as API safety."""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import struct

PHASES = ('movement', 'command_flush', 'script_fixed', 'renderer_sync',
          'physics_begin', 'physics_wait', 'physics_complete')
CAMERA_PHASES = ('camera_update', 'camera_switch', 'camera_select', 'camera_init', 'camera_remove')
CAMERA_READS = ('ok', 'arguments', 'environment', 'selector', 'changed', 'memory')
MAX_BYTES = 64 * 1024 * 1024
MAX_LINE = 8192
MAX_TRACKED = 16384
MAX_PHYSICS_BOUNDARIES = 500000
BODY_REJECTIONS = ('ok', 'arguments', 'bounds', 'generation', 'missing_actor',
                   'actor_type', 'actor_identity', 'representation', 'scalar', 'changed', 'memory')
LINK_REJECTIONS = ('ok', 'arguments', 'world', 'bounds', 'association', 'scene',
                   'instance', 'entity', 'changed', 'memory')
ACCESS_REJECTIONS = ('ok', 'arguments', 'snapshot', 'slot', 'scalar', 'changed', 'mismatch', 'memory')


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
        line = stream.readline(MAX_LINE + 2)
        if not line:
            return
        # Older Windows recorders counted LF bytes before text-mode CRLF expansion.
        # Normalize only record terminators, preserving the same logical size bound.
        # The physical input is consequently bounded by twice MAX_BYTES at most.
        size = len(line) - int(line.endswith(b'\r\n'))
        total += size
        require(total <= MAX_BYTES and size <= MAX_LINE, 'Capture exceeds size bounds')
        if not line.endswith(b'\n'):
            # A process exit can interrupt the final write. Preserve prior complete records.
            yield {'type': 'truncated_tail'}
            return
        row = json.loads(line)
        require(isinstance(row, dict), 'Expected a JSON object')
        yield row


def analyze(stream):
    header = None
    camera_only = False
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
    accessor_observations = {}
    accessor_scans = 0
    accessor_scan_rejections = Counter()
    orphan_returns = 0
    camera_reads, camera_modes, camera_slots = Counter(), Counter(), Counter()
    camera_before = {}
    camera_current = {}
    camera_pairs, camera_changes = Counter(), []
    first_time = last_time = None
    for row in records(stream):
        kind = row['type']
        require(end is None, 'Records after terminal capture marker')
        if kind == 'truncated_tail':
            notes.add('Final partial line ignored; capture tail is incomplete.')
            break
        if header is None:
            camera_only = row.get('mode') == 'camera-observation'
            require(kind == 'header' and row.get('schema') in (1, 2, 3, 4, 5) and
                    ((camera_only and row['schema'] == 5 and row.get('mods_suspended') is False) or
                     (row.get('mode') == 'observe-only' and row.get('mods_suspended') is True)),
                    'Unsupported or non-observational capture')
            require(number(row, 'qpc_frequency') > 0, 'Missing clock frequency')
            number(row, 'qpc_origin')
            require(isinstance(row.get('sha256'), str) and len(row['sha256']) == 64, 'Invalid fingerprint')
            expected_phases = PHASES + (('post_physics',) if row['schema'] >= 2 else ())
            if row['schema'] >= 5:
                expected_phases += CAMERA_PHASES
                for phase in CAMERA_PHASES:
                    phases[phase] = {'enters': 0, 'returns': 0, 'threads': set(),
                                     'paired': 0, 'duration_ticks_total': 0, 'duration_ticks_max': 0}
            if camera_only:
                expected_phases = CAMERA_PHASES
                phases = {name: phases[name] for name in CAMERA_PHASES}
                notes.add('Camera-only observation: gameplay mods may run concurrently; other engine systems were not recorded.')
            require({h['name'] for h in row['hooks']} == set(expected_phases), 'Incomplete hook manifest')
            if row['schema'] >= 2 and not camera_only:
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
        require(not camera_only or name in CAMERA_PHASES + ('camera_state', 'camera_slot'), 'Unexpected non-camera record in focused capture')
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
                if name in CAMERA_PHASES:
                    camera_current.pop(span, None)
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
        elif name == 'entity_body' and header['schema'] >= 3:
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
        elif name == 'entity_scan' and header['schema'] >= 3:
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
        elif name == 'body_accessor' and header['schema'] >= 4:
            require(edge == 0 and span > 0 and flags in (1, 3, 4, 6) and obj != '0' and detail != '0'
                    and (int(entity) & 0xffffffff) < 1 << 20, 'Invalid accessor sample')
            linear, angular = struct.unpack('<ff', int(value).to_bytes(8, 'little'))
            require(all(math.isfinite(v) and v >= 0 for v in (linear, angular)), 'Invalid getter values')
            key = (detail, entity, obj)
            require(key in accessor_observations or len(accessor_observations) < MAX_TRACKED, 'Too many accessor observations')
            observed = accessor_observations.setdefault(key, {'scene': detail, 'handle': entity, 'actor': obj,
                'matched_samples': 0, 'mismatched_samples': 0, 'alternate_samples': 0,
                'linear_min': linear, 'linear_max': linear, 'angular_min': angular, 'angular_max': angular})
            observed['matched_samples' if flags & 1 else 'mismatched_samples'] += 1
            observed['alternate_samples'] += bool(flags & 2)
            for prefix, v in (('linear', linear), ('angular', angular)):
                observed[prefix + '_min'] = min(observed[prefix + '_min'], v)
                observed[prefix + '_max'] = max(observed[prefix + '_max'], v)
        elif name == 'accessor_scan' and header['schema'] >= 4:
            require(edge == 0 and span > 0 and int(value) <= 4 and int(entity) <= int(value)
                    and detail == '0' and flags < 1 << len(ACCESS_REJECTIONS) and not flags & 1
                    and (obj != '0' or (value == entity == '0' and flags == 0)), 'Invalid accessor scan')
            accessor_scans += 1
            for i, reason in enumerate(ACCESS_REJECTIONS):
                if flags & (1 << i): accessor_scan_rejections[reason] += 1
        elif name in ('camera_state', 'camera_slot') and header['schema'] >= 5:
            sample_edge = flags >> 8
            require(edge == 0 and span > 0 and sample_edge in (1, 2), 'Invalid camera snapshot edge')
            parent = pending.get(span)
            if parent is None:
                notes.add('Camera snapshots lack an enclosing phase entry; recording losses limit attribution.')
            else:
                require(parent[0] in CAMERA_PHASES and parent[2] == thread, 'Camera snapshot phase/thread mismatch')
            if name == 'camera_state':
                reason = flags & 255
                require(reason < len(CAMERA_READS), 'Unknown camera read result')
                require((reason == 0 and 1 <= int(value) <= 4 and obj != '0' and entity != '0') or
                        (reason != 0 and value == detail == entity == '0'), 'Invalid camera state payload')
                camera_reads[CAMERA_READS[reason]] += 1
                if reason == 0:
                    camera_modes[str(int(value) - 1)] += 1
                current = (obj, entity, value, detail, reason, thread, time)
                require(span in camera_current or len(camera_current) < MAX_TRACKED, 'Too many camera sample groups')
                camera_current[span] = [sample_edge, entity, reason, 0]
                if sample_edge == 1:
                    require(span not in camera_before and len(camera_before) < MAX_TRACKED, 'Invalid camera snapshot pairing')
                    camera_before[span] = current
                else:
                    previous = camera_before.pop(span, None)
                    if previous is not None:
                        require(previous[5] == thread and previous[6] <= time, 'Camera snapshot time/thread mismatch')
                        camera_pairs['pairs'] += 1
                        if previous[4] == reason == 0:
                            camera_pairs['readable_pairs'] += 1
                            if previous[:4] != current[:4]:
                                require(len(camera_changes) < MAX_TRACKED, 'Too many camera transitions')
                                camera_changes.append({'span': span, 'phase': parent[0] if parent else None,
                                    'thread': thread, 'qpc': time,
                                    'before': {'world': previous[0], 'camera_global': previous[1], 'mode': int(previous[2])-1, 'selected': previous[3]},
                                    'after': {'world': obj, 'camera_global': entity, 'mode': int(value)-1, 'selected': detail}})
            else:
                slot, state = flags & 3, (flags & 255) >> 2
                current = camera_current.get(span)
                if current is None or current[0] != sample_edge:
                    notes.add('Camera slot snapshots lack their state record; recording losses limit attribution.')
                else:
                    require(current[1] == obj and current[2] == 0, 'Camera slot does not match readable global state')
                    require(not current[3] & (1 << slot), 'Duplicate slot in camera snapshot')
                    current[3] |= 1 << slot
                require(obj != '0' and (bool(state & 1) == (entity != '0')), 'Invalid camera slot identity')
                require(not state & 2 or state & 1, 'Live camera slot must be present')
                require(not state & 12 or state & 2, 'Camera pose needs a live entity')
                require(not state & 32 or state == 33, 'Changed camera slot cannot retain pose data')
                require((bool(state & 4) == (value != '0')) and (bool(state & 8) == (detail != '0')), 'Invalid camera pose digests')
                camera_slots[(slot, state)] += 1
        else:
            raise ValueError('Unknown event kind')
    require(header is not None, 'No capture header')
    # Switch/init/remove calls need not occur in every capture. Their absence
    # remains explicit and is never interpreted as verified restoration.
    missing = [name for name, info in phases.items() if not info['paired'] and
               (name not in CAMERA_PHASES or name == 'camera_update')]
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
    if header['schema'] >= 2 and not camera_only and not body_observations:
        failed = True
        notes.add('No accepted dynamic-body snapshots; body observation remains unvalidated.')
    if header['schema'] >= 3 and not camera_only and not entity_observations:
        failed = True
        notes.add('No accepted body/entity associations; prop identity remains unvalidated.')
    if entity_observations:
        notes.add('Body/entity associations are guarded same-callback observations, not retained handles, selectable props, or scheduler exclusion.')
    getter_matches = sum(o['matched_samples'] for o in accessor_observations.values())
    getter_mismatches = sum(o['mismatched_samples'] for o in accessor_observations.values())
    mismatch_seen = getter_mismatches > 0 or accessor_scan_rejections['mismatch'] > 0
    if header['schema'] >= 4 and not camera_only and (not getter_matches or mismatch_seen):
        failed = True
        notes.add('Native getter agreement is missing or a mismatch was recorded; investigate before using property writes.')
    if header['schema'] >= 5 and not camera_pairs['readable_pairs']:
        failed = True
        notes.add('No readable paired camera snapshots; camera lifetime and timing remain unestablished.')
    incomplete = failed or missing or dropped or (not camera_only and (not player_samples or not resource_ids))
    return {'schema': 1, 'sha256': header['sha256'], 'mode': header['mode'], 'status': 'incomplete' if incomplete else 'ready_for_manual_review',
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
            'body_probe': {'available': header['schema'] >= 2 and not camera_only, 'scans': body_scans,
                           'status': 'observed' if body_observations else 'no_accepted_samples' if header['schema'] >= 2 and not camera_only else 'not_recorded',
                           'observed_slot_replacements': body_replacements,
                           'scans_with_rejection': dict(body_scan_rejections),
                           'bodies': list(body_observations.values()),
                           'ownership_or_mutation_verified': False},
            'entity_probe': {'available': header['schema'] >= 3 and not camera_only, 'scans': entity_scans,
                             'status': 'observed' if entity_observations else 'no_accepted_samples' if header['schema'] >= 3 and not camera_only else 'not_recorded',
                             'scans_with_rejection': dict(entity_scan_rejections),
                             'associations': list(entity_observations.values()),
                             'ownership_or_mutation_verified': False},
            'accessor_probe': {'available': header['schema'] >= 4 and not camera_only, 'scans': accessor_scans,
                              'status': 'not_recorded' if header['schema'] < 4 or camera_only else 'mismatch_observed' if mismatch_seen else 'agreement_observed' if getter_matches else 'no_accepted_samples',
                              'matched_samples': getter_matches, 'mismatched_samples': getter_mismatches,
                              'scans_with_rejection': dict(accessor_scan_rejections),
                              'bodies': list(accessor_observations.values()),
                              'ownership_or_mutation_verified': False},
            'camera_probe': {'available': header['schema'] >= 5,
                             'status': 'not_recorded' if header['schema'] < 5 else 'snapshots_observed' if camera_pairs['readable_pairs'] else 'no_readable_pairs',
                             'reads': dict(camera_reads), 'modes': dict(camera_modes),
                             'snapshot_pairs': dict(camera_pairs), 'unpaired_snapshots': len(camera_before),
                             'transitions': camera_changes,
                             'slots': [{'slot': s, 'flags': f, 'samples': n} for (s, f), n in sorted(camera_slots.items())],
                             'unobserved_phases': [name for name in CAMERA_PHASES if name in phases and not phases[name]['paired']],
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
