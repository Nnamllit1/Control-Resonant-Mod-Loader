"""Synthetic declaration, reference, and boundary tests; no game files required."""
import contextlib
import copy
import hashlib
import io
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import engine_atlas as atlas
from engine_research import PE, inspect_executable
from test_engine_research import pe_fixture


def signature(body):
    return 'char const * __cdecl ecs::system::getName<void __cdecl ' + body + '>(void)'


class DeclarationTests(unittest.TestCase):
    def test_nested_function_pointer_and_access_entries(self):
        text = signature('game::update<struct Context<void (__cdecl *)(int,float)> >('
            'struct ecs::Query<struct ecs::access::Definition<'
            'struct ecs::access::DefinitionComponentEntry<struct game::Pair<int,float>,2,1>,'
            'struct ecs::access::DefinitionEnvironmentEntry<struct game::env::Clock,3> > >,'
            'class ecs::QueryRemoved<struct game::Gone>,float)')
        name, args = atlas.parse_system(text)
        self.assertIn('void (__cdecl *)(int,float)', name)
        self.assertEqual(len(args), 3)
        self.assertEqual(args[0]['components'], [{'type': 'struct game::Pair<int,float>',
                                               'access_code': 2, 'qualifier_code': 1}])
        self.assertEqual(args[0]['environments'][0]['access_code'], 3)
        self.assertEqual(args[2], {'type': 'float'})

    def test_invalid_declarations_fail(self):
        for text in ['f(int)', signature('f(A<B)'), signature('f(A>B)')]:
            with self.subTest(text=text), self.assertRaises(ValueError):
                atlas.parse_system(text)

    def test_queries_and_markdown_preserve_template_names(self):
        row = {'name': "game::`anonymous namespace'::update<Pair<int,float> >",
               'family': "game::`anonymous namespace'", 'signature_rva': '0x2000',
               'dispatch_candidates': [], 'arguments': [
                   {'type': 'ecs::QueryRemoved<game::Gone>'},
                   {'type': 'game::env::Clock &'}]}
        data = {'schema': 1, 'kind': 'engine_atlas', 'systems': [row], 'bindings': [],
                'imports': [], 'families': {row['family']: 1},
                'summary': {'systems': 1, 'families': 1, 'bindings': 0}}
        self.assertEqual(list(atlas.query(data, 'component', 'gone')), [row])
        self.assertEqual(list(atlas.query(data, 'environment', 'clock')), [row])
        self.assertIn('`` ' + row['name'] + ' ``', atlas.index_markdown(data))


class EvidenceTests(unittest.TestCase):
    def test_binding_source_validation(self):
        data = pe_fixture()
        catalog = inspect_executable(data)
        result = atlas.build_atlas(data, catalog)
        self.assertEqual(result['bindings'][0]['callback_rva'], '0x1080')
        bad = copy.deepcopy(catalog)
        bad['binding_candidates'][0]['callback_rva'] = 0x1090
        with self.assertRaisesRegex(ValueError, 'callback reference'):
            atlas.build_atlas(data, bad)
        data[0x207] = 0x90
        with self.assertRaisesRegex(ValueError, 'fingerprint'):
            atlas.build_atlas(data, catalog)
        catalog['sha256'] = hashlib.sha256(data).hexdigest()
        with self.assertRaisesRegex(ValueError, 'stores differ'):
            atlas.build_atlas(data, catalog)

    def test_system_string_and_reference_validation(self):
        data = pe_fixture()
        value = signature('game::tick(void)')
        data[0x450:0x450 + len(value) + 1] = value.encode() + b'\0'
        data[0x240:0x247] = b'\x48\x8d\x05' + struct.pack('<i', 0x2050 - 0x1047)
        catalog = {'sha256': hashlib.sha256(data).hexdigest(), 'binding_candidates': [],
                   'ecs_systems': [{'signature': value, 'rva': 0x2050,
                                    'lea_candidates': [{'rva': 0x1040}]}]}
        self.assertEqual(atlas.build_atlas(data, catalog)['systems'][0]['name'], 'game::tick')
        catalog['ecs_systems'][0]['lea_candidates'][0]['rva'] = 0x1000
        with self.assertRaisesRegex(ValueError, 'name reference'):
            atlas.build_atlas(data, catalog)
        catalog['ecs_systems'][0]['signature'] = value.replace('tick', 'tock')
        with self.assertRaisesRegex(ValueError, 'signature differs'):
            atlas.build_atlas(data, catalog)

    def test_dispatch_stops_at_next_registration_or_function_end(self):
        data = pe_fixture()
        data[0x220:0x22e] = (b'\x48\x8d\x05' + struct.pack('<i', 0x1080 - 0x1027) +
                            b'\x48\x89\x83\x40\x01\x00\x00')
        pe = PE(data)
        pe.functions, pe.starts = [(0x1000, 0x1040, 0)], [0x1000]
        self.assertEqual(atlas.dispatch_candidates(pe, 0x1000, 0x1040)[0]['target_rva'], '0x1080')
        self.assertEqual(atlas.dispatch_candidates(pe, 0x1000, 0x1020), [])
        pe.functions = [(0x1000, 0x1020, 0)]
        self.assertEqual(atlas.dispatch_candidates(pe, 0x1000, 0x1100), [])

    def test_import_names_ordinals_and_termination(self):
        data = pe_fixture()
        struct.pack_into('<5I', data, 0x440, 0x2090, 0, 0, 0x2080, 0x20c0)
        data[0x480:0x486] = b'x.dll\0'
        struct.pack_into('<3Q', data, 0x490, 0x20e0, (1 << 63) | 7, 0)
        data[0x4e0:0x4e7] = b'\0\0test\0'
        pe = PE(data)
        pe.directories = [(0, 0), (0x2040, 40)]
        self.assertEqual(atlas.imported_functions(pe), [
            {'module': 'x.dll', 'name': 'test', 'iat_rva': '0x20c0'},
            {'module': 'x.dll', 'ordinal': 7, 'iat_rva': '0x20c8'}])
        pe.directories[1] = (0x2040, 20)
        with self.assertRaisesRegex(ValueError, 'descriptor'):
            atlas.imported_functions(pe)

    def test_output_checks_happen_before_file_reads(self):
        game = Path.cwd() / 'fake-game'
        for output, index in [(game / 'atlas.json', None),
                              (Path.cwd() / 'source.json', None),
                              (Path.cwd() / 'out.json', Path.cwd() / 'out.json')]:
            args = ['atlas', 'build', str(game / 'game.exe'), str(Path.cwd() / 'source.json'),
                    '--output', str(output)]
            if index:
                args += ['--index', str(index)]
            with patch.object(sys, 'argv', args), patch.object(atlas, 'read_bounded') as read, \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    atlas.main()
                read.assert_not_called()


if __name__ == '__main__':
    unittest.main()
