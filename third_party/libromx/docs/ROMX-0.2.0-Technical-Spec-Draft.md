# ROMX 0.2.0 Technical Specification Draft

> This is an evidence-based review document, not a new wire specification. It
> records the current ROMX standard, libromx headers and implementation, tests,
> and the adjacent reference repository. It does not change the C/C++ code.
>
> Evidence tags: `[SPEC]` is stated by `/Volumes/Repositories/romx/docs/ROMX-SPEC.md`
> or its Chinese counterpart; `[IMPLEMENTATION]` is current code in
> `/Volumes/Repositories/libromx`; `[TEST EVIDENCE]` is a test or reference
> fixture; `[RESOLVED]` marks a former release-gate item closed by the pinned
> provenance and decision records.

> **Decision-record note:** This file is maintained as the final release-gate
> baseline. Former release-gate items are closed by
> `ROMX-0.2.0-OPEN-ITEMS-RESOLVED.md`; the pinned wire snapshot is in
> `ROMX-0.2.0-PROVENANCE.md`.

## 0. Evidence boundary and version

- `[SPEC]` ROMX 0.2.0 uses footer wire version 2, RIDX version 1, metadata schema `0.2.0`; integers are unsigned little-endian and reserved fields/bits must be zero.
- `[IMPLEMENTATION]` `include/romx/romx.h` exposes the C99 ABI; `include/romx/romx.hpp` is an optional exception/RAII wrapper; the core has no GUI, emulator, or C++ STL dependency.
- `[TEST EVIDENCE]` `tests/test_v2.c` and `tests/test_writer_mutable.c` identify their coverage as ROMX 0.2.0; the adjacent fixtures record footer, regions, RIDX, CRCs and SHA-256.
- `[RESOLVED][SPEC][TEST EVIDENCE]` The normative markdown, schema and frozen fixtures remain in `/Volumes/Repositories/romx` rather than being vendored. Their source commit and SHA-256 values are pinned in `ROMX-0.2.0-PROVENANCE.md`; provenance and decision records are installed with the release for auditability.

## 1. Scope, goals and non-goals

- `[SPEC]` ROMX is a container with raw payload at file offset zero, a mandatory RIDX, optional metadata/PNG cover, optional fixed-capacity mutable region, and a 128-byte footer.
- `[IMPLEMENTATION]` Reader performs structural parsing, positional entry reads, VFS access and entrypoint mapping; writer streams one or more inputs, creates RIDX/metadata/cover/mutable/footer and atomically publishes a temporary file.
- `[IMPLEMENTATION]` Extraction writes the entrypoint bytes through a temporary file with optional durable/replace flags; cover is validated and copied as PNG without a mandatory image-conversion dependency.
- `[SPEC]` Payload bytes are preserved verbatim. Metadata `crc32` identifies the original ROM/entrypoint, not the ROMX file. Footer immutable SHA-256 covers immutable content only.
- `[SPEC]` The container does not define emulator launch, frontend directories, playlists, cloud sync, compression/encryption/patching, save-state format, or automatic mutable growth.
- `[IMPLEMENTATION]` Probe is best-effort metadata/cover assistance and never replaces footer/RIDX declarations.
- `[RESOLVED][SPEC][IMPLEMENTATION]` Salvage is explicitly outside the 0.2.0 libromx API. The normal reader is fail-closed and exposes no salvage handle; any future recovery tool must be separate, read-only, clearly unverified and unable to write mutable state or auto-launch.

## 2. Top-level physical layout

`[SPEC]` The mandatory order is payload → RIDX → metadata (optional) → cover
(optional) → zero immutable alignment padding (optional) → mutable region
(optional) → final 128-byte footer.

`[IMPLEMENTATION]` `src/footer.c` derives the footer offset and mutable start;
`src/ridx.c` derives RIDX and following regions. Payload starts at zero and
RIDX starts at `payload_size`.

| Region | Offset/size | Constraints and checks |
|---|---|---|
| Payload | `[SPEC]` offset 0, size `payload_size>0` | `[IMPLEMENTATION]` Every entry range is inside payload; unindexed bytes are zero; entrypoint offset is 0 |
| RIDX | `[SPEC]` offset `payload_size`, size `64+entry_count*512` | `[IMPLEMENTATION]` Header/index CRC/entries/path/range/overlap/gap checks |
| Metadata | `[SPEC]` immediately after RIDX, optional | `[IMPLEMENTATION]` Size limit, UTF-8/JSON/schema validation |
| Cover | `[SPEC]` immediately after metadata/RIDX, optional PNG | `[IMPLEMENTATION]` PNG chunk/CRC/size/dimension validation |
| Immutable padding | `[SPEC]` zero bytes before mutable start | `[IMPLEMENTATION]` Only when mutable exists; mutable start is 4096-aligned |
| Mutable | `[SPEC]` fixed capacity, 4096-aligned start, capacity multiple of 4096 and ≥12288 | `[IMPLEMENTATION]` RMUT/header/slot/extent checks; invalid mutable does not erase immutable access |
| Footer | `[SPEC]` final 128 bytes | `[IMPLEMENTATION]` Magic/version/CRC/range/hash/reserved checks |

