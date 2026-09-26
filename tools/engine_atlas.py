"""Build and query a fingerprinted index of recovered engine declarations.

The index verifies strings and reference encodings against the executable. It
does not turn compiler metadata or registration patterns into callable APIs.
Only standard-library modules and the existing PE reader are required.
"""
import argparse
from bisect import bisect_right
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

from engine_research import PE, read_bounded, require


def split_top_level(text, delimiter=','):
    """Split C++ template/function declarations without splitting nested types."""
    stack, start, parts = [], 0, []
    pairs = {'>': '<', ')': '(', ']': '['}
    for i, char in enumerate(text):
        if char in '<([':
            stack.append(char)
        elif char in '>)]':
            require(bool(stack) and stack.pop() == pairs[char], 'Unbalanced declaration')
        elif char == delimiter and not stack:
            parts.append(text[start:i].strip())
            start = i + 1
    require(not stack, 'Unbalanced declaration')
    parts.append(text[start:].strip())
    return parts


def template_entries(text, marker):
    start = 0
    while (at := text.find(marker + '<', start)) >= 0:
        begin = at + len(marker) + 1
        depth, end = 1, begin
        while end < len(text) and depth:
            depth += (text[end] == '<') - (text[end] == '>')
            end += 1
        require(depth == 0, 'Unterminated access definition')
        yield split_top_level(text[begin:end - 1])
        start = end


def parse_system(signature):
    marker = 'ecs::system::getName<'
    require(marker in signature and signature.endswith('>(void)'), 'Unsupported system signature')
    declaration = signature.split(marker, 1)[1][:-len('>(void)')]
    require(' __cdecl ' in declaration, 'Missing system calling-convention declaration')
    name_and_args = declaration.split(' __cdecl ', 1)[1]
    depth, begin = 0, None
    for i, char in enumerate(name_and_args):
        depth += (char == '<') - (char == '>')
        if char == '(' and depth == 0:
            begin = i
            break
    require(begin is not None and name_and_args.endswith(')'), 'Missing system argument list')
    name = name_and_args[:begin]
    arguments = []
    for raw in split_top_level(name_and_args[begin + 1:-1]):
        if raw in ('', 'void'):
            continue
        components, environments = [], []
        for parts in template_entries(raw, 'ecs::access::DefinitionComponentEntry'):
            require(len(parts) == 3, 'Unexpected component access declaration')
            components.append({'type': parts[0], 'access_code': int(parts[1]),
                               'qualifier_code': int(parts[2])})
        for parts in template_entries(raw, 'ecs::access::DefinitionEnvironmentEntry'):
            require(len(parts) == 2, 'Unexpected environment access declaration')
            environments.append({'type': parts[0], 'access_code': int(parts[1])})
        if 'ecs::access::Definition<' in raw:
            arguments.append({'type': raw.split('<', 1)[0], 'components': components,
                              'environments': environments})
        else:
            arguments.append({'type': raw})
    return name, arguments


def lea_target(pe, rva):
    code = pe.slice(pe.offset(rva, 7), 7)
    require(code[0] in (0x48, 0x4c) and code[1] == 0x8d and
            code[2] in range(5, 0x3e, 8), 'Reference is not a RIP-relative LEA')
    require(pe.section(rva, 7)['executable'], 'Reference is outside code')
    return rva + 7 + int.from_bytes(code[3:], 'little', signed=True)


def dispatch_candidates(pe, site, next_site):
    function = pe.function(site)
    if not function:
        return []
    # Never search across another system-name reference or a pdata boundary.
    end = min(function['end_rva'], next_site)
    if end <= site:
        return []
    code = pe.slice(pe.offset(site, end - site), end - site)
    pattern = rb'\x48\x8d\x05....[\x48\x49]\x89[\x80-\x83\x85-\x87]\x40\x01\x00\x00'
    found = []
    for match in re.finditer(pattern, code, re.DOTALL):
        at = site + match.start()
        target = lea_target(pe, at)
        if pe.section(target)['executable']:
            found.append({'lea_rva': hex(at), 'target_rva': hex(target),
                          'record_slot': '0x140', 'evidence': 'registration_pattern'})
    return found


