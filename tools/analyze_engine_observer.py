"""Summarize bounded observe-only captures; never interprets observation as API safety."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path

PHASES = ('movement', 'command_flush', 'script_fixed', 'renderer_sync',
          'physics_begin', 'physics_wait', 'physics_complete')
MAX_BYTES = 64 * 1024 * 1024
MAX_LINE = 8192
MAX_TRACKED = 16384


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
    orphan_returns = 0
    first_time = last_time = None
    for row in records(stream):
        kind = row['type']
        require(end is None, 'Records after terminal capture marker')
        if kind == 'truncated_tail':
            notes.add('Final partial line ignored; capture tail is incomplete.')
            break
        if header is None:
            require(kind == 'header' and row.get('schema') == 1 and row.get('mode') == 'observe-only'
                    and row.get('mods_suspended') is True, 'Unsupported or non-observational capture')
            require(number(row, 'qpc_frequency') > 0, 'Missing clock frequency')
            number(row, 'qpc_origin')
            require(isinstance(row.get('sha256'), str) and len(row['sha256']) == 64, 'Invalid fingerprint')
            require({h['name'] for h in row['hooks']} == set(PHASES), 'Incomplete hook manifest')
            header = row
            continue
        if kind in ('stats', 'end'):
            dropped = max(dropped, number(row, 'dropped'))
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
    for info in phases.values():
        info['threads'] = sorted(info['threads'])
        paired = info['paired']
        info['mean_ms'] = info.pop('duration_ticks_total') * 1000 / header['qpc_frequency'] / paired if paired else None
        info['max_ms'] = info.pop('duration_ticks_max') * 1000 / header['qpc_frequency'] if paired else None
    failed = end is not None and end['reason'] in ('hook_enable_failed', 'io_error')
    return {'schema': 1, 'sha256': header['sha256'], 'status': 'incomplete' if failed or missing or not player_samples or not resource_ids or dropped else 'ready_for_manual_review',
            'not_a_gameplay_or_api_validation': True,
            'duration_seconds': (last_time - first_time) / header['qpc_frequency'] if first_time is not None else 0,
            'missing_paired_phases': missing, 'dropped': dropped, 'open_spans': len(pending), 'orphan_returns': orphan_returns,
            'end_reason': end['reason'] if end else None, 'phases': phases,
            'player_samples': player_samples, 'player_identities': len(players), 'player_transitions': transitions,
            'resource_identities': len(resource_ids), 'resource_records_by_component': dict(resource_samples),
            'resource_replacements_by_component': dict(resource_changes),
            'physics_wrapper_threads': {obj: {kind: sorted(threads) for kind, threads in groups.items()} for obj, groups in physics.items()},
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
