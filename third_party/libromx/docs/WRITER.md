# ROMX 0.2.0 writer and mutable commit API

The field-level and lifecycle decisions summarized here are cross-referenced
by the bilingual technical specification drafts and Canvas maps; the frozen
wire provenance is recorded in `ROMX-0.2.0-PROVENANCE.md`.

## Immutable container writer

`romx_writer_write_io_entries` and `romx_writer_write_path_entries` create
native ROMX 0.2.0 containers. Each input becomes one RIDX entry. Exactly one
entry must set `ROMX_RIDX_ENTRYPOINT`, and its `format_id` must be non-zero.
`ROMX_RIDX_HAS_CRC32` independently enables a stored CRC32 for that entry.

The writer streams entry bytes without compression. It physically writes the
entrypoint first at payload offset zero, writes the remaining entries, builds
RIDX, then writes optional metadata, optional PNG cover, immutable alignment
padding, optional mutable capacity, and the 128-byte footer. The completed
temporary file is published atomically. With `ROMX_WRITER_DURABLE`, POSIX
builds also flush the containing directory after the rename/link; Windows uses
file-handle flush plus write-through move and has no portable directory-handle
durability guarantee.

For an Arcade single-archive container, set the entrypoint format to
`ROMX_FORMAT_ZIP` and the launch format to `ROMX_LAUNCH_RAW_SINGLE_FILE`.
Pass the original ZIP as the source entry. The writer copies those bytes
unchanged; it does not inspect, inflate, recompress, or split ZIP members.
The existing reader payload I/O, mapping, and extraction APIs are sufficient,
so this profile does not require a ZIP-specific libromx handle or ABI.

Initialize `romx_writer_options_t` with `ROMX_WRITER_OPTIONS_INIT` and set a
registered `platform_id` and `launch_format_id`.

- `ROMX_WRITER_IMMUTABLE_SHA256` stores the optional immutable-range digest.
- `ROMX_WRITER_REPLACE_EXISTING` permits atomic destination replacement.
- `ROMX_WRITER_DURABLE` flushes the completed temporary file before publish.
- `ROMX_WRITER_PROBE_PAYLOAD` asks libromx to fill a missing metadata region
  and/or cover from the entrypoint when the format has extractable information.

Probing never overrides caller-supplied metadata or cover. It is best-effort;
unsupported, absent, encrypted, damaged, or non-text header content remains
absent. An extracted image must still pass the normal PNG validator.

For mutable storage, `mutable_capacity` must be a multiple of 4096 and at least
12288. `mutable_entry_capacity` must be a positive multiple of eight; zero
selects eight slots. Capacity is reserved once and does not enlarge during
later commits.

## Metadata and cover

Metadata is an optional in-memory strict UTF-8 JSON object conforming to the
ROMX 0.2.0 schema. libromx does not rewrite caller metadata and does not invent
database values. The cover is an optional PNG callback/path input and is
embedded byte-for-byte after structural validation. JPEG/WebP/GIF/BMP
conversion and resizing belong in a frontend or image adapter.

## Mutable in-place commits

`romx_mutable_write_io_path` and `romx_mutable_write_path` explicitly create or
overwrite one opaque SAVE, CHEAT, STATS, or PRIVATE object inside the reserved
mutable region. `romx_mutable_delete_path` explicitly deletes one object.

Writes lock the container, calculate the source CRC32, and use the required
three-stage protocol:

1. write and flush a valid `WRITING` directory entry;
2. overwrite the object's fixed-capacity extent and flush its bytes;
3. write and flush the final `ACTIVE` entry.

The third step is the commit point. Delete similarly commits `DELETING` before
clearing the directory slot. Every mutable commit is durable; the options
`flags` field is reserved and must be zero. A replacement keeps the original
extent and capacity and increments its generation. A new object may request a
larger fixed `data_capacity`, rounded to 64-byte alignment.

Mutable commits never change file size, move the footer, or recalculate the
optional immutable SHA-256. Insufficient capacity returns
`ROMX_E_MUTABLE_NO_SPACE`. An interrupted transaction may quarantine the
affected slot, but immutable game content remains readable.

## SAVE/CHEAT bundles and STATS

`romx_mutable_bundle_write_path_entries` accepts an explicit list of regular
source files and ROMX 0.2.0 canonical relative paths (well-formed UTF-8 with
ASCII-only collision folding). For `SAVE`, multiple object keys
can coexist in the same container, and each key gets an independent
transaction and fixed extent. The explicit paths under one call form one
bundle; the platform profile may project one or more logical save slots from
it. The key is not a host path. The function
sorts paths canonically, rejects traversal, case-folding collisions, symlinks
and non-regular files, and streams an uncompressed `RMBL` object through the
existing durable mutable transaction. It does not create a tar file or scan a
directory.

For host imports, `romx_save_catalog_open_path` is the library-owned scan and
grouping layer. Its 3DS profile treats each direct child directory as one
candidate and keeps all regular files below that directory together; direct
files remain separate candidates. `romx_save_catalog_write_candidate` feeds
the selected candidate to the same durable RMBL writer, so a GUI does not
need a second directory walker or a second grouping implementation. Source
format labels are descriptive; wrapping a Gateway file does not decrypt it.

`romx_mutable_bundle_open` validates the selected outer object CRC32, bundle
header, directory, path table, zero padding, and every file CRC32 before
exposing entries. The bundle borrows its reader, and its entry reads are
cursor-backed; serialize `romx_mutable_bundle_read_entry` calls or open one
bundle handle per consumer thread. The bundle `entry_count` is
the number of files in the whole bundle, not the number of SAVE slots;
`romx_mutable_save_slot_info_t.entry_count` is the number of files in one
selected slot. Enumerate
`SAVE` objects with `romx_reader_get_mutable_object_count` and
`romx_reader_get_mutable_object`, filtering `object_namespace` for
`ROMX_MUTABLE_NAMESPACE_SAVE`. For each bundle, call
`romx_mutable_bundle_get_save_slot_count`, `romx_mutable_bundle_get_save_slot`,
and `romx_mutable_bundle_get_save_slot_entry` to expose the platform-aware slot
projection defined by the ROMX specification: PSP groups only directories
whose `PARAM.SFO` contains a valid `DISC_ID` or matching
`SAVEDATA_DIRECTORY`, 3DS groups entries below each first-level directory,
and other ROMX 0.2.0 platforms expose one slot per bundle file.

`romx_mutable_psp_savedata_inspect_sfo` applies the same PSP identity check to
caller-provided SFO bytes before an adapter imports a local directory. It does
not choose frontend paths; the host-side directory scan is provided by the
SAVE catalog above.

`romx_mutable_stats_serialize_json` and `romx_mutable_stats_parse_json`
implement the strict `romx.stats` version 1 schema.
`romx_mutable_stats_write_path` serializes and commits it in one operation.
`romx_mutable_stats_merge_session_delta` combines a freshly read baseline with
the current frontend session delta without allowing stale absolute counters to
overwrite newer data.
SAVE, CHEAT, STATS, and PRIVATE remain separate mutable objects with independent
fixed extents; one namespace cannot borrow another object's capacity.
