"""Verify the offline dialogue producer trace against an exact executable build.

Reads bytes only; never executes engine code, opens a process or extracts dialogue.
Optional stock UI input verifies model field consumption without copying text.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

from engine_research import PE, read_bounded, require
from verify_engine_map import verify


def verify_trace(data, profile, ui=None):
    failures = verify(data, profile)
    pe = PE(data)
    instructions = profile.get('instruction_checks', [])
    declarations = profile.get('declarations', [])
    require(0 < len(instructions) <= 256, 'Missing or excessive instruction checks')
    require(0 < len(declarations) <= 64, 'Missing or excessive declaration checks')
    for row in instructions:
        try:
            rva = int(row['rva'], 0)
            expected = bytes.fromhex(row['bytes'])
            require(1 <= len(expected) <= 32, 'Invalid instruction size')
            require(pe.section(rva, len(expected))['executable'], 'Instruction outside code')
            actual = pe.slice(pe.offset(rva, len(expected)), len(expected))
            require(actual == expected, 'Instruction bytes differ')
        except (KeyError, TypeError, ValueError) as error:
            failures.append(f"{row.get('label', 'instruction')}: {error}")
    for row in declarations:
        try:
            rva = int(row['signature_rva'], 0)
            require(not pe.section(rva)['executable'], 'Declaration inside code')
            signature = pe.cstring(rva, 32768)
            require(hashlib.sha256(signature.encode('ascii')).hexdigest() ==
                    row['signature_sha256'], 'Declaration fingerprint differs')
            require(row['name'] + '(' in signature, 'Declaration name differs')
        except (KeyError, TypeError, ValueError) as error:
            failures.append(f"{row.get('name', 'declaration')}: {error}")
    if ui is not None:
        contract = profile['ui_contract']
        for slot in contract['slots']:
            for field in contract['fields']:
                name = f'ui_subtitle_{slot}_{field}.value'
                if name not in ui:
                    failures.append(f'UI does not consume {name}')
        for name in [contract['visibility'], *contract['options']]:
            if name + '.value' not in ui:
                failures.append(f'UI does not consume {name}')
        slots = sorted(set(int(i) for i in re.findall(r'ui_subtitle_(\d+)_line\.value', ui)))
        if slots != contract['slots']:
            failures.append(f'UI subtitle slot set differs: {slots}')
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--map', type=Path, default=Path(__file__).resolve().parents[1] /
                        'docs/research/dialogue-observation-map.json')
    parser.add_argument('--ui', type=Path, help='Optional already extracted stock UI HTML')
    args = parser.parse_args(argv)
    try:
        profile = json.loads(read_bounded(args.map, 1024 * 1024))
        data = read_bounded(args.executable, 256 * 1024 * 1024)
        ui = read_bounded(args.ui, 16 * 1024 * 1024).decode('utf-8') if args.ui else None
        failures = verify_trace(data, profile, ui)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Dialogue trace verification failed: {error}\n')
    for failure in failures:
        print(failure)
    checks = len(profile['checks']) + len(profile['instruction_checks']) + len(profile['declarations'])
    print(f'{checks} native trace checks examined; {len(failures)} failures.')
    print('UI consumption checked.' if ui is not None else 'UI consumption not checked; supply --ui.')
    print('Static evidence only: occurrence delivery, presentation, speaker localization, '
          'end reasons and campaign separation remain unqualified.')
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
