"""Build and check freestanding C mods from a checkout or extracted CRML SDK."""
import argparse
import hashlib
import json
import math
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = 'wasi-sdk-27.0-x86_64-windows'
TOOLCHAIN_SHA256 = '4a576c13125c91996d8cc3b70b7ea0612c2044598d2795c9be100d15f874adf6'
TOOLCHAIN_URL = f'https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/{TOOLCHAIN}.tar.gz'
SIM_CAPS = {'input.buttons': 2, 'player.visibility': 8, 'physics.damping': 16,
            'input.motion': 32, 'player.motion': 64, 'input.actions': 128,
            'player.read': 256, 'camera.read': 512, 'feedback': 131072,
            'ui.read': 1024, 'ui.activate': 2048, 'ui.presentation': 16384,
            'media.read': 4096, 'media.skip': 8192, 'navigation.read': 524288, 'lists': 2097152}
SIM_OPERATIONS = {'motion_set', 'visibility_set', 'physics_select', 'physics_apply', 'physics_restore'}


def object_fields(value, allowed, label):
    if not isinstance(value, dict) or set(value) - set(allowed):
        raise ValueError(f'{label}: expected an object with fields {", ".join(allowed)}')
    return value


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'Expected an integer in {low}..{high}')
    return value


def number(value):
    if type(value) not in (int, float) or abs(value) > 3.4028234e38 or not math.isfinite(value):
        raise ValueError('Expected a finite float32 value')
    return value


def vector(value, length):
    if not isinstance(value, list) or len(value) != length:
        raise ValueError(f'Expected {length} numeric components')
    return [number(item) for item in value]


