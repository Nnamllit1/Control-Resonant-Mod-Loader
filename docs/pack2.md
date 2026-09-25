# Pack2 index format

These notes describe the COTR version-3 indexes observed in the [engine research baseline](engine-research.md). The implementation is `tools/engine_research.py`. Every table range, file block total, typed metadata range, and referenced blob size was checked across all 25 installed indexes. This is an observed format description, not a promise that another game build uses it unchanged.

All integers below are little-endian. Offsets in the outer header address the **decoded payload**, except the wrapper block table offset. String records use byte offsets and byte lengths into a shared UTF-8 table; strings are not NUL-terminated.

## Outer header: 88 bytes

| Offset | Type | Observed meaning |
| --- | --- | --- |
| `0x00` | 4 bytes | `COTR` |
| `0x04` | u32 | Version: 3 |
| `0x08` | u32 | Wrapper block table file offset: 88 |
| `0x0c` | u32 | Wrapper block table byte size, a multiple of 16 |
| `0x10` | u32 | Zero in baseline; meaning unknown |
| `0x14` | u32 | Blob record count; records start at payload offset zero |
| `0x18`, `0x1c` | u32, u32 | Directory table offset and record count |
| `0x20`, `0x24` | u32, u32 | File table offset and record count |
| `0x28`, `0x2c` | u32, u32 | String table offset and byte size |
| `0x30`, `0x34` | u32, u32 | Metadata type-name table offset and record count |
| `0x38`, `0x3c` | u32, u32 | Asset metadata area offset and byte size |
| `0x40`–`0x4c` | four u32 | Zero in baseline; meanings unknown |
| `0x50`, `0x54` | u32, u32 | Asset block table offset and byte size |

Each wrapper block record is `<u64 packed, u32 decoded_size, u32 compressed_size>`. Its physical file offset is `packed >> 24`; the low 24 bits are `0x10` in all baseline wrapper records. Decode each block independently and concatenate in table order. Padding between physical blocks is not part of the compressed input.

The block codec matches the [upstream LZ4 block specification](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md). The scanner implements bounded literal and overlapping-match decoding. Exact decoded lengths must match the descriptors. This is raw block compression, with framing and sizes supplied by Pack2.

## Decoded tables

### Blob record: 24 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00`, `0x04` | u32, u32 | Relative blob path string offset and length |
| `0x08` | u64 | Identity value; hash algorithm/semantics unknown |
| `0x10` | u64 | Physical blob file size |

Paths are relative to the index's directory. `../generic/...` legitimately crosses from `pc` into the sibling asset directory. The scanner resolves references within `data_pack2`, checks physical sizes, and refuses paths outside that tree.

### Directory record: 28 bytes

Seven u32 fields: parent directory index, first child directory, child count, first file index, file count, name string offset, name string length.

Record zero is a sentinel root. Its name fields do not represent a virtual path segment. Other directory records use parent indices preceding the child. Child and file ranges must agree with the corresponding parent fields and may not overlap. Combining parent names reconstructs virtual paths such as `data/lua_scripts/systems.binlua`.

### File record: 32 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00`, `0x04` | u32, u32 | Offset and byte size within asset block table |
| `0x08` | u32 | Parent directory index |
| `0x0c`, `0x10` | u32, u32 | Filename string offset and length |
| `0x14` | u32 | Logical file size; equals sum of decoded block sizes |
| `0x18`, `0x1c` | u32, u32 | Offset and byte size within asset metadata area |

The word at `0x18` is a separate offset, not the upper half of a 64-bit file size.

### Asset block record: 16 bytes

The layout is `<u64 packed, u32 decoded_size, u32 compressed_size>`:

```text
codec       = packed & 0xff
blob_index  = (packed >> 8) & 0xffff
blob_offset = packed >> 24
```

Codec `0x10` denotes an LZ4 block. Codec zero denotes uncompressed bytes, with the compressed-size field set to zero; read `decoded_size` bytes for those blocks. Other codecs are rejected. Offsets are 40-bit values and can exceed 4 GiB. Blob numbering comes from the index table, not lexicographic filename order.

The catalog validates block bounds using physical blob sizes. Header sampling separately decoded the first blocks of a script, CSS resource, and compiled UI page; the survey does not decode every asset payload.

### Typed metadata

The type-name table contains pairs of u32 string offset and string length. Nonempty per-file metadata begins with `DMKP`, followed by a u32 entry count. Each entry is `<u32 type_index, u16 begin, u16 end>`. The data body starts immediately after the eight-byte header and all eight-byte entries. Begin/end address byte ranges within that body.

The scanner validates these ranges and resolves type names. Field layouts inside each type remain unknown. Some assets have no metadata. Observed types include `r::FileInfoMetadata`, `r::ResourceMetadata`, `content::LuaScriptMetadata`, `ui::PageMetadata`, `ui::CSSMetadata`, `physics::CollisionPackageMetadata`, and `puppet::GraphMetadata`.

## Worked script location

For baseline `data/lua_scripts/systems.binlua`:

| Field | Value |
| --- | --- |
| Index | `data_pack2/pc/base-generic.rmdtoc` |
| Logical size | 1,630 bytes |
| Blob table index | 0 |
| Blob path | `../pc/base-generic-000.rmdblob` |
| Blob offset | 344,308,416 |
| Stored block size | 1,133 bytes |
| Codec | `0x10` |
| Resource-specific metadata | `content::LuaScriptMetadata` |

These coordinates are evidence for that installation, not stable offsets to embed in the runtime. Rebuild the catalog after an update. Payload checksums, mount precedence, resource identity algorithms, repacking, and hot reload remain unverified.
