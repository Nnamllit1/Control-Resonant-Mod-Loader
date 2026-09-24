"""Read-only x64 PE imports and bounded gameplay-string leads; no game execution."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

def inspect(path):
    data = path.read_bytes()
    def u16(at): return struct.unpack_from('<H', data, at)[0]
    def u32(at): return struct.unpack_from('<I', data, at)[0]
    if data[:2] != b'MZ':
        raise ValueError('Not a PE executable')
    pe = u32(0x3C)
    if data[pe:pe + 4] != b'PE\0\0' or u16(pe + 4) != 0x8664 or u16(pe + 24) != 0x20B:
        raise ValueError('Expected a Windows x64 PE32+ executable')
    optional = pe + 24
    sections = []
    section_start = optional + u16(pe + 20)
    for i in range(u16(pe + 6)):
        start = section_start + i * 40
        size, rva, raw_size, raw = struct.unpack_from('<IIII', data, start + 8)
        sections.append((rva, max(size, raw_size), raw, raw_size))
    def offset(rva):
        for start, _, raw, raw_size in sections:
            if start <= rva < start + raw_size:
                return raw + rva - start
        raise ValueError(f'RVA outside file-backed sections: {rva:#x}')
    def rva_for(raw_offset):
        for rva, _, raw, size in sections:
            if raw <= raw_offset < raw + size:
                return rva + raw_offset - raw
        return None
    def string(at):
        end = data.find(b'\0', at, at + 4096)
        if end < 0:
            raise ValueError('Unterminated PE string')
        return data[at:end].decode('ascii', errors='replace')
    imports = []
    imports_rva = u32(optional + 112 + 8)
    if imports_rva:
        desc = offset(imports_rva)
        for i in range(4096):
            original, timestamp, chain, name, thunk = struct.unpack_from('<IIIII', data, desc + i * 20)
            if not any((original, timestamp, chain, name, thunk)):
                break
            symbols = []
            table = offset(original or thunk)
            for j in range(65536):
                value = struct.unpack_from('<Q', data, table + j * 8)[0]
                if not value:
                    break
                symbols.append({'ordinal': value & 0xFFFF} if value >> 63 else {'name': string(offset(value) + 2)})
            else:
                raise ValueError('Unterminated import thunk table')
            imports.append({'dll': string(offset(name)), 'symbols': symbols})
        else:
            raise ValueError('Unterminated import directory')
    leads = []
    terms = (b'noclip', b'no_clip', b'free_camera', b'freecamera', b'playercollision', b'flymode')
    for match in re.finditer(rb'[ -~]{5,}', data):
        text = match.group()
        if len(text) <= 160 and any(term in text.lower() for term in terms):
            leads.append({'text': text.decode('ascii'), 'rva': rva_for(match.start())})
        if len(leads) == 100:
            break
    return {'executable': path.name, 'sha256': hashlib.sha256(data).hexdigest(), 'architecture': 'x64',
            'imports': imports, 'gameplay_string_leads': leads,
            'note': 'Static evidence only. Strings do not establish a callable API or working noclip.'}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = inspect(args.executable)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f"{result['executable']}: {result['architecture']}\nSHA-256: {result['sha256']}")
    for library in result['imports']:
        if library['dll'].lower() in ('xinput1_4.dll', 'dxgi.dll', 'winmm.dll', 'sl.interposer.dll'):
            print(library['dll'] + ': ' + ', '.join(symbol.get('name', '#' + str(symbol.get('ordinal'))) for symbol in library['symbols']))
    print(f"Gameplay string leads: {len(result['gameplay_string_leads'])}; no callable gameplay API verified.")

if __name__ == '__main__':
    main()