def imported_functions(pe):
    result = []
    if len(pe.directories) <= 1 or not pe.directories[1][0]:
        return result
    rva, size = pe.directories[1]
    for offset in range(0, size, 20):
        original, stamp, chain, name, thunk = pe.unpack('<5I', pe.offset(rva + offset, 20))
        if not any((original, stamp, chain, name, thunk)):
            return result
        module = pe.cstring(name)
        for i in range(65536):
            value = pe.unpack('<Q', pe.offset((original or thunk) + i * 8, 8))[0]
            if not value:
                break
            item = {'module': module, 'iat_rva': hex(thunk + i * 8)}
            if value >> 63:
                item['ordinal'] = value & 0xffff
            else:
                item['name'] = pe.cstring(value + 2)
            result.append(item)
        else:
            raise ValueError('Unterminated import thunk table')
    raise ValueError('Unterminated import descriptor table')


def build_atlas(data, catalog):
    executable = catalog.get('executable', catalog)
    digest = hashlib.sha256(data).hexdigest()
    require(digest == executable['sha256'], 'Executable differs from catalog fingerprint')
    pe = PE(data)
    systems, bindings = [], []
    sites = sorted({c['rva'] for r in executable['ecs_systems'] for c in r['lea_candidates']})
    for row in executable['ecs_systems']:
        signature, rva = row['signature'], row['rva']
        require(not pe.section(rva)['executable'], 'System signature points into code')
        require(pe.cstring(rva, len(signature) + 1) == signature, 'System signature differs')
        name, arguments = parse_system(signature)
        references, dispatch = [], []
        for candidate in row['lea_candidates']:
            at = candidate['rva']
            require(lea_target(pe, at) == rva, 'System name reference differs')
            function = pe.function(at)
            reference = {'rva': hex(at)}
            if function:
                reference['runtime_function'] = {k: hex(v) for k, v in function.items()}
            references.append(reference)
            index = bisect_right(sites, at)
            dispatch.extend(dispatch_candidates(pe, at, sites[index] if index < len(sites) else 1 << 32))
        systems.append({'name': name, 'family': '::'.join(name.split('::')[:2]),
                        'signature_rva': hex(rva),
                        'signature_sha256': hashlib.sha256(signature.encode('ascii')).hexdigest(),
                        'arguments': arguments, 'name_references': references,
                        'dispatch_candidates': dispatch})
    for row in executable['binding_candidates']:
        name, at = row['name'], row['registration_rva']
        require(pe.cstring(row['name_rva'], len(name) + 1) == name, 'Binding name differs')
        require(not pe.section(row['name_rva'])['executable'], 'Binding name points into code')
        require(lea_target(pe, at) == row['name_rva'], 'Binding name reference differs')
        require(lea_target(pe, at + 14) == row['callback_rva'], 'Binding callback reference differs')
        require(pe.section(row['callback_rva'])['executable'], 'Binding callback is outside code')
        code = pe.slice(pe.offset(at, 28), 28)
        require(code[7:10] == b'\x48\x89\x85' and code[21:24] == b'\x48\x89\x85',
                'Binding registration stores differ')
        bindings.append({'name': name, 'name_rva': hex(row['name_rva']),
                         'callback_rva': hex(row['callback_rva']), 'registration_rva': hex(at),
                         'evidence': 'registration_pattern'})
    systems.sort(key=lambda r: (r['name'], r['signature_rva']))
    bindings.sort(key=lambda r: (r['name'], r['registration_rva']))
    families = Counter(r['family'] for r in systems)
    imports = imported_functions(pe)
    return {'schema': 1, 'kind': 'engine_atlas', 'executable_sha256': digest,
            'evidence_boundary': 'Complete for supplied catalog only. Strings and encodings verified; '
            'pattern associations are candidates, not callable ABI, behavior, scheduler order or engine completeness.',
            'access_codes': 'Raw compiler metadata; numeric modes are not translated into permissions.',
            'summary': {'systems': len(systems), 'families': len(families), 'bindings': len(bindings),
                        'systems_with_dispatch_candidates': sum(bool(r['dispatch_candidates']) for r in systems),
                        'imports': len(imports)},
            'families': dict(sorted(families.items())), 'systems': systems,
            'bindings': bindings, 'imports': imports}