`[TEST EVIDENCE]` `minimal-single` has payload 24592, RIDX 576 and footer
offset 25168. `single-complete` has metadata 68, cover 70, mutable offset
28672/capacity 12288 and footer offset 40960.

## 3. Footer — 128-byte field table

`[SPEC]` Footer CRC32 is computed over all 128 bytes with its own field at
`0x50..0x53` treated as zero.

| Offset | Size | Type | Field/value | Constraint/check |
|---:|---:|---|---|---|
| 0x00 | 4 | bytes/ASCII | `magic="ROMX"` | Exact match; implementation returns invalid-footer otherwise |
| 0x04 | 4 | uint32 LE | `wire_version=2` | Exact version 2 |
| 0x08 | 8 | uint64 LE | `payload_size` | Greater than zero; RIDX begins here; checked against footer |
| 0x10 | 8 | uint64 LE | `metadata_size` | Zero means absent; otherwise immediately follows RIDX |
| 0x18 | 8 | uint64 LE | `cover_size` | Zero means absent; otherwise follows metadata/RIDX |
| 0x20 | 8 | uint64 LE | `mutable_capacity` | Zero means absent; otherwise ≥12288, 4096 multiple, in bounds |
| 0x28 | 2 | uint16 LE | `platform_id` | 0 unspecified; standard 0x0001..0x7FFF; private 0x8000..0xFFFE; 0xFFFF prohibited |
| 0x2A | 2 | uint16 LE | `launch_format_id` | 0 unspecified; 1 RAW_SINGLE_FILE, 2 CUE, 3 GDI, 4 M3U, 5 CCD, 6 MDS, 7 TOC, 8 DIRECTORY, 9 ROMSET, 0xA SPLIT_FILE_SET |
| 0x2C | 4 | uint32 LE | `immutable_hash_algorithm` | 0 NONE or 1 SHA256; other values invalid |
| 0x30 | 32 | bytes | `immutable_sha256` | All zero for NONE; otherwise hash of `[0, mutable_offset)` or `[0, footer_offset)` |
| 0x50 | 4 | uint32 LE | `footer_crc32` | CRC-32/ISO-HDLC with this field zeroed |
| 0x54 | 44 | bytes | reserved | All zero |

`[IMPLEMENTATION]` `src/footer.c` rejects bad magic/version/CRC/reserved,
out-of-range payload/mutable values, unknown hash algorithms and 0xFFFF IDs.
`src/registry.c` exposes status helpers that classify values as KNOWN,
UNSPECIFIED, UNKNOWN, PRIVATE or PROHIBITED. Unknown non-zero values remain
numerically readable but unsupported by built-in policy; 0xFFFF is prohibited.

### Registry values

`[SPEC]` Platform values are GB `0x0001`, GBC `0x0002`, GBA `0x0003`, NES
`0x0004`, SNES `0x0005`, N64 `0x0006`, NDS `0x0007`, N3DS `0x0008`, Master
System `0x0010`, Game Gear `0x0011`, Mega Drive `0x0012`, 32X `0x0013`, Sega
CD `0x0014`, Saturn `0x0015`, Dreamcast `0x0016`, PC Engine `0x0020`, PC Engine
CD `0x0021`, PlayStation `0x0030`, PS2 `0x0031`, PSP `0x0032`, GameCube
`0x0040`, Wii `0x0041`, Arcade `0x0050`, ScummVM `0x0060`, DOS `0x0061`, and
Amiga `0x0062`; `0x0000` is unspecified.

`[SPEC]` Launch values are `0x0001` RAW_SINGLE_FILE, `0x0002` CUE, `0x0003`
GDI, `0x0004` M3U, `0x0005` CCD, `0x0006` MDS, `0x0007` TOC, `0x0008`
DIRECTORY, `0x0009` ROMSET, and `0x000A` SPLIT_FILE_SET; `0x0000` is
unspecified. Private values require a shared definition.

## 4. Payload and RIDX

### RIDX header — 64 bytes

| Offset | Size | Type | Field | Constraint/check |
|---:|---:|---|---|---|
| 0x00 | 4 | bytes | `magic="RIDX"` | Exact match |
| 0x04 | 2 | uint16 LE | `index_version=1` | Exact version 1 |
| 0x06 | 2 | uint16 LE | `header_size=64` | Fixed |
| 0x08 | 4 | uint32 LE | `entry_count` | ≥1; multiplication checked |
| 0x0C | 4 | uint32 LE | `entry_size=512` | Fixed |
| 0x10 | 4 | uint32 LE | flags | Zero |
| 0x14 | 4 | uint32 LE | `index_crc32` | Complete index with this field zeroed |
| 0x18 | 40 | bytes | reserved | All zero |

### RIDX entry — 512 bytes

