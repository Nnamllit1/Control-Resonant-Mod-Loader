"""Self-authored bytecode fixtures; no game installation or script payloads."""
import contextlib
import argparse
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import binlua as bl
from binlua_source import reconstruct, quote

LUAU_COMPILER = None
LUAU_VM = None


def var(n):
    data = bytearray()
    while n > 127:
        data.append((n & 127) | 128)
        n >>= 7
    data.append(n)
    return bytes(data)


def ins(op, a=0, b=0, c=0, d=None):
    return bl.OPCODES.index(op) | (a << 8) | ((d & 65535) << 16 if d is not None else (b << 16) | (c << 24))


def prototype(code, constants=(), children=(), name=0, line=1, stack=8, params=0, ups=0, debug=b'\0', lineinfo=b'\0'):
    return (bytes([stack, params, ups, 0, 0]) + var(0) + var(len(code)) + struct.pack('<' + str(len(code)) + 'I', *code) +
            var(len(constants)) + b''.join(constants) + var(len(children)) + b''.join(var(p) for p in children) +
            var(line) + var(name) + lineinfo + debug)


def chunk(protos, strings=(), envelope=0, main=None):
    return (bytes([envelope, 6, 3]) + var(len(strings)) + b''.join(var(len(s)) + s for s in strings) + b'\0' +
            var(len(protos)) + b''.join(protos) + var(len(protos) - 1 if main is None else main))