def index_markdown(atlas):
    summary = atlas['summary']
    lines = ['# Recovered system index', '',
             'Generated by `tools/engine_atlas.py` from the fingerprinted executable and research catalog. '
             'This index covers all declarations in that catalog, not all code in the engine. '
             'Registration targets remain candidates until their dispatch bodies are reviewed.', '',
             f"**{summary['systems']} system declarations; {summary['families']} namespace families; "
             f"{summary['bindings']} script-binding candidates.**", '',
             'For declared component/environment inputs, reference locations, bindings and middleware imports, '
             'query [the machine-readable atlas](research/engine-atlas.json):', '', '```powershell',
             'python tools/engine_atlas.py query docs/research/engine-atlas.json system coregame::spawning',
             'python tools/engine_atlas.py query docs/research/engine-atlas.json component LuaScript',
             'python tools/engine_atlas.py query docs/research/engine-atlas.json binding event',
             '```', '', 'See [engine atlas](engine-atlas.md) for subsystem responsibilities and evidence levels.', '']
    for family in atlas['families']:
        lines.extend([f'## {family}', '', '| System | Signature RVA | Candidate dispatcher RVA |',
                      '| --- | --- | --- |'])
        for row in atlas['systems']:
            if row['family'] != family:
                continue
            # Code spans preserve C++ templates and MSVC's single-backtick names.
            name = '`` ' + row['name'].replace('|', '&#124;') + ' ``'
            targets = ', '.join('`' + d['target_rva'] + '`' for d in row['dispatch_candidates']) or 'Unresolved'
            lines.append(f"| {name} | `{row['signature_rva']}` | {targets} |")
        lines.append('')
    return '\n'.join(lines)


def query(atlas, category, term):
    require(atlas.get('kind') == 'engine_atlas' and atlas.get('schema') == 1, 'Unsupported atlas')
    key = {'system': 'systems', 'component': 'systems', 'environment': 'systems',
           'binding': 'bindings', 'import': 'imports'}[category]
    needle = term.casefold()
    for row in atlas[key]:
        if category in ('component', 'environment'):
            entries = [entry['type'] for arg in row['arguments']
                       for entry in arg.get(category + 's', [])]
            if category == 'environment':
                entries.extend(arg['type'] for arg in row['arguments']
                               if '::env::' in arg['type'] or '::global::' in arg['type'])
            else:
                entries.extend(arg['type'] for arg in row['arguments']
                               if 'ecs::QueryRemoved<' in arg['type'])
            matches = any(needle in value.casefold() for value in entries)
        elif category == 'import':
            matches = needle in (row['module'] + ' ' + row.get('name', '')).casefold()
        else:
            matches = needle in row['name'].casefold()
        if matches:
            yield row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    build = sub.add_parser('build', help='Verify source evidence and create an index')
    build.add_argument('executable', type=Path)
    build.add_argument('catalog', type=Path)
    build.add_argument('--output', type=Path, required=True)
    build.add_argument('--index', type=Path)
    search = sub.add_parser('query', help='Search a generated index')
    search.add_argument('atlas', type=Path)
    search.add_argument('category', choices=('system', 'component', 'environment', 'binding', 'import'))
    search.add_argument('term')
    search.add_argument('--limit', type=int, default=10)
    args = parser.parse_args()
    try:
        if args.command == 'build':
            destinations = [args.output] + ([args.index] if args.index else [])
            for path in destinations:
                require(not path.resolve().is_relative_to(args.executable.resolve().parent),
                        'Output must be outside game directory')
                require(path.resolve() != args.catalog.resolve(), 'Output would overwrite source catalog')
            require(len({p.resolve() for p in destinations}) == len(destinations), 'Output paths must differ')
            catalog = json.loads(read_bounded(args.catalog, 512 * 1024 * 1024))
            atlas = build_atlas(read_bounded(args.executable, 256 * 1024 * 1024), catalog)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(atlas, indent=2) + '\n', encoding='utf-8')
            if args.index:
                args.index.parent.mkdir(parents=True, exist_ok=True)
                args.index.write_text(index_markdown(atlas), encoding='utf-8')
            print(json.dumps(atlas['summary']))
        else:
            require(1 <= args.limit <= 1000, 'Limit must be between 1 and 1000')
            atlas = json.loads(read_bounded(args.atlas, 64 * 1024 * 1024))
            count = 0
            for row in query(atlas, args.category, args.term):
                if count < args.limit:
                    print(json.dumps(row))
                count += 1
            print(f'{count} matches; displayed {min(count, args.limit)}.')
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Atlas failed: {error}\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