| Offset | Size | Type | Field | Constraint/check |
|---:|---:|---|---|---|
| 0x00 | 4 | uint32 LE | flags | bit0 ENTRYPOINT, bit1 HAS_CRC32; other bits zero |
| 0x04 | 2 | uint16 LE | `format_id` | Entrypoint non-zero; 0xFFFF prohibited |
| 0x06 | 2 | uint16 LE | `path_size` | 1..480 bytes; not NUL-terminated on disk |
| 0x08 | 8 | uint64 LE | `data_offset` | Relative to payload; entrypoint must be 0 |
| 0x10 | 8 | uint64 LE | `data_size` | Non-zero; range inside payload |
| 0x18 | 4 | uint32 LE | `crc32` | Exact entry bytes when HAS_CRC32; otherwise zero |
| 0x1C | 4 | uint32 LE | reserved | Zero |
| 0x20 | 480 | bytes | virtual path | Strict relative UTF-8/NFC path, then zero padding |

`[SPEC]` Exactly one entrypoint is required. A single-file payload has one
entry covering the entire payload with no gap; a multi-file payload has more
than one entry. Paths use slash separators, reject NUL/backslash/empty/`.`/`..`
components and leading/trailing slash, and must not collide after Unicode case
folding. Descriptor references resolve to normalized relative paths.

`[IMPLEMENTATION]` `src/ridx.c` checks index length/CRC, flags, reserved bytes,
path bytes, entry ranges, exactly one entrypoint, overlap, zero gaps and the
metadata/cover/mutable boundary. `src/writer.c` writes the entrypoint first,
then other entries in caller order.

`[RESOLVED][SPEC][IMPLEMENTATION]` 0.2.0 accepts valid UTF-8, preserves
non-ASCII bytes, and applies ASCII A–Z folding for collision checks. Full
Unicode normalization/case folding is reserved for a future profile and is
not required for 0.2.0 interoperability.
`[TEST EVIDENCE]` `multi-cue.manifest.json` has a CUE entrypoint of 135 bytes at
offset 0 and two BIN entries at offsets 135 and 2695, all with CRC32.

## 5. Metadata

- `[SPEC]` Optional strict UTF-8/RFC 8259 JSON, no BOM, unique keys at every depth, schema version `0.2.0`.
- `[IMPLEMENTATION]` `src/metadata.c` validates UTF-8/BOM/JSON/duplicate keys/schema and enforces reader size limits.
- `[SPEC][TEST EVIDENCE]` Required fields are `schema_version` (constant `0.2.0`) and `name` (1..512). Optional fields match `schema/romx-metadata.schema.json`: bounded serial/origin/category/developer/publisher/franchise/language/enhancement_hw/media/description, arrays genre/region, users 1..255, coop/rumble/analog booleans, ISO-like release date, dump status enum, lowercase 8-hex `crc32`/`origin_crc32`, and cover `{mime_type:image/png,width,height}` with dimensions 1..8192.
- `[SPEC]` Metadata must not store payload/mutable offsets, host paths, launch paths, external cover paths, or platform/launch declarations. Footer/RIDX are authoritative for those values; metadata `crc32` is an identity lookup value.
- `[IMPLEMENTATION]` `romx_metadata_open`, `copy_json`, `get_string`, `get_crc32` and `get_value_json` return validated/copied values; absent/wrong type/size errors are explicit.
- `[TEST EVIDENCE]` `single-complete` has 68 metadata bytes; reader and writer tests read/write the `name` and reject invalid schemas.
- `[RESOLVED][SPEC][IMPLEMENTATION][TEST EVIDENCE]` The schema remains an external normative input; its source commit and SHA-256 are pinned in the provenance record, and the installed release documentation identifies that exact input.

## 6. Cover

- `[SPEC]` One PNG: IHDR first and unique, IDAT consecutive, zero-length IEND last; chunk bounds/CRC, legal color/depth, PLTE rules and unknown critical chunks are checked.
- `[IMPLEMENTATION]` `src/png.c` validates in chunks with default 32 MiB size and 8192-pixel dimension limits; it reports width/height and keeps cover validation independent from payload access.
- `[SPEC]` An invalid cover may be reported/ignored while valid payload/RIDX remain usable.
- `[TEST EVIDENCE]` `single-complete` contains a 70-byte single-pixel PNG; reader validation and writer/probe tests exercise cover handling.
- `[RESOLVED][SPEC][IMPLEMENTATION]` Wire/API scope is frozen at PNG copy and validation. Color conversion, resizing and display policy belong to the caller or an optional adapter and are not ROMX compatibility requirements.

## 7. Mutable region

### Mutable header — 4096 bytes

| Offset | Size | Type | Field | Constraint/check |
|---:|---:|---|---|---|
| 0x00 | 4 | bytes | `magic="RMUT"` | Exact match |
| 0x04 | 2 | uint16 LE | version=1 | Fixed |
| 0x06 | 2 | uint16 LE | header_size=4096 | Fixed |
| 0x08 | 4 | uint32 LE | entry_size=512 | Fixed |
| 0x0C | 4 | uint32 LE | entry_capacity | ≥8 and multiple of 8 |
| 0x10 | 8 | uint64 LE | directory_offset=4096 | Fixed, relative to mutable start |
| 0x18 | 8 | uint64 LE | directory_size | `entry_capacity*512` |
| 0x20 | 8 | uint64 LE | data_area_offset | `4096+directory_size`, 4096-aligned |
| 0x28 | 8 | uint64 LE | data_area_size | capacity minus data_area_offset, >0 |
| 0x30 | 4 | uint32 LE | flags | Zero |
| 0x34 | 4 | uint32 LE | header_crc32 | Full 4096 bytes with field zeroed |
| 0x38 | 4040 | bytes | reserved | All zero |

