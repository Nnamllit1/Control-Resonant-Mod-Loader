"""Summarize bounded native physics-trial logs without reading game memory."""
import argparse
import json
from pathlib import Path

RESULTS = ('idle', 'invalid', 'busy', 'unavailable', 'retired', 'unchanged',
           'applied', 'active', 'restore_pending', 'restored', 'refused', 'conflict')


def analyze(path):
    if path.stat().st_size > 4 * 1024 * 1024:
        raise ValueError('Capture exceeds 4 MiB')
    selections, failures = {}, {}
    previous = 0
    dropped = dispatches = rejections = retirements = 0
    selection_slots = selection_scanned = 0
    latest_search = None
    targets = []
    controls = []
    lifetime_events = []
    lifetime_missed = 0
    target_dropped = 0
    terminal = None
    header = False
    total = 0
    with path.open('rb') as stream:
        while line := stream.readline(32769):
            total += len(line)
            if len(line) > 32768 or total > 4 * 1024 * 1024:
                raise ValueError('Capture exceeds record or file bound')
            item = json.loads(line)
            if not isinstance(item, dict):
                raise ValueError('Record must be an object')
            kind = item.get('type')
            if not header:
                if kind != 'header' or item.get('schema') != 1 or item.get('mode') != 'native-physics-trial':
                    raise ValueError('Unsupported physics trial header')
                header = True
                continue
            if kind == 'target':
                for name in ('search', 'tick_ms', 'thread', 'body_count', 'exclusion'):
                    if type(item.get(name)) is not int or not 0 <= item[name] < 2**64:
                        raise ValueError('Invalid target integer')
                if not item['search'] or item['body_count'] > 65535 or item['exclusion'] > 6:
                    raise ValueError('Invalid target bounds')
                for name in ('scene', 'player', 'entity', 'body'):
                    value = item.get(name)
                    if not isinstance(value, str) or not value.isascii() or not value.isdigit() or int(value) >= 2**64:
                        raise ValueError('Invalid target identity')
                if item.get('role') not in ('player', 'candidate') or type(item.get('components_valid')) is not bool:
                    raise ValueError('Invalid target role')
                if type(item.get('distance')) not in (int, float) or not 0 <= item['distance'] <= 2:
                    raise ValueError('Invalid target distance')
                for name in ('position', 'player_position'):
                    value = item.get(name)
                    if not isinstance(value, list) or len(value) != 3 or any(
                            type(v) not in (int, float) or not -3.4028235e38 <= v <= 3.4028235e38 for v in value):
                        raise ValueError('Invalid target coordinates')
                hashes = item.get('components')
                if not isinstance(hashes, list) or len(hashes) > 2048 or any(type(v) is not int or not 0 <= v < 2**32 for v in hashes):
                    raise ValueError('Invalid target components')
                if not item['components_valid'] and hashes:
                    raise ValueError('Unverified target components')
                targets.append(item)
                continue
            if kind == 'stats':
                for name in ('dropped', 'dispatches', 'context_rejections', 'retirements'):
                    if type(item.get(name)) is not int or item[name] < 0:
                        raise ValueError('Invalid stats')
                dropped = max(dropped, item['dropped'])
                dispatches = max(dispatches, item['dispatches'])
                rejections = max(rejections, item['context_rejections'])
                retirements = max(retirements, item['retirements'])
                if type(item.get('target_dropped', 0)) is not int or item.get('target_dropped', 0) < 0:
                    raise ValueError('Invalid target drop count')
                target_dropped = max(target_dropped, item.get('target_dropped', 0))
                if type(item.get('lifetime_missed', 0)) is not int or item.get('lifetime_missed', 0) < 0:
                    raise ValueError('Invalid lifetime miss count')
                lifetime_missed = max(lifetime_missed, item.get('lifetime_missed', 0))
                for name in ('selection_slots', 'selection_scanned'):
                    if type(item.get(name, 0)) is not int or not 0 <= item.get(name, 0) <= 2**20:
                        raise ValueError('Invalid selection bounds')
                selection_slots = max(selection_slots, item.get('selection_slots', 0))
                selection_scanned = max(selection_scanned, item.get('selection_scanned', 0))
                if 'selection_candidates' in item:
                    count = item['selection_candidates']
                    if type(count) is not int or not 0 <= count <= 2**20:
                        raise ValueError('Invalid candidate count')
                    for name in ('nearest_distance', 'second_distance'):
                        value = item.get(name)
                        if type(value) not in (int, float) or not (value == -1 or 0 <= value <= 2):
                            raise ValueError('Invalid candidate distance')
                    latest_search = {name: item[name] for name in
                        ('selection_candidates', 'nearest_distance', 'second_distance')}
                continue
            if kind == 'end':
                terminal = item.get('reason')
                continue
            if kind != 'event':
                raise ValueError('Unsupported record')
            for name in ('sequence', 'tick_ms', 'thread', 'action', 'result', 'selection'):
                if type(item.get(name)) is not int or not 0 <= item[name] < 2**64:
                    raise ValueError('Invalid event integer')
            if item['sequence'] != previous + 1:
                raise ValueError('Non-contiguous event publication sequence')
            previous = item['sequence']
            for name in ('before', 'after'):
                if type(item.get(name)) not in (int, float) or not 0 <= item[name] <= 3.4028235e38:
                    raise ValueError('Invalid property value')
            identities = tuple(item.get(name) for name in ('scene', 'entity', 'body'))
            if any(not isinstance(v, str) or not v.isascii() or not v.isdigit() or int(v) >= 2**64 for v in identities):
                raise ValueError('Invalid identity')
            action, result = item['action'], item['result']
            if action not in range(7) or (action == 0 and result > 6) or (action == 1 and result >= len(RESULTS)) or (action == 2 and (result & ~0x1ff or (result & 255) > 9)) or (action == 3 and (result >> 4 > 6 or result & 15 > 3)) or (action in (4, 5) and not 1 <= result <= 7) or (action == 6 and result > 1):
                raise ValueError('Unknown action or result')
            if action == 6:
                if not item['selection']:
                    raise ValueError('Missing lifetime selection identity')
                lifetime_events.append({'tick_ms': item['tick_ms'], 'selection': item['selection'],
                                        'identity': list(identities),
                                        'reason': 'scene_destroy' if result else 'body_release'})
                continue
            if action in (4, 5):
                controls.append({'tick_ms': item['tick_ms'], 'stage': 'input' if action == 4 else 'callback',
                                 'buttons': result, 'selection': item['selection']})
                continue
            if action == 0 and result:
                failures[str(result)] = failures.get(str(result), 0) + 1
                continue
            key = str(item['selection'])
            if not item['selection']:
                raise ValueError('Missing selection identity')
            state = selections.setdefault(key, {'identity': list(identities), 'results': [], 'writes': [], 'motion': []})
            if state['identity'] != list(identities):
                raise ValueError('Identity changed within a selection')
            if action == 0:
                state['original_linear'] = item['before']
                state['original_angular'] = item['after']
            elif action == 1:
                state['results'].append(RESULTS[result])
            elif action == 2:
                state['writes'].append({'status': result & 255, 'attempted': bool(result & 256),
                                        'before': item['before'], 'after': item['after']})
            else:
                state['motion'].append({'tick_ms': item['tick_ms'], 'status': result >> 4,
                                        'phase': ('selected', 'active', 'restoring', 'after')[result & 15],
                                        'linear_speed': item['before'], 'angular_speed': item['after']})
    if not header:
        raise ValueError('Empty capture')
    for state in selections.values():
        # Selection can precede application by several seconds. The first
        # attempted write carries the actual captured baseline, which may
        # differ from the earlier selection sample. Require a later reverse
        # write; the application itself cannot count as restoration.
        writes = [w for w in state['writes'] if w['attempted']]
        state['restoration_observed'] = bool(not dropped and writes and
            'restored' in state['results'] and
            any(w['status'] == 0 and w['before'] == writes[0]['after'] and
                w['after'] == writes[0]['before'] for w in writes[1:]))
        state['motion_summary'] = {}
        for phase in ('selected', 'active', 'restoring', 'after'):
            samples = [m for m in state['motion'] if m['phase'] == phase and m['status'] == 0]
            if samples:
                state['motion_summary'][phase] = {'samples': len(samples),
                    'max_linear_speed': max(m['linear_speed'] for m in samples),
                    'max_angular_speed': max(m['angular_speed'] for m in samples)}
    return {'schema': 1, 'events': previous, 'dispatches': dispatches, 'context_rejections': rejections,
            'retirements': retirements, 'dropped': dropped, 'terminal_reason': terminal,
            'selection_failures': failures, 'selections': selections,
            'max_selection_slots': selection_slots, 'max_selection_scanned': selection_scanned,
            'latest_search': latest_search,
            'targets': targets, 'target_dropped': target_dropped,
            'controls': controls,
            'lifetime_events': lifetime_events, 'lifetime_missed': lifetime_missed,
            'gameplay_effect_verified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        text = json.dumps(analyze(args.capture), indent=2) + '\n'
        if args.output:
            args.output.write_text(text, encoding='utf-8')
        else:
            print(text, end='')
    except (OSError, ValueError, TypeError) as error:
        parser.exit(1, f'Trial analysis failed: {error}\n')


if __name__ == '__main__':
    main()