class BinluaTests(unittest.TestCase):
    def sample(self):
        return chunk([prototype([ins('LOADN', d=7), ins('RETURN', b=2)], name=1)], [b'answer'])

    def test_container_and_disassembly(self):
        doc = bl.parse(self.sample())
        self.assertEqual(doc['prototypes'][0]['instructions'][0]['d'], 7)
        self.assertIn('name="answer"', bl.disassemble(doc))
        self.assertEqual(bl.summary(doc)['instructions'], 2)
        self.assertEqual(bl.parse(self.sample()[1:], envelope=False)['envelope'], None)
        altered = bytearray(self.sample());altered[0] = 1
        self.assertEqual(bl.parse(altered)['envelope'], 1)

    def test_all_truncations_and_trailing_data_rejected(self):
        data = self.sample()
        for end in range(len(data)):
            with self.subTest(end=end), self.assertRaises(bl.FormatError):
                bl.parse(data[:end])
        with self.assertRaisesRegex(bl.FormatError, 'Trailing'):
            bl.parse(data + b'x')

    def test_limits_and_versions(self):
        for data in (b'\0\5\3', b'\0\6\2', b'\0\6\3\xff\xff\xff\xff\x1f', b'\0\6\3\x80\0', b'\0\6\3' + var(bl.MAX_ITEMS + 1)):
            with self.assertRaises(bl.FormatError):
                bl.parse(data)
        with patch.object(bl, 'MAX_BYTES', 3), self.assertRaisesRegex(bl.FormatError, 'limit'):
            bl.parse(self.sample())

    def test_binary_strings_constants_and_lines(self):
        constants = [b'\3' + var(1), b'\2' + struct.pack('<d', float('inf')), b'\7' + struct.pack('<4f', 1, 2, 3, 4)]
        # Offset deltas accumulate modulo 256; interval base deltas also accumulate.
        lines = b'\1\0' + bytes([2, 255, 1]) + struct.pack('<3i', 10, 10, -5)
        p = prototype([ins('LOADK', d=0), ins('LOADK', a=1, d=1), ins('RETURN', b=1)], constants, lineinfo=lines)
        doc = bl.parse(chunk([p], [b'a\0\xff']))
        self.assertEqual(doc['strings'][0]['hex'], '6100ff')
        self.assertEqual(doc['prototypes'][0]['lines'], [12, 21, 17])
        self.assertEqual(doc['prototypes'][0]['constants'][1]['value'], {'nonfinite': 'inf'})
        json.dumps(doc, allow_nan=False)
        self.assertEqual(quote(b'"\n\xff1'), '"\\034\\010\\2551"')

    def test_references_and_operand_bounds(self):
        for p in (prototype([ins('LOADK', d=4)]), prototype([ins('LOADN', a=8)]),
                  prototype([ins('GETUPVAL')]), prototype([ins('GETGLOBAL'), 0]),
                  prototype([ins('RETURN', b=1)], children=[1]),
                  prototype([ins('RETURN', b=1)], name=2),
                  prototype([ins('RETURN', b=1)], constants=[b'\6\0']),
                  prototype([ins('RETURN', b=1)], constants=[b'\x08'])):
            with self.assertRaises(bl.FormatError):
                bl.parse(chunk([p]))
        doc = bl.parse(chunk([prototype([ins('RETURN', b=1)], stack=0)]))
        self.assertEqual(doc['prototypes'][0]['max_stack'], 0)

    def test_aux_jumps_and_fastcall(self):
        for code in ([ins('GETGLOBAL')], [ins('JUMP', d=1), ins('GETGLOBAL'), 0, ins('RETURN', b=1)],
                     [ins('JUMP', d=50)], [255], [ins('FASTCALL', c=0), ins('RETURN', b=1)]):
            with self.assertRaises(bl.FormatError):
                bl.parse(chunk([prototype(code, [b'\3\1'])], [b'key']))
        code = [ins('FASTCALL1', b=1, c=0), ins('CALL', b=2, c=1), ins('RETURN', b=1)]
        self.assertEqual(bl.parse(chunk([prototype(code)]))['prototypes'][0]['instructions'][0]['call_pc'], 1)

    def test_both_closure_capture_forms(self):
        child = prototype([ins('GETUPVAL'), ins('RETURN', b=2)], ups=1)
        for op in ('NEWCLOSURE', 'DUPCLOSURE'):
            parent = prototype([ins(op, d=0), ins('CAPTURE', a=0, b=1), ins('RETURN', b=2)],
                               [b'\6\0'] if op == 'DUPCLOSURE' else [], children=[0])
            self.assertEqual(len(bl.parse(chunk([child, parent]))['prototypes']), 2)
        for code in ([ins('NEWCLOSURE', d=0), ins('RETURN', b=1)], [ins('CAPTURE')]):
            with self.assertRaises(bl.FormatError):
                bl.parse(chunk([child, prototype(code, children=[0])]))

    def test_debug_bounds(self):
        debug = b'\1\1\1\0\2\0\0' # One local: string1, [0,2), register0; no upvalues.
        p = prototype([ins('LOADN'), ins('RETURN', b=1)], debug=debug)
        self.assertEqual(bl.parse(chunk([p], [b'x']))['prototypes'][0]['locals'][0]['end_pc'], 2)
        with self.assertRaises(bl.FormatError):
            bl.parse(chunk([prototype([ins('RETURN', b=1)], debug=debug)], [b'x']))

    def test_source_fragments_and_refusal(self):
        result = reconstruct(bl.parse(self.sample()), 0)
        self.assertEqual(result['status'], 'reconstructed_fragment')
        self.assertIn('r[0] = 7', result['source'])
        code = [ins('JUMPIFNOT', d=2), ins('LOADN', a=1, d=1), ins('JUMP', d=1), ins('LOADN', a=1, d=2), ins('RETURN', a=1, b=2)]
        result = reconstruct(bl.parse(chunk([prototype(code, params=1)])), 0)
        self.assertIn('else', result['source'])
        for code in ([ins('FORNPREP', d=0), ins('RETURN', b=1)],
                     [ins('CALL', b=0, c=0), ins('RETURN', b=0)],
                     [ins('JUMPBACK', d=-1)]):
            result = reconstruct(bl.parse(chunk([prototype(code)])), 0)
            self.assertEqual(result['status'], 'unsupported')
            self.assertIsNone(result['source'])
        # Overlapping branch ranges cannot be silently rearranged.
        code = [ins('JUMPIF', d=2), ins('JUMPIF', d=3), ins('LOADN'), ins('LOADN'), ins('LOADN'), ins('RETURN', b=1)]
        self.assertIsNone(reconstruct(bl.parse(chunk([prototype(code)])), 0)['source'])

    def test_pack_read_and_path_containment(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp);(root / 'pack').mkdir();(root / 'data.blob').write_bytes(b'ABC')
            pack = {'blobs': [{'path': '../data.blob'}]}
            entry = {'size': 3, 'blocks': [{'blob': 0, 'offset': 0, 'stored_size': 3, 'decoded_size': 3, 'codec': 0}]}
            self.assertEqual(bl.asset_bytes(root, 'pack/test.toc', pack, entry), b'ABC')
            pack['blobs'][0]['path'] = '../../escape.blob'
            with self.assertRaisesRegex(bl.FormatError, 'leaves'):
                bl.asset_bytes(root, 'pack/test.toc', pack, entry)
            pack['blobs'][0]['path'] = '../data.blob';entry['blocks'][0]['offset'] = 1
            with self.assertRaises(bl.FormatError):
                bl.asset_bytes(root, 'pack/test.toc', pack, entry)

    def test_cli_no_overwrite_or_partial_source(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stderr(io.StringIO()):
            root = Path(temp);source = root / 'input.binlua';source.write_bytes(self.sample())
            self.assertEqual(bl.main([str(source), '--output', str(source)]), 1)
            self.assertEqual(source.read_bytes(), self.sample())
            target = root / 'nested' / 'source.lua'
            self.assertEqual(bl.main([str(source), '--format', 'source', '--prototype', '0', '--output', str(target)]), 0)
            before = target.read_bytes()
            source.write_bytes(chunk([prototype([ins('CALL', b=0, c=0), ins('RETURN', b=1)])]))
            self.assertEqual(bl.main([str(source), '--format', 'source', '--prototype', '0', '--output', str(target)]), 1)
            self.assertEqual(target.read_bytes(), before)

    def test_optional_compiler_behavior_comparison(self):
        if not LUAU_COMPILER or not LUAU_VM:
            self.skipTest('Pass --compiler and --vm to compare self-authored source with reconstructed fragments')
        fixtures = [
            ('arithmetic', '', 'return a+b, a-b, a*b, a/b, a%b, a^b, -a, a//b', '{{2,3},{-4,2},{7,-2}}'),
            ('branch', '', 'if a < b then return a+10 else return b-10 end', '{{2,3},{3,2},{2,2},{0/0,1}}'),
            ('nested', '', 'if a then if b then return 1 else return 2 end else return 3 end', '{{true,true},{true,false},{false,true},{0,false}}'),
            ('imports', '', 'return math.abs(a) + math.floor(b)', '{{-2,3.5},{4,-1.5}}'),
            ('table', '', 'local t={} t.x=a t[1]=b t[a]=b return t.x,t[1],t[a]', '{{2,3},{4,5}}'),
            ('upvalue', 'local total=10\n', 'total=total+a return total', '{{2,3},{4,5},{-1,0}}'),
            ('global', '', 'counter=a return counter', '{{2,3},{4,5}}'),
            ('strings', '', 'return "a\\000\\255", a .. "x" .. b', '{{"a","b"},{"z",""}}'),
            ('truth', '', 'return a and b, a or b', '{{false,9},{0,3},{true,false}}'),
        ]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for level in (0, 1):
                for name, setup, body, cases in fixtures:
                    with self.subTest(name=name, optimization=level):
                        original = setup + 'local function f(a,b)\n' + body + '\nend\nreturn f\n'
                        path = root / 'original.luau';path.write_text(original, encoding='utf-8')
                        bytecode = subprocess.check_output([str(LUAU_COMPILER), '--binary', f'-O{level}', '-g2', str(path)], timeout=20)
                        doc = bl.parse(bytecode, envelope=False)
                        p = next(p for p in doc['prototypes'] if p['name_string'] is not None and doc['strings'][p['name_string']]['text'] == 'f')
                        fragment = reconstruct(doc, p['id'])
                        self.assertIsNotNone(fragment['source'], fragment['reasons'])
                        imports = []
                        for index, c in enumerate(p['constants']):
                            if c['kind'] == 'import':
                                parts = [bytes.fromhex(doc['strings'][s]['hex']) for s in c['path_strings']]
                                # Only this test's controlled standard-library imports.
                                self.assertEqual(parts[0], b'math')
                                imports.append(f'[{index}]=math' + ''.join('[' + quote(part) + ']' for part in parts[1:]))
                        runner = ('local original=(function()\n' + original + '\nend)()\n' +
                                  'local factory=(function()\n' + fragment['source'] + '\nend)()\n' +
                                  'local reconstructed=factory({math=math}, {[0]={value=10}}, {' + ','.join(imports) + '})\n' +
                                  'for _, args in ipairs(' + cases + ') do\n' +
                                  ' local expected=table.pack(original(table.unpack(args)))\n' +
                                  ' local actual=table.pack(reconstructed(table.unpack(args)))\n' +
                                  ' assert(expected.n==actual.n, "result count")\n' +
                                  ' for i=1,expected.n do assert(expected[i]==actual[i] or (expected[i]~=expected[i] and actual[i]~=actual[i]), "result "..i) end\nend\nprint("equivalent")\n')
                        test = root / 'compare.luau';test.write_text(runner, encoding='utf-8')
                        result = subprocess.run([str(LUAU_VM), str(test)], capture_output=True, text=True, timeout=20)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stdout.strip(), 'equivalent')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--vm', type=Path)
    options, remaining = parser.parse_known_args()
    if bool(options.compiler) != bool(options.vm):
        parser.error('--compiler and --vm must be supplied together')
    LUAU_COMPILER, LUAU_VM = options.compiler, options.vm
    unittest.main(argv=[sys.argv[0]] + remaining)