### Mutable directory entry — 512 bytes

| Offset | Size | Type | Field | Constraint/check |
|---:|---:|---|---|---|
| 0x00 | 4 | bytes | `magic="MENT"` | Empty slot is all zero |
| 0x04 | 2 | uint16 LE | state | ACTIVE=1, WRITING=2, DELETING=3 |
| 0x06 | 2 | uint16 LE | namespace | SAVE=1, CHEAT=2, STATS=3, PRIVATE=4 |
| 0x08 | 4 | uint32 LE | flags | Zero |
| 0x0C | 4 | uint32 LE | key_size | 1..448 |
| 0x10 | 8 | uint64 LE | data_offset | Relative mutable offset, 64-aligned, in data area |
| 0x18 | 8 | uint64 LE | data_capacity | >0 and in bounds |
| 0x20 | 8 | uint64 LE | data_size | ≤capacity; zero data has zero CRC |
| 0x28 | 8 | uint64 LE | generation | Starts at 1; increments on replacement |
| 0x30 | 8 | uint64 LE | modified_unix_seconds | UTC seconds or zero |
| 0x38 | 4 | uint32 LE | data_crc32 | Exact data bytes |
| 0x3C | 4 | uint32 LE | entry_crc32 | Full entry with field zeroed |
| 0x40 | 448 | bytes | key | UTF-8 key, then zero padding |

`[SPEC]` `(namespace,key)` is unique after case folding; all ACTIVE/WRITING/
DELETING extents are non-overlapping. SAVE/CHEAT/STATS/PRIVATE are namespaces;
PRIVATE keys begin with a producer identifier and `/`.

`[IMPLEMENTATION]` `src/mutable.c` makes a bad header INVALID and bad slots
DEGRADED/quarantined; ACTIVE objects are CRC-checked before exposure.
`src/mutable_write.c` reuses an existing extent and does not relocate, grow,
compact or repack.

`[RESOLVED][SPEC][IMPLEMENTATION]` Key and path collision checks use the same
ASCII A–Z folding rule as RIDX; non-ASCII bytes are preserved. A full Unicode
profile may be added later without changing the 0.2.0 wire layout.

## 8. Mutable commit protocol

`[SPEC][IMPLEMENTATION]` Create/replace sequence: durable valid WRITING entry →
write exactly the selected data extent and durable data → durable valid ACTIVE
entry (commit point). Delete sequence: durable DELETING entry → zero the full
512-byte slot and durable flush. WRITING/DELETING are not exposed; an
interrupted object can become unavailable, while immutable payload/RIDX/footer
and file size remain unchanged.

`[IMPLEMENTATION]` POSIX uses an exclusive `fcntl` lock, `pwrite` and `fsync`;
Windows uses LockFileEx and FlushFileBuffers. Reader close does not auto-recover,
sync, write back or delete mutable data.

`[TEST EVIDENCE]` Tests cover generation 1→2, extent reuse, delete, mutable-data
CRC failure, intermediate WRITING/DELETING visibility and mutable-header
isolation while the entrypoint remains readable. Real power-cut recovery is a
platform release-acceptance test; it does not add a new wire state.

## 9. SAVE/CHEAT bundles and SAVE slots

### 9.1 SAVE slot model

- `[SPEC]` SAVE is zero or more logical slots. Every ACTIVE SAVE object is independently committed; one object may project multiple slots. Object keys are stable consumer labels, not host paths; slot ordering is not encoded.
- `[SPEC]` A multi-file slot is one uncompressed RMBL bundle. RMBL `entry_count` counts all files, not slots. Replacing/deleting one projected slot must retain unselected entries and atomically replace the same outer object.
- `[IMPLEMENTATION]` `src/mutable_bundle.c` reads footer `platform_id`/`launch_format_id` and exposes `romx_mutable_bundle_get_save_slot_count`, `get_save_slot` and `get_save_slot_entry`.
- `[SPEC][IMPLEMENTATION]` PSP: only a directory with valid `PARAM.SFO` identity is a slot; valid `DISC_ID`, or `SAVEDATA_DIRECTORY` matching the directory basename, is sufficient; all files below that directory belong to it.
- `[IMPLEMENTATION]` `romx_mutable_psp_savedata_inspect_sfo` parses caller-provided SFO bytes only; it does not scan directories or choose host paths. It checks bounded PSF records, identity characters and optional title.
- `[SPEC][IMPLEMENTATION]` Other registered platforms, including UNSPECIFIED, expose one slot per complete normalized bundle-relative file path; a directory in a path is not implicit grouping.
- `[RESOLVED][SPEC][IMPLEMENTATION][TEST EVIDENCE]` UNSPECIFIED and unknown
platform IDs use conservative one-slot-per-complete-file projection. Nested
PSP roots are valid when their `PARAM.SFO` identity is valid; the longest
matching valid root owns each subtree. The bounded PSF v1.01 grammar and
malformed-table rejection are part of the 0.2.0 profile.

