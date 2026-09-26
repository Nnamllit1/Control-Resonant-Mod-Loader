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
    terminal = None
    header = False
    total = 0
    with path.open('rb') as stream:
        while line := stream.readline(4097):
            total += len(line)
            if len(line) > 4096 or total > 4 * 1024 * 1024:
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
            if kind == 'stats':
                for name in ('dropped', 'dispatches', 'context_rejections', 'retirements'):
                    if type(item.get(name)) is not int or item[name] < 0:
                        raise ValueError('Invalid stats')
                dropped = max(dropped, item['dropped'])
                dispatches = max(dispatches, item['dispatches'])
                rejections = max(rejections, item['context_rejections'])
                retirements = max(retirements, item['retirements'])
                for name in ('selection_slots', 'selection_scanned'):
                    if type(item.get(name, 0)) is not int or not 0 <= item.get(name, 0) <= 2**20:
                        raise ValueError('Invalid selection bounds')
                selection_slots = max(selection_slots, item.get('selection_slots', 0))
                selection_scanned = max(selection_scanned, item.get('selection_scanned', 0))
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
            if action not in (0, 1, 2) or (action == 0 and result > 6) or (action == 1 and result >= len(RESULTS)) or (action == 2 and (result & ~0x1ff or (result & 255) > 9)):
                raise ValueError('Unknown action or result')
            if action == 0 and result:
                failures[str(result)] = failures.get(str(result), 0) + 1
                continue
            key = str(item['selection'])
            if not item['selection']:
                raise ValueError('Missing selection identity')
            state = selections.setdefault(key, {'identity': list(identities), 'results': [], 'writes': []})
            if state['identity'] != list(identities):
                raise ValueError('Identity changed within a selection')
            if action == 0:
                state['original_linear'] = item['before']
                state['original_angular'] = item['after']
            elif action == 1:
                state['results'].append(RESULTS[result])
            else:
                state['writes'].append({'status': result & 255, 'attempted': bool(result & 256),
                                        'before': item['before'], 'after': item['after']})
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
    return {'schema': 1, 'events': previous, 'dispatches': dispatches, 'context_rejections': rejections,
            'retirements': retirements, 'dropped': dropped, 'terminal_reason': terminal,
            'selection_failures': failures, 'selections': selections,
            'max_selection_slots': selection_slots, 'max_selection_scanned': selection_scanned,
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
