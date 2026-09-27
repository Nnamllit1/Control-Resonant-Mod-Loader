"""Bounded offline inspection of the observed Northlight/Luau binlua format.

No VM execution. Opcode facts follow Luau Common/include/Luau/Bytecode.h;
container interpretation follows the reviewed engine loader. See docs/binlua.md.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

from engine_research import MAX_TOC_SIZE, inspect_toc, lz4_block, read_bounded

MAX_BYTES = 16 * 1024 * 1024
MAX_ITEMS = 1_000_000
OPCODES = """NOP BREAK LOADNIL LOADB LOADN LOADK MOVE GETGLOBAL SETGLOBAL
GETUPVAL SETUPVAL CLOSEUPVALS GETIMPORT GETTABLE SETTABLE GETTABLEKS SETTABLEKS
GETTABLEN SETTABLEN NEWCLOSURE NAMECALL CALL RETURN JUMP JUMPBACK JUMPIF JUMPIFNOT
JUMPIFEQ JUMPIFLE JUMPIFLT JUMPIFNOTEQ JUMPIFNOTLE JUMPIFNOTLT ADD SUB MUL DIV MOD POW
ADDK SUBK MULK DIVK MODK POWK AND OR ANDK ORK CONCAT NOT MINUS LENGTH NEWTABLE
DUPTABLE SETLIST FORNPREP FORNLOOP FORGLOOP FORGPREP_INEXT FASTCALL3 FORGPREP_NEXT
NATIVECALL GETVARARGS DUPCLOSURE PREPVARARGS LOADKX JUMPX FASTCALL COVERAGE CAPTURE
SUBRK DIVRK FASTCALL1 FASTCALL2 FASTCALL2K FORGPREP JUMPXEQKNIL JUMPXEQKB JUMPXEQKN
JUMPXEQKS IDIV IDIVK""".split()
AUX_OPS = set("""GETGLOBAL SETGLOBAL GETIMPORT GETTABLEKS SETTABLEKS NAMECALL
JUMPIFEQ JUMPIFLE JUMPIFLT JUMPIFNOTEQ JUMPIFNOTLE JUMPIFNOTLT NEWTABLE SETLIST
FORGLOOP FASTCALL3 LOADKX FASTCALL2 FASTCALL2K JUMPXEQKNIL JUMPXEQKB JUMPXEQKN JUMPXEQKS""".split())
D_JUMPS = set("""JUMP JUMPBACK JUMPIF JUMPIFNOT JUMPIFEQ JUMPIFLE JUMPIFLT
JUMPIFNOTEQ JUMPIFNOTLE JUMPIFNOTLT FORNPREP FORNLOOP FORGLOOP FORGPREP_INEXT
FORGPREP_NEXT FORGPREP JUMPXEQKNIL JUMPXEQKB JUMPXEQKN JUMPXEQKS""".split())


class FormatError(ValueError):
    pass


def check(condition, message):
    if not condition:
        raise FormatError(message)


class Reader:
    def __init__(self, data):
        check(len(data) <= MAX_BYTES, 'Input exceeds 16 MiB limit')
        self.data, self.at, self.items = data, 0, 0

    def take(self, size):
        check(0 <= size <= len(self.data) - self.at, f'Truncated field at byte {self.at}')
        result = self.data[self.at:self.at + size]
        self.at += size
        return result

    def byte(self):
        return self.take(1)[0]

    def flag(self):
        value = self.byte()
        check(value in (0, 1), f'Invalid boolean at byte {self.at - 1}')
        return bool(value)

    def unpack(self, fmt):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))

    def varint(self):
        value = 0
        for shift in range(0, 35, 7):
            b = self.byte()
            check(shift != 28 or b <= 15, 'Varint exceeds uint32')
            value |= (b & 127) << shift
            if b < 128:
                check(shift == 0 or b != 0, 'Noncanonical varint')
                return value
        raise FormatError('Unterminated varint')

    def count(self, minimum=1):
        value = self.varint()
        self.items += value
        check(self.items <= MAX_ITEMS, 'Aggregate item limit exceeded')
        check(value <= (len(self.data) - self.at) // minimum, 'Count exceeds remaining input')
        return value


def number(value):
    # JSON must remain valid and preserve unusual floating-point constants.
    return value if math.isfinite(value) else {'nonfinite': 'nan' if math.isnan(value) else 'inf' if value > 0 else '-inf'}


def parse(data, envelope=True):
    r = Reader(data)
    prefix = r.byte() if envelope else None
    version = r.byte()
    check(version == 6, f'Unsupported bytecode version {version}; supported profile is 6')
    types = r.byte()
    check(types == 3, f'Unsupported type version {types}; supported profile is 3')
    strings = [r.take(r.varint()) for _ in range(r.count())]

    def string(index, optional=False):
        if index == 0 and optional:
            return None
        check(1 <= index <= len(strings), 'String reference outside table')
        return index - 1

    userdata, seen = [], set()
    while (index := r.byte()) != 0:
        check(index not in seen, 'Duplicate userdata remapping index')
        seen.add(index)
        userdata.append({'index': index, 'string': string(r.varint())})
    protos = []
    for pid in range(r.count()):
        p = {'id': pid, 'max_stack': r.byte(), 'parameters': r.byte(), 'upvalues': r.byte(),
             'vararg': r.flag(), 'flags': r.byte()}
        check(p['parameters'] <= p['max_stack'] <= 255, 'Invalid prototype stack size')
        p['type_info_hex'] = r.take(r.varint()).hex()
        size = r.count(4)
        p['code'] = list(r.unpack('<' + str(size) + 'I'))
        constants = []
        for _ in range(r.count()):
            tag = r.byte()
            if tag == 0:
                c = {'kind': 'nil'}
            elif tag == 1:
                c = {'kind': 'boolean', 'value': r.flag()}
            elif tag == 2:
                c = {'kind': 'number', 'value': number(r.unpack('<d')[0])}
            elif tag == 3:
                c = {'kind': 'string', 'string': string(r.varint())}
            elif tag == 4:
                c = {'kind': 'import', 'packed': r.unpack('<I')[0]}
            elif tag == 5:
                c = {'kind': 'table', 'keys': [r.varint() for _ in range(r.count())]}
            elif tag == 6:
                c = {'kind': 'closure', 'prototype': r.varint()}
                check(c['prototype'] < pid, 'Closure must reference an earlier prototype')
            elif tag == 7:
                c = {'kind': 'vector', 'value': [number(v) for v in r.unpack('<4f')]}
            else:
                raise FormatError(f'Unsupported constant tag {tag} at byte {r.at - 1}')
            constants.append(c)
        p['constants'] = constants
        p['children'] = [r.varint() for _ in range(r.count())]
        check(all(child < pid for child in p['children']), 'Child must reference an earlier prototype')
        p['defined_line'], p['name_string'] = r.varint(), string(r.varint(), optional=True)
        p['lines'] = None
        if r.flag():
            gap = r.byte()
            check(gap <= 31 and size > 0, 'Invalid line-info interval')
            deltas = r.take(size)
            base_deltas = r.unpack('<' + str(((size - 1) >> gap) + 1) + 'i')
            bases, last_line = [], 0
            for delta in base_deltas:
                last_line += delta
                bases.append(last_line)
            offset, lines = 0, []
            for pc, delta in enumerate(deltas):
                offset = (offset + delta) & 255
                lines.append(bases[pc >> gap] + offset)
            p['lines'] = lines
        p['locals'], p['upvalue_names'] = [], []
        if r.flag():
            for _ in range(r.count(4)):
                local = {'name_string': string(r.varint(), optional=True), 'start_pc': r.varint(),
                         'end_pc': r.varint(), 'register': r.byte()}
                check(local['start_pc'] <= local['end_pc'] <= size and local['register'] < p['max_stack'], 'Invalid local debug range')
                p['locals'].append(local)
            p['upvalue_names'] = [string(r.varint(), optional=True) for _ in range(r.count())]
            check(len(p['upvalue_names']) == p['upvalues'], 'Upvalue debug count differs from prototype')
        for c in constants:
            if c['kind'] == 'table':
                check(all(k < len(constants) for k in c['keys']), 'Table key reference outside constants')
            elif c['kind'] == 'import':
                packed, path = c['packed'], []
                check(1 <= packed >> 30 <= 3, 'Invalid import path length')
                for i in range(packed >> 30):
                    k = (packed >> (20 - i * 10)) & 1023
                    check(k < len(constants) and constants[k]['kind'] == 'string', 'Import path must reference string constants')
                    path.append(constants[k]['string'])
                c['path_strings'] = path
        protos.append(p)
    main = r.varint()
    check(main < len(protos), 'Main prototype outside table')
    check(r.at == len(data), f'Trailing data at byte {r.at}')
    doc = {'schema': 1, 'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data), 'envelope': prefix,
           'bytecode_version': version, 'type_version': types, 'main': main,
           'strings': [{'text': s.decode('utf-8', errors='backslashreplace'), 'hex': s.hex()} for s in strings],
           'userdata_types': userdata, 'prototypes': protos}
    for p in protos:
        p['instructions'] = instructions(p, protos)
    return doc


def instructions(p, protos):
    code, result, pc = p['code'], [], 0
    while pc < len(code):
        word = code[pc]
        op = word & 255
        check(op < len(OPCODES), f'Unknown opcode {op} in prototype {p["id"]} at pc {pc}')
        name = OPCODES[op]
        size = 2 if name in AUX_OPS else 1
        check(pc + size <= len(code), f'Missing AUX for {name} at pc {pc}')
        i = {'pc': pc, 'op': name, 'a': (word >> 8) & 255, 'b': (word >> 16) & 255,
             'c': word >> 24, 'd': (word >> 16) - (65536 if word & 0x80000000 else 0),
             'e': (word >> 8) - (16777216 if word & 0x80000000 else 0), 'size': size}
        if size == 2:
            i['aux'] = code[pc + 1]
        if p['lines'] is not None:
            i['line'] = p['lines'][pc]
        if name in D_JUMPS:
            i['target'] = pc + 1 + i['d']
        elif name == 'JUMPX':
            i['target'] = pc + 1 + i['e']
        elif name == 'LOADB' and i['c']:
            i['target'] = pc + 1 + i['c']
        elif name.startswith('FASTCALL'):
            i['call_pc'] = pc + 1 + i['c']
        result.append(i)
        pc += size
    by_pc = {i['pc']: i for i in result}
    capture_pcs = set()

    def constant(k, kind=None):
        check(0 <= k < len(p['constants']), f'Constant reference outside prototype {p["id"]}')
        c = p['constants'][k]
        check(kind is None or c['kind'] == kind, f'Expected {kind} constant in prototype {p["id"]}')
        return c

    for i in result:
        op, a, b, c, d, aux = (i['op'], i['a'], i['b'], i['c'], i['d'], i.get('aux'))
        registers = []
        if op not in ('NOP', 'BREAK', 'NATIVECALL', 'COVERAGE', 'CAPTURE', 'PREPVARARGS', 'JUMP', 'JUMPBACK', 'JUMPX', 'CLOSEUPVALS') and not op.startswith('FASTCALL'):
            if op not in ('RETURN', 'GETVARARGS') or b != 1:
                registers.append(a)
        if op in ('MOVE', 'GETTABLE', 'SETTABLE', 'GETTABLEKS', 'SETTABLEKS', 'GETTABLEN', 'SETTABLEN', 'NAMECALL', 'NOT', 'MINUS', 'LENGTH', 'CONCAT', 'ADD', 'SUB', 'MUL', 'DIV', 'MOD', 'POW', 'AND', 'OR', 'IDIV', 'ADDK', 'SUBK', 'MULK', 'DIVK', 'MODK', 'POWK', 'ANDK', 'ORK', 'IDIVK'):
            registers.append(b)
        if op in ('GETTABLE', 'SETTABLE', 'CONCAT', 'ADD', 'SUB', 'MUL', 'DIV', 'MOD', 'POW', 'AND', 'OR', 'IDIV', 'SUBRK', 'DIVRK'):
            registers.append(c)
        if op in ('JUMPIFEQ', 'JUMPIFLE', 'JUMPIFLT', 'JUMPIFNOTEQ', 'JUMPIFNOTLE', 'JUMPIFNOTLT'):
            registers.append(aux & 255)
        if op in ('FASTCALL1', 'FASTCALL2', 'FASTCALL2K', 'FASTCALL3'):
            registers.append(b)
        if op in ('FASTCALL2', 'FASTCALL3'):
            registers.append(aux & 255)
        if op == 'FASTCALL3':
            registers.append((aux >> 8) & 255)
        if op in ('FORNPREP', 'FORNLOOP', 'FORGPREP', 'FORGPREP_NEXT', 'FORGPREP_INEXT'):
            registers.append(a + 2)
        if op == 'FORGLOOP':
            check(aux & 255, 'Generic loop has no output variables')
            registers.append(a + 2 + (aux & 255))
        if op == 'NAMECALL':
            registers.append(a + 1)
        if op in ('CALL', 'RETURN', 'GETVARARGS') and b:
            if op == 'CALL':
                registers.append(a + b - 1)
            elif b > 1:
                registers.append(a + b - 2)
        if op == 'CALL' and c > 1:
            registers.append(a + c - 2)
        if op == 'SETLIST':
            registers.append(b)
            if c > 1:
                registers.append(b + c - 2)
        check(all(reg < p['max_stack'] for reg in registers), f'Register outside prototype {p["id"]} at pc {i["pc"]}: {op}, registers {registers}, stack {p["max_stack"]}')
        if op in ('LOADK', 'DUPCLOSURE', 'DUPTABLE', 'GETIMPORT'):
            constant(d, {'DUPCLOSURE': 'closure', 'DUPTABLE': 'table', 'GETIMPORT': 'import'}.get(op))
        if op == 'GETIMPORT':
            check(aux == constant(d)['packed'], 'Import instruction and constant disagree')
        if op in ('GETGLOBAL', 'SETGLOBAL', 'GETTABLEKS', 'SETTABLEKS', 'NAMECALL'):
            constant(aux, 'string')
        if op in ('LOADKX', 'FASTCALL2K'):
            constant(aux)
        if op in ('ADDK', 'SUBK', 'MULK', 'DIVK', 'MODK', 'POWK', 'ANDK', 'ORK', 'IDIVK'):
            constant(c)
        if op in ('SUBRK', 'DIVRK'):
            constant(b)
        if op in ('JUMPXEQKN', 'JUMPXEQKS'):
            constant(aux & 0xffffff, 'number' if op == 'JUMPXEQKN' else 'string')
        if op in ('NEWCLOSURE', 'DUPCLOSURE'):
            if op == 'NEWCLOSURE':
                check(0 <= d < len(p['children']), 'Child closure reference outside prototype')
                child = protos[p['children'][d]]
            else:
                child = protos[constant(d, 'closure')['prototype']]
            for n in range(child['upvalues']):
                at = i['pc'] + 1 + n
                check(at in by_pc and by_pc[at]['op'] == 'CAPTURE', 'Missing closure capture')
                capture_pcs.add(at)
        if op in ('GETUPVAL', 'SETUPVAL'):
            check(b < p['upvalues'], 'Upvalue reference outside prototype')
        if op == 'CAPTURE':
            check(a <= 2 and b < (p['upvalues'] if a == 2 else p['max_stack']), 'Invalid capture operand')
    for i in result:
        if i['op'] == 'CAPTURE':
            check(i['pc'] in capture_pcs, 'Orphan closure capture')
        if 'target' in i:
            check(i['target'] in by_pc and i['target'] not in capture_pcs, f'Jump is not an instruction boundary at pc {i["pc"]}')
        if 'call_pc' in i:
            check(i['call_pc'] in by_pc and by_pc[i['call_pc']]['op'] == 'CALL', 'FASTCALL fallback does not point to CALL')
    return result


def summary(doc):
    ops = Counter(i['op'] for p in doc['prototypes'] for i in p['instructions'])
    return {key: doc[key] for key in ('schema', 'sha256', 'size', 'envelope', 'bytecode_version', 'type_version', 'main')} | {
        'strings': len(doc['strings']), 'prototypes': len(doc['prototypes']), 'instructions': sum(ops.values()), 'opcodes': dict(sorted(ops.items()))}


def disassemble(doc):
    lines = ['; CRML binlua disassembly; offline only, not executable source', '; SHA256 ' + doc['sha256']]
    for p in doc['prototypes']:
        name = doc['strings'][p['name_string']]['text'] if p['name_string'] is not None else ''
        lines.append(f'\nPROTO {p["id"]} name={json.dumps(name)} defined_line={p["defined_line"]} params={p["parameters"]} upvalues={p["upvalues"]}')
        for k, c in enumerate(p['constants']):
            value = dict(c)
            if c['kind'] == 'string':
                value['text'] = doc['strings'][c['string']]['text']
            if c['kind'] == 'import':
                value['path'] = [doc['strings'][s]['text'] for s in c['path_strings']]
            lines.append(f'  K{k} {json.dumps(value, ensure_ascii=True, allow_nan=False)}')
        for i in p['instructions']:
            fields = ' '.join(f'{k}={v}' for k, v in i.items() if k not in ('pc', 'op'))
            lines.append(f'  {i["pc"]:6} {i["op"]:18} {fields}')
    return '\n'.join(lines) + '\n'


def inside(root, candidate):
    path = (root / candidate).resolve()
    check(path.is_relative_to(root), 'Pack path leaves game directory')
    return path


def asset_bytes(root, toc_path, pack, entry):
    root = Path(root).resolve()
    toc = inside(root, toc_path)
    check(0 < entry['size'] <= MAX_BYTES, 'Asset exceeds size limit or is empty')
    result = bytearray()
    for block in entry['blocks']:
        path = inside(root, toc.parent / pack['blobs'][block['blob']]['path'])
        size, stored, offset = block['decoded_size'], block['stored_size'], block['offset']
        check(0 < size <= MAX_BYTES - len(result) and 0 < stored <= MAX_BYTES, 'Asset block exceeds limits')
        check(0 <= offset <= path.stat().st_size - stored, 'Asset block outside physical blob')
        with path.open('rb') as stream:
            stream.seek(offset)
            data = stream.read(stored)
        check(len(data) == stored, 'Short blob read')
        check(block['codec'] in (0, 16), 'Unsupported asset block codec')
        decoded = lz4_block(data, size) if block['codec'] == 16 else data
        check(len(decoded) == size, 'Asset block size mismatch')
        result.extend(decoded)
    check(len(result) == entry['size'], 'Asset size mismatch')
    return bytes(result)


def main(argv=None):
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('input', nargs='?', type=Path, help='Local binlua file')
    cli.add_argument('--game-dir', type=Path)
    cli.add_argument('--toc', type=Path, help='Pack TOC relative to game directory')
    cli.add_argument('--asset', help='Exact virtual asset path in the TOC')
    cli.add_argument('--raw-bytecode', action='store_true', help='Input has no resource envelope')
    cli.add_argument('--format', choices=('summary', 'json', 'disasm', 'source'), default='summary')
    cli.add_argument('--prototype', type=int, help='Prototype index for --format source')
    cli.add_argument('--output', type=Path, help='Write local output; prefer an ignored directory')
    args = cli.parse_args(argv)
    try:
        if args.input:
            check(not any((args.game_dir, args.toc, args.asset)), 'Choose a local file OR game/TOC/asset')
            data = read_bounded(args.input, MAX_BYTES)
        else:
            check(all((args.game_dir, args.toc, args.asset)), 'Specify input file or --game-dir, --toc and --asset')
            root = args.game_dir.resolve()
            pack = inspect_toc(read_bounded(inside(root, args.toc), MAX_TOC_SIZE))
            matches = [f for f in pack['files'] if f['path'] == args.asset]
            check(len(matches) == 1, 'Asset must match exactly one TOC entry')
            data = asset_bytes(root, args.toc, pack, matches[0])
        doc = parse(data, envelope=not args.raw_bytecode)
        if args.format == 'source':
            from binlua_source import reconstruct
            check(args.prototype is not None and 0 <= args.prototype < len(doc['prototypes']), 'Source output requires a valid --prototype')
            result = reconstruct(doc, args.prototype)
            check(result['source'] is not None, 'Source reconstruction unavailable: ' + json.dumps(result['reasons']))
            text = result['source']
        else:
            check(args.prototype is None, '--prototype requires --format source')
            text = disassemble(doc) if args.format == 'disasm' else json.dumps(summary(doc) if args.format == 'summary' else doc, indent=2, ensure_ascii=True, allow_nan=False) + '\n'
        if args.output:
            output = args.output.resolve()
            check(not args.input or output != args.input.resolve(), 'Output would overwrite input')
            check(not args.game_dir or not output.is_relative_to(args.game_dir.resolve()), 'Output must be outside the game directory')
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(text, encoding='utf-8')
        else:
            print(text, end='')
        return 0
    except (OSError, ValueError, KeyError, IndexError, struct.error) as error:
        # OSError paths can contain user information; details stay off stdout.
        print('binlua: ' + ('File access failed' if isinstance(error, OSError) else str(error)), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