#### 9.1.1 libromx slot-projection output structs (in-memory ABI, not wire fields)

`[IMPLEMENTATION]` These structures are defined by `include/romx/romx.h`; callers initialize `struct_size` to `sizeof` before each call. They are not stored in the ROMX footer, RIDX, or RMBL bytes.

| Structure/field | Type and meaning | Constraint/lifetime |
|---|---|---|
| `romx_mutable_save_slot_info_t.struct_size` | uint32 structure version/input size | Must be at least the current structure; output is reset to the current size |
| `index` / `entry_count` | uint32 slot index and member-file count | Zero-based; `entry_count` counts only this slot's members |
| `key` / `key_size` | NUL-terminated UTF-8 slot label and byte length | Bundle-relative/stable label, never a host path; copied to caller storage |
| `display_name` / `display_name_size` | NUL-terminated display name and byte length | PSP uses SFO title first, otherwise directory name; ordinary slots use path basename |
| `data_size` | uint64 total data bytes of member files | Projection sum, bounded by bundle data; copied value |
| `is_directory` | uint32, 1 for PSP directory slot and 0 for per-file slot | Projection hint only; does not alter RMBL wire |
| `romx_mutable_psp_savedata_info_t.flags` | uint32 identity bit mask | `HAS_DISC_ID=1`, `HAS_DIRECTORY=2`, `HAS_TITLE=4` |
| `disc_id` / `savedata_directory` / `title` | NUL-terminated SFO strings | Maximum 64, 1024, and 1024 bytes respectively; caller owns output struct |

`[RESOLVED][IMPLEMENTATION]` These are in-memory ABI structs, not wire fields.
`struct_size` is the explicit extension gate: callers pass the size they know,
and the library writes only that compatible prefix while reporting the current
size. Tail compatibility does not alter footer/RIDX/RMBL bytes.

### 9.2 RMBL header — 64 bytes

| Offset | Size | Type | Field | Constraint/check |
|---:|---:|---|---|---|
| 0x00 | 4 | bytes | `magic="RMBL"` | Exact match |
| 0x04 | 2 | uint16 LE | version=1 | Fixed |
| 0x06 | 2 | uint16 LE | header_size=64 | Fixed |
| 0x08 | 2 | uint16 LE | namespace | Outer SAVE or CHEAT |
| 0x0A | 2 | uint16 LE | flags | Zero |
| 0x0C | 4 | uint32 LE | entry_size=64 | Fixed |
| 0x10 | 4 | uint32 LE | entry_count | Number of regular files; default max 4096 |
| 0x14 | 4 | uint32 LE | reserved | Zero |
| 0x18 | 8 | uint64 LE | directory_offset=64 | Fixed |
| 0x20 | 8 | uint64 LE | path_table_offset | `64+entry_count*64` |
| 0x28 | 8 | uint64 LE | data_offset | `align_up(path_table_end,64)` |
| 0x30 | 8 | uint64 LE | bundle_size | Exactly outer object data_size |
| 0x38 | 4 | uint32 LE | header_crc32 | Full header with field zeroed |
| 0x3C | 4 | bytes | reserved | All zero |

### 9.3 RMBL entry — 64 bytes

| Offset | Size | Type | Field | Constraint/check |
|---:|---:|---|---|---|
| 0x00 | 8 | uint64 LE | path_offset | Absolute bundle offset, packed path table |
| 0x08 | 4 | uint32 LE | path_size | 1..1024 UTF-8 bytes |
| 0x0C | 4 | uint32 LE | flags | Zero; regular files only |
| 0x10 | 8 | uint64 LE | data_offset | Absolute bundle offset, 64-aligned and directory ordered |
| 0x18 | 8 | uint64 LE | data_size | Verbatim file length; zero allowed |
| 0x20 | 4 | uint32 LE | data_crc32 | Exact file bytes |
| 0x24 | 28 | bytes | reserved | All zero |

`[SPEC]` Paths are packed, strict normalized UTF-8, unsigned-byte sorted and
unique after ASCII folding; all padding is zero; empty bundle is exactly the
64-byte header. Symlinks, absolute paths, dot/traversal components and
destination conflicts are forbidden.

`[IMPLEMENTATION]` `src/mutable_bundle.c` rejects non-regular/reparse sources,
computes file CRCs, validates the full object before exposing entries, and
uses the mutable WRITING→ACTIVE transaction. Host staging/restore is not
performed by the C API; an adapter must validate all selected paths and replace
only those paths, never a shared root.

`[TEST EVIDENCE]` `tests/test_writer_mutable.c` covers two SAVE objects, normal
per-file slots, PSP SFO directory slots, slot member lookup, path sorting,
file reads and collision rejection.

