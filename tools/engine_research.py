"""Read-only engine research: Pack2 metadata and static executable evidence.

Only writes a JSON report to the requested output, never into the game directory.
No game code is executed. Optional samples record headers, never asset files.
"""
import argparse
from bisect import bisect_right
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import struct

MAX_TOC_SIZE = 128 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def lz4_block(data, expected_size):
    """Decode an independent raw LZ4 block with bounded input/output access."""
    require(0 <= expected_size <= MAX_TOC_SIZE, 'LZ4 output exceeds size limit')
    output = bytearray()
    cursor = 0

    def length(initial):
        nonlocal cursor
        value = initial
        if initial == 15:
            while True:
                require(cursor < len(data), 'Truncated LZ4 length')
                extra = data[cursor]
                cursor += 1
                value += extra
                require(value <= expected_size, 'LZ4 length exceeds output size')
                if extra != 255:
                    break
        return value

    while cursor < len(data):
        token = data[cursor]
        cursor += 1
        literals = length(token >> 4)
        require(cursor + literals <= len(data), 'Truncated LZ4 literals')
        require(len(output) + literals <= expected_size, 'LZ4 output overflow')
        output.extend(data[cursor:cursor + literals])
        cursor += literals
        if cursor == len(data):
            break
        require(cursor + 2 <= len(data), 'Truncated LZ4 offset')
        distance = int.from_bytes(data[cursor:cursor + 2], 'little')
        cursor += 2
        require(0 < distance <= len(output), 'Invalid LZ4 back reference')
        count = length(token & 15) + 4
        require(len(output) + count <= expected_size, 'LZ4 output overflow')
        # Repeating the initial window also handles overlapping matches.
        window = output[-distance:]
        output.extend((window * ((count + distance - 1) // distance))[:count])
    require(len(output) == expected_size, 'LZ4 output size mismatch')
    return bytes(output)


def decode_toc(data):
    """Observed COTR v3 wrapper; unsupported encodings fail explicitly."""
    require(88 <= len(data) <= MAX_TOC_SIZE, 'Invalid TOC file size')
    header = struct.unpack_from('<22I', data)
    require(data[:4] == b'COTR' and header[1] == 3, 'Expected COTR version 3')
    require(header[4] == 0 and not any(header[16:20]), 'Unsupported TOC header fields')
    table_at, table_size = header[2:4]
    require(table_at == 88 and table_size > 0 and table_size % 16 == 0,
            'Unsupported TOC block table')
    require(table_at + table_size <= len(data), 'Truncated TOC block table')
    blocks = []
    total = 0
    previous_end = table_at + table_size
    for at in range(table_at, table_at + table_size, 16):
        packed, unpacked, stored = struct.unpack_from('<QII', data, at)
        offset, flags = packed >> 24, packed & 0xffffff
        require(flags == 0x10, f'Unsupported TOC block flags: {flags:#x}')
        require(unpacked > 0 and stored > 0, 'Empty TOC block')
        require(offset >= previous_end and offset + stored <= len(data),
                'Overlapping or out-of-range TOC block')
        total += unpacked
        require(total <= MAX_TOC_SIZE, 'TOC decoded size exceeds limit')
        blocks.append({'offset': offset, 'flags': flags, 'decoded_size': unpacked,
                       'stored_size': stored})
        previous_end = offset + stored
    payload = b''.join(lz4_block(data[b['offset']:b['offset'] + b['stored_size']],
                                 b['decoded_size']) for b in blocks)
    return header, blocks, payload


def inspect_toc(data):
    header, blocks, payload = decode_toc(data)
    blob_count, dirs_at, dir_count, files_at, file_count = header[5:10]
    strings_at, strings_size, types_at, type_count, metadata_at, metadata_size = header[10:16]
    chunks_at, chunks_size = header[20:22]
    regions = [(0, blob_count * 24), (dirs_at, dir_count * 28),
               (files_at, file_count * 32), (strings_at, strings_size),
               (types_at, type_count * 8), (metadata_at, metadata_size),
               (chunks_at, chunks_size)]
    end = 0
    for offset, size in regions:
        require(offset >= end and offset + size <= len(payload), 'Invalid TOC region')
        end = offset + size
    require(end == len(payload), 'Unaccounted TOC payload tail')

    def string(offset, size):
        require(offset + size <= strings_size, 'TOC string outside table')
        return payload[strings_at + offset:strings_at + offset + size].decode('utf-8')

    blobs = []
    for index in range(blob_count):
        name, length, identity, size = struct.unpack_from('<IIQQ', payload, index * 24)
        blobs.append({'path': string(name, length), 'identity_raw': f'{identity:016x}',
                      'stored_size': size})
    types = [string(*struct.unpack_from('<II', payload, types_at + i * 8))
             for i in range(type_count)]
    dirs = [struct.unpack_from('<7I', payload, dirs_at + i * 28) for i in range(dir_count)]
    require(bool(dirs) and dirs[0][0] == 0, 'Missing TOC root directory')
    # Record zero is a sentinel. Parent indices precede children in observed v3 indexes.
    paths = ['']
    owners = [-1] * file_count
    children = [-1] * dir_count
    for index, (parent, first_child, child_count, first_file, count, name, length) in enumerate(dirs):
        require(first_child + child_count <= dir_count and first_file + count <= file_count,
                'Invalid TOC directory range')
        if index:
            require(parent < index, 'Unsupported TOC directory parent order')
            part = string(name, length)
            require(part not in ('', '.', '..') and '/' not in part and '\\' not in part,
                    'Invalid TOC directory name')
            paths.append(f'{paths[parent]}/{part}'.lstrip('/'))
        for child in range(first_child, first_child + child_count):
            require(child > index and children[child] == -1 and dirs[child][0] == index,
                    'Inconsistent TOC child directory')
            children[child] = index
        for file in range(first_file, first_file + count):
            require(owners[file] == -1, 'Overlapping TOC file ranges')
            owners[file] = index
    require(all(parent >= 0 for parent in children[1:]), 'Orphan TOC directory')
    files = []
    for index in range(file_count):
        chunk_offset, chunk_bytes, parent, name, length, size, meta, meta_size = struct.unpack_from(
            '<8I', payload, files_at + index * 32)
        require(parent < dir_count and owners[index] == parent, 'Inconsistent TOC file parent')
        require(meta + meta_size <= metadata_size, 'File metadata outside table')
        require(chunk_offset % 16 == 0 and chunk_bytes % 16 == 0 and
                chunk_offset + chunk_bytes <= chunks_size, 'File blocks outside table')
        part = string(name, length)
        require(part not in ('', '.', '..') and '/' not in part and '\\' not in part,
                'Invalid TOC file name')
        file_blocks = []
        for block_at in range(chunks_at + chunk_offset, chunks_at + chunk_offset + chunk_bytes, 16):
            packed, decoded, stored = struct.unpack_from('<QII', payload, block_at)
            codec, blob, offset = packed & 255, (packed >> 8) & 65535, packed >> 24
            require(codec in (0, 16) and blob < blob_count, 'Unsupported asset block encoding')
            if codec == 0:
                require(stored == 0, 'Unsupported raw asset block size field')
                stored = decoded
            require(offset + stored <= blobs[blob]['stored_size'], 'Asset block outside blob')
            require(decoded > 0 and stored > 0, 'Invalid asset block sizes')
            file_blocks.append({'blob': blob, 'offset': offset, 'decoded_size': decoded,
                                'stored_size': stored, 'codec': codec})
        require(sum(b['decoded_size'] for b in file_blocks) == size, 'Asset size differs from block total')
        file_types = []
        if meta_size:
            metadata = payload[metadata_at + meta:metadata_at + meta + meta_size]
            require(meta_size >= 8 and metadata[:4] == b'DMKP', 'Unsupported asset metadata header')
            count = struct.unpack_from('<I', metadata, 4)[0]
            body = 8 + count * 8
            require(body <= meta_size, 'Truncated asset metadata entries')
            for entry in range(count):
                kind, begin, end = struct.unpack_from('<IHH', metadata, 8 + entry * 8)
                require(kind < type_count and begin <= end and body + end <= meta_size,
                        'Invalid typed asset metadata range')
                file_types.append(types[kind])
        files.append({'path': f'{paths[parent]}/{part}'.lstrip('/'), 'size': size,
                      'metadata_offset': meta, 'metadata_size': meta_size,
                      'block_table_offset': chunk_offset, 'block_table_size': chunk_bytes,
                      'metadata_types': file_types, 'blocks': file_blocks})
    return {'sha256': hashlib.sha256(data).hexdigest(), 'version': 3,
            'decoded_size': len(payload), 'blocks': blocks, 'blobs': blobs,
            'directory_count': dir_count, 'file_count': file_count,
            'metadata_types': types,
            'extensions': dict(sorted(Counter(Path(f['path']).suffix.lower() or '(none)'
                                              for f in files).items())),
            'files': files}


class PE:
    """Minimal, bounds-checked PE32+ file view; all addresses in reports are RVAs."""
    def __init__(self, data):
        self.data = data
        require(len(data) >= 64 and data[:2] == b'MZ', 'Expected DOS header')
        pe = self.unpack('<I', 60)[0]
        require(self.slice(pe, 4) == b'PE\0\0', 'Expected PE signature')
        machine, count = self.unpack('<HH', pe + 4)
        optional_size = self.unpack('<H', pe + 20)[0]
        optional = pe + 24
        require(machine == 0x8664 and optional_size >= 112 and
                self.unpack('<H', optional)[0] == 0x20b, 'Expected x64 PE32+')
        self.slice(optional, optional_size)
        self.image_base = self.unpack('<Q', optional + 24)[0]
        directory_count = self.unpack('<I', optional + 108)[0]
        require(directory_count <= (optional_size - 112) // 8, 'Truncated PE directories')
        self.directories = [self.unpack('<II', optional + 112 + i * 8)
                            for i in range(directory_count)]
        self.sections = []
        for i in range(count):
            at = optional + optional_size + i * 40
            name = self.slice(at, 8).split(b'\0', 1)[0].decode('ascii', errors='replace')
            virtual_size, rva, size, raw = self.unpack('<IIII', at + 8)
            flags = self.unpack('<I', at + 36)[0]
            self.slice(raw, size)
            self.sections.append({'name': name, 'rva': rva, 'raw': raw, 'size': size,
                                  'virtual_size': virtual_size, 'executable': bool(flags & 0x20000000)})
        self.functions = []
        if len(self.directories) > 3:
            rva, size = self.directories[3]
            if rva and size:
                require(size % 12 == 0, 'Invalid PE exception table')
                at = self.offset(rva, size)
                self.functions = sorted(struct.unpack_from('<III', data, at + i)
                                        for i in range(0, size, 12))
        self.starts = [f[0] for f in self.functions]

    def slice(self, at, size):
        require(0 <= at <= len(self.data) and 0 <= size <= len(self.data) - at,
                'Read outside PE file')
        return self.data[at:at + size]

    def unpack(self, fmt, at):
        return struct.unpack(fmt, self.slice(at, struct.calcsize(fmt)))

    def section(self, rva, size=1):
        for s in self.sections:
            if s['rva'] <= rva and rva + size <= s['rva'] + s['size']:
                return s
        raise ValueError(f'RVA outside file-backed sections: {rva:#x}')

    def offset(self, rva, size=1):
        s = self.section(rva, size)
        return s['raw'] + rva - s['rva']

    def cstring(self, rva, limit=4096):
        s = self.section(rva)
        at = self.offset(rva)
        end = self.data.find(b'\0', at, min(at + limit, s['raw'] + s['size']))
        require(end >= 0, 'Unterminated PE string')
        return self.data[at:end].decode('ascii')

    def function(self, rva):
        index = bisect_right(self.starts, rva) - 1
        if index >= 0:
            start, end, _ = self.functions[index]
            if start <= rva < end:
                return {'begin_rva': start, 'end_rva': end}
        return None

    def imports(self):
        if len(self.directories) <= 1 or not self.directories[1][0]:
            return []
        rva, size = self.directories[1]
        at = self.offset(rva, size)
        result = []
        for index in range(size // 20):
            original, stamp, chain, name, thunk = self.unpack('<5I', at + index * 20)
            if not any((original, stamp, chain, name, thunk)):
                return result
            result.append(self.cstring(name))
        raise ValueError('Unterminated PE import directory')


ANCHOR_TERMS = (b'pack2', b'packfilemanager', b'lualibs', b'luascriptresource',
                b'luastate', b'lua 5.', b'luajit', b'luavm', b'luamanager',
                b'scriptmanager', b'gameface', b'cohtml', b'luaevents')


def inspect_executable(data):
    pe = PE(data)
    anchors, systems, bindings = [], [], []
    for section in pe.sections:
        if section['executable']:
            continue
        raw, size = section['raw'], section['size']
        for match in re.finditer(rb'[ -~]{5,}', data[raw:raw + size]):
            value = match.group()
            rva = section['rva'] + match.start()
            if len(value) <= 32768 and b'ecs::' in value and b'getName<' in value:
                systems.append({'rva': rva, 'signature': value.decode('ascii'), 'lea_candidates': []})
            elif len(value) <= 2048 and any(term in value.lower() for term in ANCHOR_TERMS):
                anchors.append({'rva': rva, 'text': value.decode('ascii'), 'lea_candidates': []})
    targets = {a['rva']: a for a in anchors + systems}
    pattern = rb'\x48\x8d\x05....\x48\x89\x85....\x48\x8d\x05....\x48\x89\x85....'
    for section in pe.sections:
        if not section['executable']:
            continue
        code = data[section['raw']:section['raw'] + section['size']]
        for match in re.finditer(pattern, code, re.DOTALL):
            at = section['rva'] + match.start()
            name_rva = at + 7 + struct.unpack_from('<i', match.group(), 3)[0]
            callback = at + 21 + struct.unpack_from('<i', match.group(), 17)[0]
            try:
                name = pe.cstring(name_rva, 128)
                if (re.fullmatch('[a-z][a-z0-9_]+', name) and
                        pe.section(callback)['executable'] and not pe.section(name_rva)['executable']):
                    bindings.append({'name': name, 'name_rva': name_rva, 'callback_rva': callback,
                                     'registration_rva': at, 'confidence': 'pattern_candidate'})
            except (ValueError, UnicodeDecodeError):
                continue
        for match in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]....', code, re.DOTALL):
            at = section['rva'] + match.start()
            target = at + 7 + struct.unpack_from('<i', match.group(), 3)[0]
            if target in targets:
                targets[target]['lea_candidates'].append({'rva': at, 'runtime_function': pe.function(at)})
    return {'sha256': hashlib.sha256(data).hexdigest(), 'image_base': pe.image_base,
            'imports': pe.imports(), 'sections': pe.sections,
            'anchors': anchors, 'ecs_systems': systems, 'binding_candidates': bindings,
            'note': 'Byte-pattern references can include false positives. Runtime-function ranges may be fragments. '
                    'No calling conventions, signatures, thread ownership, or binding completeness established.'}


def read_bounded(path, limit):
    require(path.stat().st_size <= limit, f'File exceeds research size limit: {path.name}')
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, f'File changed beyond size limit: {path.name}')
    return data


def survey(root, sample_paths=()):
    executable = root / 'CONTROLResonant.exe'
    result = {'schema': 1, 'executable': inspect_executable(read_bounded(executable, 256 * 1024 * 1024)),
              'inventory': [], 'packs': [], 'libraries': [], 'header_samples': [], 'errors': []}
    for name in ('cohtml.WindowsDesktop.dll', 'v8.dll'):
        path = root / name
        if path.is_file():
            data = read_bounded(path, 256 * 1024 * 1024)
            result['libraries'].append({'path': name, 'sha256': hashlib.sha256(data).hexdigest(),
                                        'imports': PE(data).imports()})
    for folder in ('data', 'data_pack2'):
        base = root / folder
        if not base.is_dir():
            result['errors'].append({'path': folder, 'error': 'Directory missing'})
            continue
        def walk_error(error):
            result['errors'].append({'path': folder, 'error': str(error)})

        for directory, dirs, names in os.walk(base, followlinks=False, onerror=walk_error):
            dirs[:] = sorted(d for d in dirs if (Path(directory) / d).resolve().is_relative_to(root))
            for name in sorted(names):
                path = Path(directory) / name
                relative = path.relative_to(root).as_posix()
                if not path.resolve().is_relative_to(root):
                    result['errors'].append({'path': relative, 'error': 'Link outside game root'})
                    continue
                try:
                    result['inventory'].append({'path': relative, 'size': path.stat().st_size})
                    if path.suffix.lower() == '.rmdtoc':
                        report = inspect_toc(read_bounded(path, MAX_TOC_SIZE))
                        report['path'] = relative
                        for blob in report['blobs']:
                            target = (path.parent / blob['path']).resolve()
                            require(target.is_relative_to(root / 'data_pack2'), 'Blob reference escapes asset directory')
                            blob['resolved_path'] = target.relative_to(root).as_posix()
                            blob['size_matches'] = target.is_file() and target.stat().st_size == blob['stored_size']
                            if not blob['size_matches']:
                                result['errors'].append({'path': relative, 'error': 'Missing or size-mismatched blob: ' + blob['path']})
                        result['packs'].append(report)
                except (OSError, ValueError) as error:
                    result['errors'].append({'path': relative, 'error': str(error)})
    for virtual_path in dict.fromkeys(sample_paths):
        found = False
        for pack in result['packs']:
            for file in pack['files']:
                if file['path'] != virtual_path:
                    continue
                found = True
                try:
                    require(bool(file['blocks']), 'Asset is empty')
                    block = file['blocks'][0]
                    require(max(block['decoded_size'], block['stored_size']) <= 16 * 1024 * 1024,
                            'First block exceeds header-sampling size limit')
                    path = (root / pack['path']).parent / pack['blobs'][block['blob']]['path']
                    require(path.resolve().is_relative_to(root / 'data_pack2'), 'Sample path escapes assets')
                    with path.open('rb') as stream:
                        stream.seek(block['offset'])
                        data = stream.read(block['stored_size'])
                    require(len(data) == block['stored_size'], 'Truncated asset block')
                    if block['codec'] == 16:
                        data = lz4_block(data, block['decoded_size'])
                    result['header_samples'].append({'path': virtual_path, 'pack': pack['path'],
                                                     'first_16_bytes_hex': data[:16].hex(),
                                                     'first_block_sha256': hashlib.sha256(data).hexdigest(),
                                                     'first_block_decoded_size': len(data)})
                except (OSError, ValueError) as error:
                    result['errors'].append({'path': virtual_path, 'error': str(error)})
        if not found:
            result['errors'].append({'path': virtual_path, 'error': 'Requested sample not found'})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path, help='Game installation directory')
    parser.add_argument('--output', type=Path, required=True, help='JSON report outside game directory')
    parser.add_argument('--sample-header', action='append', default=[], metavar='VIRTUAL_PATH',
                        help='Read one named asset first block and report 16 header bytes; repeat up to 16 times')
    args = parser.parse_args()
    root, output = args.game.resolve(), args.output.resolve()
    if output.is_relative_to(root):
        parser.error('Report output must be outside the game directory')
    if output.exists():
        parser.error('Report already exists; choose a new output filename')
    if len(args.sample_header) > 16:
        parser.error('At most 16 header samples are allowed')
    try:
        result = survey(root, args.sample_header)
    except (OSError, ValueError) as error:
        parser.exit(1, f'Research failed: {error}\n')
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    exe = result['executable']
    print(f"Executable SHA-256: {exe['sha256']}")
    print(f"Packs: {len(result['packs'])}; file records: {sum(p['file_count'] for p in result['packs'])}")
    print(f"Binding candidates: {len(exe['binding_candidates'])}; ECS signatures: {len(exe['ecs_systems'])}")
    print(f"Errors: {len(result['errors'])}; report: {output}")
    return 1 if result['errors'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
