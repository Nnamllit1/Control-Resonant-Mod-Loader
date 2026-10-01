"""Verify reviewed static engine-map references without running or modifying the game."""
import argparse
import hashlib
import json
from pathlib import Path
from engine_research import PE, read_bounded, require


def verify(data, profile):
    require(profile.get('schema') == 1, 'Unsupported engine-map schema')
    require(hashlib.sha256(data).hexdigest() == profile['executable_sha256'],
            'Executable fingerprint differs; map requires review for this build')
    pe = PE(data)
    failures = []
    for check in profile['checks']:
        try:
            rva, expected = int(check['rva'], 0), int(check['target'], 0)
            kind = check['kind']
            if kind == 'pointer64':
                actual = pe.unpack('<Q', pe.offset(rva, 8))[0] - pe.image_base
            elif kind == 'u32':
                actual = pe.unpack('<I', pe.offset(rva, 4))[0]
            elif kind in ('call_rel32', 'jump_rel32', 'lea_rax_rip'):
                prefix = {'call_rel32': b'\xe8', 'jump_rel32': b'\xe9',
                          'lea_rax_rip': b'\x48\x8d\x05'}[kind]
                size = len(prefix) + 4
                at = pe.offset(rva, size)
                require(pe.section(rva, size)['executable'], 'Reference outside executable section')
                require(pe.slice(at, len(prefix)) == prefix, 'Instruction encoding differs')
                actual = rva + size + pe.unpack('<i', at + len(prefix))[0]
            else:
                raise ValueError('Unsupported reference kind')
            require(actual == expected, f'Expected {expected:#x}, found {actual:#x}')
        except (KeyError, ValueError, TypeError) as error:
            failures.append(f"{check.get('label', 'unnamed')}: {error}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('map', type=Path)
    parser.add_argument('--middleware', type=Path, help='Also verify the middleware image described by the map')
    args = parser.parse_args()
    try:
        profile = json.loads(args.map.read_text(encoding='utf-8'))
        failures = verify(read_bounded(args.executable, 256 * 1024 * 1024), profile)
        middleware_failures = None
        if args.middleware:
            middleware = profile.get('middleware')
            require(isinstance(middleware, dict), 'Map has no middleware profile')
            middleware_failures = verify(read_bounded(args.middleware, 256 * 1024 * 1024),
                dict(schema=1, executable_sha256=middleware['sha256'], checks=middleware['checks']))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Map verification failed: {error}\n')
    for failure in failures:
        print(failure)
    print(f"{len(profile['checks']) - len(failures)}/{len(profile['checks'])} static references verified.")
    if middleware_failures is not None:
        for failure in middleware_failures:
            print('Middleware: ' + failure)
        print(f"{len(middleware['checks']) - len(middleware_failures)}/{len(middleware['checks'])} middleware references verified.")
    elif profile.get('middleware'):
        print('Middleware references not checked; supply --middleware to verify them.')
    print('This verifies encoded references, not semantic labels, callable ABIs, or live behavior.')
    return bool(failures or middleware_failures)


if __name__ == '__main__':
    raise SystemExit(main())