## 10. STATS JSON profile

- `[SPEC][IMPLEMENTATION]` A `STATS` object normally uses key `default`; JSON is strict UTF-8, no BOM/duplicate/unknown fields/floats/comments/trailing non-whitespace, and ≤16384 bytes. Required members are `schema="romx.stats"` and `version=1`.
- `[SPEC][IMPLEMENTATION]` Optional values are play time, launch count, first/last UTC seconds, favorite, completed, completion percent 0..100, and achievements `{unlocked,total,hardcore_unlocked}` with safe-integer and ordering constraints.
- `[IMPLEMENTATION]` Parse/serialize/read/write APIs use compact fixed-order output while accepting valid input ordering/whitespace.
- `[SPEC]` Synchronization uses latest baseline plus session delta; first time is minimum, last time maximum; stale absolute snapshots must not overwrite newer counters. Favorite/completion/achievement conflict is an explicit consumer decision.
- `[RESOLVED][IMPLEMENTATION][TEST EVIDENCE]` `romx_mutable_stats_merge_session_delta()` merges a validated session delta into the latest baseline using checked counter addition, minimum first timestamp, maximum last timestamp, and session user-state/achievement summaries overriding the baseline. Overflow and merge behavior are covered by tests; callers reread the newest generation before committing.
- `[TEST EVIDENCE]` Writer tests cover round trip, unknown/duplicate-key rejection and mutable STATS read/write, but not concurrent merge conflicts.

## 11. Reader access model and public API

### 11.1 Lifecycle and concurrency

- `[IMPLEMENTATION]` `romx_reader_open_io/open_path` obtains size, reads the final footer, parses footer/RIDX/mutable structure, then returns; full hashes/metadata/cover/entry CRC scans are opt-in through validation flags.
- `[IMPLEMENTATION]` `get_info`, entry lookup, positional reads, payload IO view, map, payload file, VFS, metadata, extraction, mutable object/file, bundle, STATS, probe and writer APIs are exposed in `romx.h`.
- `[IMPLEMENTATION]` Metadata handles, cursors, payload IO views, mappings, mutable files and bundles borrow the reader/input; close them before `romx_reader_close`. Cursors have mutable position and are not independently synchronized.
- `[SPEC][IMPLEMENTATION]` Metadata/cover/mutable failures are isolated from valid immutable payload access; an ACTIVE mutable object is exposed only after its CRC checks.
- `[RESOLVED][IMPLEMENTATION]` Metadata and positional reads may run concurrently when the caller's `romx_io_t.read_at` is thread-safe. Seek cursors, payload IO cursors and bundle handles carry mutable position/state and must not be shared concurrently; the caller owns synchronization for custom IO.

### 11.2 Interface contract table

All public calls return `romx_result_t`, optionally fill `romx_error_t`, and do
not terminate the process. Output structs require their `struct_size` to be
initialized. Opaque handles are owned by the creator and released by `*_close`.

| API family | Inputs/outputs | Lifetime | Error behavior |
|---|---|---|---|
| Reader open/get/entry/find/read/validate | IO/path/options, index/path/region/buffer; reader/info/entry/report/bytes | Reader owns path IO; custom IO remains caller-owned | invalid argument, IO, truncated, footer/index/range/CRC/hash |
| Payload IO/map, VFS, payload file | reader/path/entry, buffer/seek; borrowed IO/cursor/mapping | Reader outlives view/cursor; mapping close releases mapping | unsupported map, range, truncated, IO |
| Metadata | reader/key/buffer; metadata handle or copied JSON/string/value/CRC | Metadata borrows reader; close before reader | absent, size, UTF-8/JSON/schema, buffer-too-small |
| Extraction | reader/destination/cache/options | Temporary file is published only after verified write | integrity/hash/write/exists/rename/cover errors |
| Mutable object/file | reader or ROMX path, namespace/key/source/options; status/object/cursor | Reader/cursor lifetimes as above; writer locks file | absent/header/entry/data CRC/no-space/IO |
| Bundle core | SAVE/CHEAT path entries or reader/key; bundle/entry/bytes | Bundle borrows reader; full validation precedes exposure | bundle header/path/padding/file CRC/truncated |
| SAVE slot projection | SAVE bundle, slot index/member index; slot/member entry copies | Bundle must remain open | invalid namespace/index/argument |
| PSP SFO inspection | SFO bytes/size/optional basename; identity/title info | Pure memory; caller owns buffers | invalid argument or no valid identity (`ROMX_E_MUTABLE_BUNDLE`) |
| STATS | JSON bytes/stats/buffer or reader/path/key | Parse/serialize are memory-only; write uses mutable commit | stats/schema/value/buffer/no-space/IO |
| Probe | IO/path/format; probe info/metadata/PNG | Probe owns temporary buffers until close | unsupported/IO/buffer-too-small; bad cover is downgraded |
| Writer | destination, entries, metadata/cover/options; report | Streams caller inputs; temporary is internal | argument/metadata/cover/index/range/write/exists/rename |
| Registry/result names | numeric value; static string or NULL | No state | Unknown values return NULL/unknown result string |