def scenario_state(state):
    object_fields(state, ('keys', 'focused', 'input_fresh', 'input_emergency', 'feedback_renderer', 'heading', 'player', 'camera', 'navigation',
                         'physics', 'physics_status', 'ui', 'ui_renderer', 'ui_new_page', 'ui_outcome', 'settings',
                         'media', 'media_consume', 'media_observer', 'visibility_observation', 'returns', 'lists_renderer', 'list_actions'), 'state')
    lines = []
    if 'visibility_observation' in state:
        values = {'none': 0, 'hidden': 1, 'submitted': 2}
        selected = state['visibility_observation']
        if not isinstance(selected, str) or selected not in values:
            raise ValueError('visibility_observation must be none, hidden or submitted')
        lines.append(f'visibility_observation {values[selected]}')
    if 'settings' in state:
        edits = state['settings']
        if not isinstance(edits, list) or len(edits) > 1024:
            raise ValueError('settings must be an array of at most 1024 edits')
        for edit in edits:
            object_fields(edit, ('mod', 'key', 'value'), 'setting edit')
            for field, limit in [('mod',64), ('key',31)]:
                token = edit.get(field)
                if (not isinstance(token,str) or not 1 <= len(token) <= limit or
                        any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789_-' for c in token)):
                    raise ValueError(f'Invalid setting {field}')
            raw = edit.get('value')
            if isinstance(raw, str):
                encoded = raw.encode('utf-8', errors='strict')
                if len(encoded) > 255 or any(ord(c) < 32 or 127 <= ord(c) <= 159 or c in '\u2028\u2029' for c in raw):
                    raise ValueError('Text settings require at most 255 UTF-8 bytes without controls or line separators')
                lines.append(f"setting_text {edit['mod']} {edit['key']} t{encoded.hex()}")
            else:
                lines.append(f"setting {edit['mod']} {edit['key']} {number(raw)}")
    for name, command in [('focused', 'focus'), ('input_fresh', 'fresh'), ('input_emergency', 'emergency'), ('feedback_renderer', 'feedback_renderer'),
                          ('ui_renderer','ui_renderer'), ('ui_new_page','ui_new_page'), ('lists_renderer','lists_renderer'),
                          ('media_consume','media_consume'), ('media_observer','media_observer')]:
        if name in state:
            if type(state[name]) is not bool:
                raise ValueError(f'{name} must be boolean')
            lines.append(f'{command} {int(state[name])}')
    if 'list_actions' in state:
        actions = state['list_actions']
        if not isinstance(actions, list) or len(actions) > 16:
            raise ValueError('list_actions must be an array of at most 16 actions')
        for action in actions:
            object_fields(action, ('mod', 'row', 'revision'), 'list action')
            identity = action.get('mod')
            if not isinstance(identity, str) or re.fullmatch(r'[a-z0-9_-]{1,64}', identity) is None:
                raise ValueError('Invalid list action mod ID')
            row = integer(action.get('row'), 1, 2**64-1)
            revision = integer(action['revision'], 1, 2**64-1) if 'revision' in action else 0
            lines.append(f'list_action {identity} {row} {revision}')
    if 'media' in state:
        value = object_fields(state['media'], ('identity','active','ready','elapsed_ms','name','source'), 'media')
        name = value.get('name','')
        # Asset references use a deliberately restricted ASCII test vocabulary.
        if not isinstance(name,str) or len(name)>255 or re.fullmatch(r'[A-Za-z0-9_./\\: -]*', name) is None:
            raise ValueError('Invalid simulated media asset name')
        for field in ('active','ready'):
            if type(value.get(field,True)) is not bool:
                raise ValueError(f'media {field} must be boolean')
        source = value.get('source','engine')
        if source not in ('engine','mapped','none'):
            raise ValueError('media source must be engine, mapped or none')
        parts = [integer(value.get('identity',1),0,2**64-1),int(value.get('active',True)),
                 int(value.get('ready',True)),integer(value.get('elapsed_ms',0),0,2**32-1),
                 {'engine':4,'mapped':8,'none':0}[source],json.dumps(name)]
        lines.append('media '+' '.join(map(str,parts)))
    if 'ui' in state:
        value = object_fields(state['ui'], ('screen', 'actions'), 'ui')
        lines.append(f"ui {integer(value.get('screen',0),0,9)} {integer(value.get('actions',0),0,1)}")
    if 'ui_outcome' in state:
        outcomes = {'none':0, 'dispatched':1, 'skipped':2, 'failed':3}
        if not isinstance(state['ui_outcome'],str) or state['ui_outcome'] not in outcomes:
            raise ValueError('ui_outcome must be none, dispatched, skipped or failed')
        lines.append(f"ui_outcome {outcomes[state['ui_outcome']]}")
    if 'keys' in state:
        keys = state['keys']
        if (not isinstance(keys, list) or len(keys) > 40 or
                any(not isinstance(key, str) or not key or not key.isascii() or not key.isalnum() for key in keys)):
            raise ValueError('keys must be a bounded list of named controls')
        lines.append('keys '+' '.join(keys))
    if 'heading' in state:
        value = object_fields(state['heading'], ('result', 'right'), 'heading')
        parts = [integer(value.get('result', 1), -1, 1), *vector(value.get('right', [1, 0]), 2)]
        lines.append('heading '+' '.join(map(str, parts)))
    if 'navigation' in state:
        value = object_fields(state['navigation'], ('result', 'generation', 'sequence', 'age_ms', 'flags', 'position', 'up'), 'navigation')
        flags = integer(value.get('flags', 0), 0, 15)
        up = vector(value.get('up', [0, 0, 0]), 3)
        if flags & 1 and not 0.98 <= sum(component*component for component in up) <= 1.02:
            raise ValueError('navigation up must be a unit direction when flags contains UP_VALID')
        if not flags & 1:
            up = [0, 0, 0]
        parts = [integer(value.get('result', 1), -1, 1), integer(value.get('generation', 1), 1, 2**64-1),
                 integer(value.get('sequence', 1), 1, 2**64-1), integer(value.get('age_ms', 0), 0, 2**32-1), flags]
        parts += vector(value.get('position', [0, 0, 0]), 3) + up
        lines.append('navigation '+' '.join(map(str, parts)))
    for name in ('player', 'camera', 'physics'):
        if name not in state:
            continue
        fields = {'player': ('result', 'generation', 'age_ms', 'position'),
                  'camera': ('result', 'generation', 'age_ms', 'mode', 'flags', 'position', 'basis', 'fov', 'aspect'),
                  'physics': ('result', 'age_ms', 'flags', 'linear_damping', 'angular_damping', 'linear_speed', 'angular_speed')}[name]
        value = object_fields(state[name], fields, name)
        parts = [integer(value.get('result', 1), -3 if name == 'physics' else -1, 1)]
        if name != 'physics':
            parts += [integer(value.get('generation', 1), 1, 2**64-1)]
        parts += [integer(value.get('age_ms', 0), 0, 2**32-1)]
        if name == 'camera':
            parts += [integer(value.get('mode', 0), -2**31, 2**31-1), integer(value.get('flags', 1), 0, 3)]
        if name != 'physics':
            parts += vector(value.get('position', [0, 0, 0]), 3)
        if name == 'camera':
            parts += vector(value.get('basis', [1, 0, 0, 0, 1, 0, 0, 0, 1]), 9)
            parts += [number(value.get('fov', 1)), number(value.get('aspect', 1.7777778))]
        if name == 'physics':
            parts += [integer(value.get('flags', 3), 0, 3)]
            parts += [number(value.get(field, 0)) for field in fields[3:]]
        lines.append(name+' '+' '.join(map(str, parts)))
    if 'physics_status' in state:
        lines.append('status '+str(integer(state['physics_status'], 0, 9)))
    if 'returns' in state:
        for operation, code in object_fields(state['returns'], sorted(SIM_OPERATIONS), 'returns').items():
            lines.append(f'return {operation} {-99 if code is None else integer(code, -3, -1)}')
    return lines


def scenario_protocol(document):
    object_fields(document, ('schema', 'capabilities', 'initial', 'frames', 'expect', 'expect_failures'), 'scenario')
    if type(document.get('schema')) is not int or document['schema'] != 1:
        raise ValueError('Scenario schema must be 1')
    caps = document.get('capabilities', list(SIM_CAPS))
    if not isinstance(caps, list) or any(not isinstance(cap, str) or cap not in SIM_CAPS for cap in caps):
        raise ValueError('Unsupported simulation capability')
    if len(caps) != len(set(caps)):
        raise ValueError('Duplicate simulation capability')
    lines = ['caps '+str(sum(SIM_CAPS[cap] for cap in caps))]
    lines += scenario_state(document.get('initial', {}))
    lines += ['start']
    frames = document.get('frames')
    if not isinstance(frames, list) or len(frames) > 10000:
        raise ValueError('frames must be an array of at most 10000 frames')
    for frame in frames:
        object_fields(frame, ('dt_ms', 'state'), 'frame')
        lines += scenario_state(frame.get('state', {}))
        lines.append('tick '+str(integer(frame.get('dt_ms', 10), 0, 60000)))
    lines.append('finish')
    integer(document.get('expect_failures', 0), 0, 32)
    if 'expect' in document:
        if not isinstance(document['expect'], list) or len(document['expect']) > 50000:
            raise ValueError('expect must be an array of at most 50000 events')
        for event in document['expect']:
            object_fields(event, ('op', 'frame', 'time_ms', 'owner', 'result', 'args', 'target'), 'expected event')
            if not isinstance(event.get('op'), str) or event['op'] not in SIM_OPERATIONS | {'release', 'motion_cancel', 'visibility_cancel', 'physics_cancel','ui_activate','ui_action_submit','ui_present','media_skip','media_consumed','list_activate','list_page'}:
                raise ValueError('Expected event needs a supported op')
            if 'target' in event:
                if event['op'] != 'ui_present' or not isinstance(event['target'],str) or re.fullmatch(r'[A-Za-z0-9_-]{1,64}',event['target']) is None:
                    raise ValueError('Expected target requires a valid ui_present name')
            for name, limit in [('frame', 10000), ('time_ms', 600000000), ('owner', 2**64-1)]:
                if name in event:
                    integer(event[name], 0, limit)
            if 'result' in event:
                integer(event['result'], -5 if event['op'] in ('ui_action_submit','ui_present') else -3,
                        2**32-1 if event['op']=='ui_action_submit' else 507 if event['op']=='list_activate' else 1)
            if 'args' in event:
                if not isinstance(event['args'], list) or len(event['args']) > 4:
                    raise ValueError('Expected event args must contain at most four numbers')
                for item in event['args']:
                    number(item)
    return '\n'.join(lines)+'\n'


def simulate(mods, scenario, host=None, report=None, profile=False):
    with scenario.open('rb') as stream:
        data = stream.read(1024*1024+1)
    if len(data) > 1024*1024:
        raise ValueError('Scenario exceeds 1 MiB')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f'Duplicate scenario field: {key}')
            result[key] = value
        return result
    try:
        document = json.loads(data, object_pairs_hook=unique)
    except RecursionError as error:
        raise ValueError('Scenario nesting is too deep') from error
    protocol = scenario_protocol(document)
    command = [str(tool('crml_host', host)), str(mods.resolve()), '--simulate']
    if profile:
        command.append('--profile')
    process = subprocess.run(command,
                             input=protocol, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
    events, logs, failures = [], [], None
    # Only ASCII newline separates native records. Guest Unicode separators
    # remain inside the prefixed log record and cannot forge protocol records.
    for line in process.stdout.split('\n'):
        if line.startswith('@event '):
            parts = line.split()
            target = None
            if parts[3] == 'ui_present':
                encoded = parts.pop()
                target = '' if encoded == '-' else bytes.fromhex(encoded).decode('utf-8',errors='replace')
            events.append({'frame': int(parts[1]), 'time_ms': int(parts[2]), 'op': parts[3],
                           'owner': int(parts[4]), 'result': int(parts[5]),
                           'args': [float(value) if any(c in value for c in '.eE') else int(value) for value in parts[6:]]})
            if target is not None:
                events[-1]['target'] = target
        elif line.startswith('@summary '):
            failures = int(line.split()[1])
        else:
            logs.append(line)
    result = {'schema': 1, 'events': events, 'logs': logs, 'failures': failures}
    if profile:
        for line in logs:
            if re.match(r'^\[\+\d+ms\] \[host\] Metrics ', line):
                print(line)
    if report:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    if process.returncode not in (0, 1) or failures is None:
        raise ValueError('Simulation host failed: '+process.stderr.strip())
    if failures != document.get('expect_failures', 0):
        raise ValueError(f'Expected {document.get("expect_failures", 0)} guest failures, observed {failures}. '+process.stdout)
    if 'expect' in document:
        if len(events) != len(document['expect']):
            raise ValueError(f'Expected {len(document["expect"])} events, observed {len(events)}: {events}')
        for index, (actual, expected) in enumerate(zip(events, document['expect'])):
            for field, value in expected.items():
                matches = actual[field] == value
                if field == 'args':
                    matches = len(actual[field]) == len(value) and all(
                        a == b if type(a) is int and type(b) is int else math.isclose(a, b, rel_tol=1e-5, abs_tol=1e-6)
                        for a, b in zip(actual[field], value))
                if not matches:
                    raise ValueError(f'Event {index} differs: expected {expected}, observed {actual}')
    print(f'Simulation passed: {len(events)} commands/cleanup events; {failures} expected guest failures.')
    return result


def run(arguments):
    subprocess.run([str(value) for value in arguments], check=True)


def tool(name, explicit=None):
    if explicit:
        path = Path(explicit).resolve()
        if not path.is_file():
            raise ValueError(f'Tool does not exist: {path}')
        return path
    for directory in (ROOT/'tools', ROOT/'crml', ROOT/'dist/crml', ROOT/'build/native/Release'):
        path = directory/(name+'.exe')
        if path.is_file():
            return path
    raise ValueError(f'{name}.exe not found. Extract the complete SDK or build the native tools.')


def compiler(explicit=None):
    if explicit or os.environ.get('CRML_CLANG'):
        return tool('clang', explicit or os.environ['CRML_CLANG'])
    pinned = ROOT/'build/deps'/TOOLCHAIN/'bin/clang.exe'
    if pinned.is_file():
        return pinned
    available = shutil.which('clang')
    if available:
        return Path(available)
    raise ValueError('Install a wasm32-capable Clang, set CRML_CLANG, or run: python tools/mod.py toolchain')


def fetch_toolchain():
    if sys.platform != 'win32':
        raise ValueError('The pinned compiler download is Windows x64; use --clang on other hosts.')
    deps = ROOT/'build/deps'
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps/(TOOLCHAIN+'.tar.gz')
    if not archive.exists():
        print('Downloading pinned C compiler (543 MB); it is not bundled in mod releases.', flush=True)
        with tempfile.NamedTemporaryFile(dir=deps, suffix='.download', delete=False) as stream:
            temporary = Path(stream.name)
            try:
                with urllib.request.urlopen(TOOLCHAIN_URL, timeout=60) as response:
                    shutil.copyfileobj(response, stream)
            except BaseException:
                stream.close()
                temporary.unlink(missing_ok=True)
                raise
        try:
            with temporary.open('rb') as stream:
                checksum = hashlib.file_digest(stream, 'sha256').hexdigest()
            if checksum != TOOLCHAIN_SHA256:
                raise ValueError('Compiler download checksum mismatch')
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    with archive.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != TOOLCHAIN_SHA256:
            raise ValueError('Cached compiler archive checksum mismatch')
    # Extract only the freestanding compiler/linker, their DLLs and builtin
    # headers. No WASI sysroot or WASI runtime is installed or linked.
    with tarfile.open(archive, 'r|gz') as source:
        for entry in source:
            parts = Path(entry.name).parts
            if not parts or parts[0] != TOOLCHAIN:
                continue
            relative = Path(*parts[1:])
            selected = (relative.as_posix() in ('bin/clang.exe', 'bin/wasm-ld.exe') or
                        (relative.parent.as_posix() == 'bin' and relative.suffix.lower() == '.dll') or
                        (relative.as_posix().startswith('lib/clang/') and 'include' in relative.parts))
            if not selected or not entry.isfile():
                continue
            target = (deps/entry.name).resolve()
            if not target.is_relative_to((deps/TOOLCHAIN).resolve()):
                raise ValueError('Unsafe compiler archive path')
            data = source.extractfile(entry).read()
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.is_file() or target.read_bytes() != data:
                target.write_bytes(data)
    for name in ('clang.exe', 'wasm-ld.exe'):
        if not (deps/TOOLCHAIN/'bin'/name).is_file():
            raise ValueError(f'Compiler archive is missing {name}')
    run([deps/TOOLCHAIN/'bin/clang.exe', '--version'])


def manifest(path):
    fields = {}
    data = path.read_text(encoding='utf-8')
    if len(data.encode()) > 8192:
        raise ValueError('Manifest exceeds 8 KiB')
    for line in data.splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, separator, value = line.partition('=')
        key, value = key.strip(), value.strip()
        if not separator or key in fields:
            raise ValueError('Expected unique key=value manifest fields')
        fields[key] = value
    module = fields.get('module', '')
    if (not module or len(module) > 100 or not module.endswith('.wasm') or
            any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-' for c in module)):
        raise ValueError('Manifest module must be a local .wasm filename')
    return fields


def build(package, output, clang=None):
    package, output = package.resolve(), output.resolve()
    fields = manifest(package/'mod.ini')
    sources = sorted(package.glob('*.c'))
    if not sources:
        raise ValueError('Package needs at least one C source file')
    output.mkdir(parents=True, exist_ok=True)
    # A temporary binary preserves the last successful build on compiler failure.
    with tempfile.TemporaryDirectory(prefix='crml-build-') as temporary:
        binary = Path(temporary)/fields['module']
        run([compiler(clang), '--target=wasm32-unknown-unknown', '-std=c11', '-O2',
             '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin', '-nostdlib',
             '-I', ROOT/'sdk/include', *sources, '-Wl,--no-entry', '-Wl,--export-memory',
             '-Wl,--export=crml_abi_version', '-Wl,--export=crml_init',
             '-Wl,--export-if-defined=crml_tick', '-Wl,--export-if-defined=crml_shutdown',
             '-Wl,--max-memory=16777216', '-o', binary])
        data = binary.read_bytes()
        if not data.startswith(b'\0asm\x01\0\0\0') or len(data) > 4*1024*1024:
            raise ValueError('Compiler output is not a supported core Wasm module')
        (output/fields['module']).write_bytes(data)
    if package != output:
        shutil.copyfile(package/'mod.ini', output/'mod.ini')
    print(f'Built {output/fields["module"]}')


def check(package, host=None, ticks=0):
    fields = manifest(package/'mod.ini')
    with tempfile.TemporaryDirectory(prefix='crml-check-') as temporary:
        target = Path(temporary)/'mod'
        target.mkdir()
        shutil.copyfile(package/'mod.ini', target/'mod.ini')
        shutil.copyfile(package/fields['module'], target/fields['module'])
        run([tool('crml_host', host), temporary, str(ticks)])


def create(package, mod_id):
    if not mod_id or len(mod_id) > 64 or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789_-' for c in mod_id):
        raise ValueError('ID must be 1..64 lowercase letters, digits, underscore or hyphen')
    if package.exists():
        raise ValueError('Source directory already exists; choose a new one')
    template = ROOT/'sdk/templates/basic'
    shutil.copytree(template, package)
    path = package/'mod.ini'
    path.write_text(path.read_text(encoding='utf-8').replace('id=my-mod', 'id='+mod_id, 1), encoding='utf-8')
    print(f'Created {package}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('toolchain', help='Download the pinned Windows compiler and verify its checksum')
    creator = commands.add_parser('new', help='Create a C mod from the basic template')
    creator.add_argument('package', type=Path)
    creator.add_argument('--id', required=True)
    simulation = commands.add_parser('simulate', help='Run mods with deterministic scripted services and expectations')
    simulation.add_argument('mods', type=Path, help='Directory containing built mod packages')
    simulation.add_argument('scenario', type=Path)
    simulation.add_argument('--host', type=Path)
    simulation.add_argument('--report', type=Path, help='Write the observed command trace and guest logs as JSON')
    simulation.add_argument('--profile', action='store_true', help='Include measured wall-time, fuel and call-budget diagnostics (timings vary between runs)')
    builder = commands.add_parser('build', help='Compile all C sources in a mod package')
    builder.add_argument('package', type=Path)
    builder.add_argument('--output', type=Path, required=True, help='Output package directory')
    builder.add_argument('--clang', type=Path)
    for name in ('check', 'test'):
        checker = commands.add_parser(name, help='Load and run the guest in the standalone sandbox (no game)')
        checker.add_argument('package', type=Path)
        checker.add_argument('--host', type=Path)
        checker.add_argument('--ticks', type=int, default=0 if name == 'check' else 10)
    args = parser.parse_args()
    try:
        if args.command == 'toolchain':
            fetch_toolchain()
        elif args.command == 'new':
            create(args.package, args.id)
        elif args.command == 'simulate':
            simulate(args.mods, args.scenario, args.host, args.report, args.profile)
        elif args.command == 'build':
            build(args.package, args.output, args.clang)
        else:
            if not 0 <= args.ticks <= 10000:
                raise ValueError('Ticks must be 0..10000')
            check(args.package, args.host, args.ticks)
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
