"""Synthetic fixtures only; no installed game or proprietary assets required."""
import contextlib
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import engine_research as research
from engine_query import matches


def literal_block(data):
    extra = len(data) - 15
    if extra < 0:
        return bytes([len(data) << 4]) + data
    return b'\xf0' + b'\xff' * (extra // 255) + bytes([extra % 255]) + data


def wrap(header, payload):
    encoded = literal_block(payload)
    return (struct.pack('<22I', *header) + struct.pack('<QII', (104 << 24) | 16,
                                                     len(payload), len(encoded)) + encoded)


def fixture():
    strings = bytearray()

    def add(value):
        at = len(strings)
        strings.extend(value.encode())
        return at, len(value)

    blob = add('../pc/test-000.rmdblob')
    directory = add('data')
    name = add('example.binlua')
    kind = add('content::LuaScriptMetadata')
    payload = bytearray(struct.pack('<IIQQ', *blob, 123, 21))
    dirs_at = len(payload)
    payload.extend(struct.pack('<7I', 0, 1, 1, 0, 0, *blob))
    payload.extend(struct.pack('<7I', 0, 0, 0, 0, 1, *directory))
    files_at = len(payload)
    payload.extend(struct.pack('<8I', 0, 16, 1, *name, 5, 0, 20))
    strings_at = len(payload)
    payload.extend(strings)
    types_at = len(payload)
    payload.extend(struct.pack('<II', *kind))
    metadata_at = len(payload)
    payload.extend(b'DMKP' + struct.pack('<IIHHI', 1, 0, 0, 4, 0))
    blocks_at = len(payload)
    payload.extend(struct.pack('<QII', 16 << 24, 5, 0))
    header = [int.from_bytes(b'COTR', 'little'), 3, 88, 16, 0, 1,
              dirs_at, 2, files_at, 1, strings_at, len(strings), types_at, 1,
              metadata_at, 20, 0, 0, 0, 0, blocks_at, 16]
    return header, payload


def pe_fixture():
    data = bytearray(0x600)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', data, 0x84, 0x8664, 2)
    struct.pack_into('<H', data, 0x94, 112)
    struct.pack_into('<H', data, 0x98, 0x20b)
    struct.pack_into('<Q', data, 0xb0, 0x140000000)
    for at, name, rva, raw, flags in [(0x108, b'.text', 0x1000, 0x200, 0x20000000),
                                       (0x130, b'.rdata', 0x2000, 0x400, 0x40000000)]:
        data[at:at + len(name)] = name
        struct.pack_into('<IIII', data, at + 8, 0x200, rva, 0x200, raw)
        struct.pack_into('<I', data, at + 36, flags)
    name = b'nl_resource_stream_in\0'
    data[0x400:0x400 + len(name)] = name
    data[0x200:0x21c] = (b'\x48\x8d\x05' + struct.pack('<i', 0x2000 - 0x1007) +
                         b'\x48\x89\x85' + b'\0' * 4 + b'\x48\x8d\x05' +
                         struct.pack('<i', 0x1080 - 0x1015) + b'\x48\x89\x85' + b'\0' * 4)
    return data


class LZ4Tests(unittest.TestCase):
    def test_literals_and_extended_lengths(self):
        for size in (0, 5, 15, 270, 4096):
            data = b'a' * size
            self.assertEqual(research.lz4_block(literal_block(data), size), data)

    def test_overlapping_match(self):
        self.assertEqual(research.lz4_block(b'\x17a\x01\x00\x50bcdef', 17), b'a' * 12 + b'bcdef')

    def test_invalid_blocks(self):
        for data, size in [(b'\xf0', 15), (b'\x30ab', 3), (b'\x10a\x00\x00', 5),
                           (b'\x10a\x02\x00', 5), (b'\x10a\x01', 5),
                           (b'\x10a\x01\x00', 4), (b'\x10a', 2), (b'\0', research.MAX_TOC_SIZE + 1)]:
            with self.subTest(data=data, size=size), self.assertRaises(ValueError):
                research.lz4_block(data, size)


class PackTests(unittest.TestCase):
    def test_paths_types_and_raw_blocks(self):
        report = research.inspect_toc(wrap(*fixture()))
        file = report['files'][0]
        self.assertEqual(file['path'], 'data/example.binlua')
        self.assertEqual(file['size'], 5)
        self.assertEqual(file['metadata_types'], ['content::LuaScriptMetadata'])
        self.assertEqual(file['blocks'][0]['stored_size'], 5)

    def test_compressed_asset_descriptor(self):
        h, p = fixture()
        struct.pack_into('<QII', p, h[20], (16 << 24) | 16, 5, 4)
        self.assertEqual(research.inspect_toc(wrap(h, p))['files'][0]['blocks'][0]['codec'], 16)

    def test_reject_bad_wrapper(self):
        valid = wrap(*fixture())
        for data in [valid[:30], b'XXXX' + valid[4:], valid[:4] + b'\x04\0\0\0' + valid[8:],
                     valid[:88] + b'\0' * 8 + valid[96:], valid[:-1]]:
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                research.inspect_toc(data)

    def test_reject_invalid_records(self):
        # Cyclic parent, bad file owner/name, metadata bounds/type, blob index/range,
        # mismatched logical size, child ownership and decoded table range.
        for region, delta, value in [(6, 28, 1), (8, 8, 0), (8, 12, 999999),
                                     (8, 24, 999999), (14, 8, 9), (20, 0, 0x100),
                                     (20, 4, 999999), (8, 20, 6), (6, 8, 0),
                                     (8, 0, 999999)]:
            h, p = fixture()
            struct.pack_into('<I', p, h[region] + delta, value)
            with self.subTest(region=region, delta=delta), self.assertRaises(ValueError):
                research.inspect_toc(wrap(h, p))


class ExecutableTests(unittest.TestCase):
    def test_binding_rvas(self):
        report = research.inspect_executable(pe_fixture())
        binding = report['binding_candidates'][0]
        self.assertEqual(binding['name'], 'nl_resource_stream_in')
        self.assertEqual(binding['callback_rva'], 0x1080)
        self.assertEqual(binding['registration_rva'], 0x1000)

    def test_reject_data_callback(self):
        data = pe_fixture()
        struct.pack_into('<i', data, 0x211, 0x2000 - 0x1015)
        self.assertEqual(research.inspect_executable(data)['binding_candidates'], [])

    def test_truncated_sections_and_headers(self):
        for end in (0, 63, 128, 255, 1024, 1535):
            with self.subTest(end=end), self.assertRaises(ValueError):
                research.PE(pe_fixture()[:end])


class ReportTests(unittest.TestCase):
    def test_query_preserves_pack_provenance(self):
        report = {'packs': [{'path': 'a.rmdtoc', 'files': [{'path': 'data/a.binlua'}]},
                            {'path': 'b.rmdtoc', 'files': [{'path': 'data/a.binlua'}]}]}
        self.assertEqual([m['pack'] for m in matches(report, 'asset', 'BINLUA')], ['a.rmdtoc', 'b.rmdtoc'])

    def test_output_in_game_rejected_before_scan(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(sys, 'argv', ['research', tmp, '--output', str(Path(tmp) / 'out.json')]), \
                    patch.object(research, 'survey') as scan, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    research.main()
                scan.assert_not_called()

    def test_survey_does_not_read_blob_contents(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            (root / 'CONTROLResonant.exe').write_bytes(pe_fixture())
            (root / 'data').mkdir()
            packs = root / 'data_pack2' / 'pc'
            packs.mkdir(parents=True)
            (packs / 'test.rmdtoc').write_bytes(wrap(*fixture()))
            (packs / 'test-000.rmdblob').write_bytes(b'x' * 21)
            with patch.object(research, 'read_bounded', wraps=research.read_bounded) as read:
                result = research.survey(root)
            self.assertEqual(result['errors'], [])
            self.assertEqual({c.args[0].suffix for c in read.call_args_list}, {'.exe', '.rmdtoc'})
            sampled = research.survey(root, ['data/example.binlua'])
            self.assertEqual(sampled['errors'], [])
            self.assertEqual(sampled['header_samples'][0]['first_16_bytes_hex'], (b'x' * 5).hex())
            missing = research.survey(root, ['missing.binlua'])
            self.assertEqual(missing['errors'][0]['error'], 'Requested sample not found')


if __name__ == '__main__':
    unittest.main()