`[RESOLVED][IMPLEMENTATION]` The C++ wrapper is an optional move-only RAII
convenience layer that covers common reader, writer, mutable and STATS-merge
operations and throws `romx::error`; it is intentionally not a replacement
for the complete C ABI.

## 12. Writer behavior

- `[IMPLEMENTATION]` Validate platform/launch IDs, entry IO/path/format, exactly one non-empty entrypoint, path collisions, metadata and cover before output.
- `[SPEC][IMPLEMENTATION]` Stream entrypoint first at payload offset zero, then remaining entries; preserve bytes; optionally calculate entry CRC32 and immutable SHA-256.
- `[IMPLEMENTATION]` Build RIDX/index CRC, append metadata/cover, zero padding/RMUT, then footer; mutable capacity is fixed and never auto-grown.
- `[IMPLEMENTATION]` Create a destination-local exclusive temporary file, optionally durable-flush it, close it and atomically publish with explicit replace semantics.
- `[IMPLEMENTATION]` `ROMX_WRITER_PROBE_PAYLOAD` can fill absent metadata/cover but never overrides caller data; unsupported probe leaves them absent.
- `[TEST EVIDENCE]` Tests cover multi-entry CUE, metadata, cover/probe, entry CRC, immutable hash, mutable writing/replacement/delete and SAVE-slot projections.
- `[RESOLVED][IMPLEMENTATION][TEST EVIDENCE]` POSIX durable publish fsyncs the
  temporary file and parent directory after rename/link. Windows uses
  `FlushFileBuffers` and a write-through replacement. The contract is
  file-level durability; platform release testing must verify the host
  filesystem behavior.

## 13. Error codes and degradation

`[IMPLEMENTATION]` Error families are argument/resource/IO (`INVALID_ARGUMENT`,
`OUT_OF_MEMORY`, `IO`, `TRUNCATED`, `RANGE`, `WRITE`, `ATOMIC_RENAME`, `EXISTS`,
`BUFFER_TOO_SMALL`); footer/structure (`INVALID_FOOTER`, `INVALID_FLAGS`,
`INDEX`, `OVERLAP`, `UNSUPPORTED`); integrity (`IMMUTABLE_HASH`, `ENTRY_CRC`,
`EXTRACT_HASH`); metadata/cover; virtual path/entry; and mutable
(`MUTABLE_ABSENT`, `MUTABLE_HEADER`, `MUTABLE_ENTRY`, `MUTABLE_DATA_CRC`,
`MUTABLE_NO_SPACE`, `MUTABLE_BUNDLE`, `MUTABLE_STATS`). `romx_error_t` carries
code, system code, byte offset and a 256-byte message.

- `[SPEC]` Structural failure is fail-closed; metadata/cover, mutable layout, individual mutable object and entry CRC domains are isolated as specified.
- `[RESOLVED][SPEC][IMPLEMENTATION]` Salvage is not part of libromx 0.2.0. Structural errors fail closed; no public salvage ABI, mutable write-back or auto-launch path exists.

## 14. Security and robustness

- `[SPEC][IMPLEMENTATION]` Checked additions/multiplications prevent overflow; ranges cannot overlap or cross the footer; reserved bytes and alignment gaps must be zero.
- `[SPEC][IMPLEMENTATION]` Paths reject absolute paths, traversal, dot/empty components, backslashes and NUL; bundle sources reject symlink/reparse/non-regular files.
- `[IMPLEMENTATION]` Defaults are metadata 1 MiB, cover 32 MiB/8192 pixels, STATS 16 KiB, bundle 128 MiB/4096 entries; reader/writer options can lower or raise configured limits.
- `[IMPLEMENTATION]` Temporary files are exclusive and published atomically; CRC/hash checks complete before cache/extraction publication.
- `[RESOLVED][IMPLEMENTATION]` The public lifecycle contract now states the
  concurrent-read versus exclusive-cursor rule; object handles remain borrowed
  from the reader and custom IO synchronization remains the caller's duty.

## 15. Version and compatibility

- `[SPEC]` ROMX 0.2.0 ↔ footer wire 2, RIDX 1, metadata 0.2.0, RMUT 1, RMBL 1, STATS 1; reserved fields are zero.
- `[SPEC]` Unknown non-prohibited non-zero IDs are unsupported rather than structural corruption; 0xFFFF is prohibited; private registry values need a shared definition.
- `[IMPLEMENTATION]` Footer/RIDX reject 0xFFFF and preserve unknown numeric IDs; registry name helpers return NULL for unknown values.
- `[RESOLVED][SPEC][IMPLEMENTATION]` Readers fail closed on future wire/RIDX/RMUT/RMBL versions. Registry status APIs distinguish known, unspecified, unknown, private and prohibited values. Unknown nonzero IDs are readable but unsupported, while UNSPECIFIED/unknown SAVE projection is one file per complete bundle-relative path.

## 16. Tests and conformance matrix

| Capability | Expected behavior | Evidence | Status |
|---|---|---|---|
| Minimal single file | offset-zero payload/entrypoint, no optional regions | `minimal-single.manifest.json`, `tests/test_v2.c` | `[TEST EVIDENCE]` |
| Complete single file | metadata/PNG/12 KiB mutable/immutable SHA | `single-complete.manifest.json`, `tests/test_v2.c` | `[TEST EVIDENCE]` |
| Multi-file CUE/BIN | descriptor entrypoint, paths, entry CRC | `multi-cue.manifest.json`, `tests/test_v2.c` | `[TEST EVIDENCE]` |
| Writer/RIDX | stream order, CRC/index, atomic publication | `tests/test_writer_mutable.c` | `[TEST EVIDENCE]` |
| Mutable objects | write/replace/generation/delete/CRC | both C tests | `[TEST EVIDENCE]` |
| SAVE/CHEAT RMBL | header/path/file CRC/collision | `tests/test_writer_mutable.c` | `[TEST EVIDENCE]` |
| SAVE slot projection | ordinary per-file and PSP directory slots | `tests/test_writer_mutable.c` | `[TEST EVIDENCE]` |
| PSP SFO inspection | DISC_ID, directory label, bounded SFO | `tests/test_writer_mutable.c` | `[TEST EVIDENCE]` |
| STATS | schema, duplicate/unknown rejection, round trip | `tests/test_writer_mutable.c` | `[TEST EVIDENCE]` |
| Probe | optional metadata/cover | `tests/test_writer_mutable.c` | `[TEST EVIDENCE]` |
| Bad magic/truncation/CRC/overlap/traversal | fail closed or isolated domain | adjacent fixtures/source | `[TEST EVIDENCE]` — fixture provenance pinned externally |
| Interrupted write/power loss | WRITING/DELETING hidden, immutable unchanged | commit code/tests | `[RESOLVED]` — intermediate-state tests; real power cut is platform acceptance |
| gcc/clang/MSVC | clean cross-platform ABI | CI history and warning-clean source | `[TEST EVIDENCE]` |

## 17. Implementation versus the frozen standard

The following register records the decisions that closed every former release-gate question.

| Topic | Final evidence | Resolved result |
|---|---|---|
| Provenance | `ROMX-0.2.0-PROVENANCE.md`, adjacent source commit and fixture hashes | Normative inputs are pinned and installed as release records; no game bytes are vendored |
| Text identity | `src/ridx.c`, `src/mutable.c`, tests | Valid UTF-8, non-ASCII preserved, ASCII A–Z folding; full Unicode profile is future work |
| Registry IDs | `src/registry.c`, public header, tests | KNOWN/UNSPECIFIED/UNKNOWN/PRIVATE/PROHIBITED status APIs; unknown nonzero values are readable but unsupported |
| Salvage | public header, `src/reader.c`, resolution record | Explicitly outside the normal reader; fail-closed, read-only recovery remains a separate future tool |
| SAVE projection | `src/mutable_bundle.c`, writer tests | UNSPECIFIED/unknown platforms are per-file; nested PSP uses longest valid SFO root |
| PSP SFO | `src/mutable_bundle.c`, writer tests | Bounded PSF v1.01 grammar, checked tables/records and identity rules are frozen |
| Slot ABI | `include/romx/romx.h`, C++ wrapper | `struct_size` gates in-memory extension; no wire compatibility impact |
| STATS merge | `src/mutable_stats.c`, tests | Public session-delta merge with checked counters, timestamp min/max and summary overwrite |
| Mutable interruption | `src/mutable_write.c`, tests | Durable WRITING/DELETING protocol; intermediate states hidden; power-cut testing is platform acceptance |
| Durable publish | `src/durability.c`, `src/writer.c`, `src/extract.c` | POSIX parent-directory fsync and Windows file-level write-through are implemented |
| Payload SHA | `src/extract.c`, public header | Derived extraction self-check only; not footer/metadata identity or trust input |
| Thread contract | public header and reader/VFS code | Concurrent positional reads require thread-safe IO; cursors/bundles are exclusive |
| C++ scope | `include/romx/romx.hpp` | Optional move-only RAII convenience for common operations, not the full C ABI |
| Compiler portability | CI and warning-clean source | GCC/Clang/MSVC warning failures are resolved; strict conversion builds are supported |

No release-gate item remains unclassified. Any future behavior change must be recorded as a new versioned decision rather than reopening the 0.2.0 baseline.

## 18. Review conclusion

- `[IMPLEMENTATION][TEST EVIDENCE]` Current code and tests cover footer, RIDX, metadata, cover, payload view/VFS, writer, mutable objects, SAVE/CHEAT bundles, platform-aware SAVE slots, PSP SFO identity inspection, STATS and probe paths.
- `[SPEC][IMPLEMENTATION]` Immutable/mutable boundaries, CRC/SHA ranges, atomic commit order, durability and path safety provide the 0.2.0 review baseline.
- `[RESOLVED][TEST EVIDENCE]` The release-gate decisions are closed by the provenance and resolution records; tests and CI cover the implemented behavior, while real power-cut behavior remains a platform acceptance check and not a new wire requirement.
