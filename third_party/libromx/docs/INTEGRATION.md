# ROMX 0.2.0 integration guide

The wire contract is the frozen snapshot documented in
`ROMX-0.2.0-PROVENANCE.md`. This guide describes the consumer API boundary;
it does not introduce a second format or a frontend-specific path convention.
For a source-backed field and lifecycle review, see the bilingual technical
specification drafts and their Obsidian Canvas maps in this directory.

## C, C++, and Rust

Install libromx and consume its exported target:

```cmake
find_package(romx CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE ROMX::romx)
```

Include `<romx/romx.h>` from C99 or later. C++ applications may use the C ABI
or the optional header-only `<romx/romx.hpp>` wrapper. Rust and other FFI
consumers should bind only `romx/romx.h`. Initialize every public structure
with its matching `*_INIT` macro.

## Embedding and platform capabilities

Keep application integrations (libretro callbacks, native save/cache paths,
UI operations and automatic/manual import policy) in the embedding project,
not in libromx. Vendored copies should use the same sources as the standalone
library. Select missing POSIX capabilities in the embedding build using private
compile definitions on the `romx` target:

- `ROMX_NO_MMAP`: positional reads remain available; mapping is unsupported.
- `ROMX_NO_PREAD`: use seek/read and seek/write; callers must serialize access
  to each shared handle.
- `ROMX_NO_FCNTL_LOCK`: the host must serialize mutable writes to a container.
- `ROMX_NO_DIRECTORY_SYNC`: sync file data without syncing the containing
  directory, for filesystems that cannot open directory descriptors. This has
  weaker rename durability after a power loss.

These are build capabilities, not a new wire format or public ABI. Only disable
facilities the destination platform actually lacks. `ROMX_DISABLE_MMAP` remains
an alias for compatibility with existing builds.

## Launch data paths

Open the container once and inspect `romx_info_t`. No launch path copies the
complete concatenated payload merely to open a game.

`romx_reader_get_payload_io` returns a borrowed positional-read view of the
RIDX entrypoint. For a single-file game this is the native ROM or disc image.
For a multi-file game it can be a small descriptor rather than the entire
payload region.

For multi-file content, enumerate or resolve virtual paths with
`romx_reader_get_entry*` and `romx_reader_find_entry`, then open them with
`romx_vfs_file_open`. The frontend exposes the stored descriptor and referenced
tracks through this VFS. libromx does not extract the set at launch.

`romx_reader_map_payload` maps only the entrypoint for path-backed readers.
Mapping is intended for a single-file entrypoint and is not a replacement for
the multi-file VFS.

The Arcade single-archive profile uses `ROMX_FORMAT_ZIP` with
`ROMX_LAUNCH_RAW_SINGLE_FILE`. The entrypoint is the complete original ZIP
archive, stored byte-for-byte. libromx deliberately does not parse or inflate
ZIP members: use the mapped/read entrypoint or
`romx_extract_payload_path` according to the consumer's loading contract, and
leave member-name/CRC matching to the arcade consumer (for example FBNeo).
The multi-archive `ROMSET` profile keeps each original ZIP as a separate RIDX
entry referenced by its launch descriptor.

## Validation and startup cost

Opening validates footer, region, RIDX, and mutable structure with bounded
reads. It does not automatically scan all game bytes.

- `ROMX_VALIDATE_ENTRY_CRC32` scans entries that declare `HAS_CRC32`.
- `ROMX_VALIDATE_IMMUTABLE_SHA256` scans the immutable range only when the
  footer declares SHA-256.
- Mutable object CRC32 is checked before an ACTIVE object is returned/opened.

The immutable hash excludes mutable bytes and the footer, so updating a save
does not rehash a multi-gigabyte image.

## Mutable restore and explicit commit

Use `romx_reader_get_mutable_status` before enumeration. Opaque objects remain
available through `romx_mutable_file_open`.

For interoperable SAVE/CHEAT file sets, use `romx_mutable_bundle_open`,
enumerate with `romx_mutable_bundle_get_entry*`, and stream original file
bytes with `romx_mutable_bundle_read_entry`. Enumerate active mutable objects,
filter for `ROMX_MUTABLE_NAMESPACE_SAVE`, then project logical slots from each
selected bundle.
Opening validates the complete selected bundle and all per-file CRC32 values.
Use `romx_mutable_bundle_get_save_slot*` to enumerate the logical slots and
their member files; libromx applies the platform policy (PSP directories with
a validated `PARAM.SFO` identity, 3DS first-level directories, and per-file
slots for other ROMX 0.2.0 platforms), so the frontend does not need to
reimplement path grouping.

For host-side imports, use `romx_save_catalog_open_path` with
`romx_save_scan_options_t`. The catalog owns the scan result and identifies
source forms such as PSP savedata, 3DS Gateway filenames, SaveDataFiler
directories, Citra data directories, and ordinary 3DS backup directories.
For 3DS, each direct child directory is one candidate and every regular file
below it stays in that candidate; direct files are kept as separate candidates.
Use `romx_save_catalog_get_candidate*` and `romx_save_catalog_get_file*` for
the UI, then call `romx_save_catalog_write_candidate` for the selected
candidate. The catalog converts the host file set into an uncompressed RMBL
SAVE object; it does not decrypt proprietary console save encryption.
`romx_mutable_psp_savedata_inspect_sfo` remains available for a standalone PSP
identity check.
Extract to staging and commit only the selected slot's paths; never replace a
shared frontend save root or another slot's paths.

Use `romx_mutable_stats_read` and `romx_mutable_stats_serialize_json` for the
strict versioned STATS profile. Cumulative runtime synchronization should
re-read the current object and add only the current session delta. The
`romx_mutable_stats_merge_session_delta` helper performs that merge with
safe-integer overflow checks, min/max timestamps, and explicit latest-session
ownership of user state and achievement summaries.

Writing back is a separate explicit action. Call
`romx_mutable_bundle_write_path_entries` for the complete SAVE object. When a
projected slot inside an existing bundle is replaced, preserve unrelated
bundle entries and replace only the selected slot's files; the
`romx_mutable_bundle_get_save_slot*` mapping identifies those entries. Call
the same writer for caller-selected CHEAT files,
`romx_mutable_stats_write_path` for statistics, or the generic
`romx_mutable_write_io_path`/`romx_mutable_write_path` APIs for opaque data.
Call `romx_mutable_delete_path` for explicit deletion. libromx performs the
in-place durable transaction, while UI,
destination directories, synchronization policy, and user confirmation remain
frontend responsibilities. Save states are outside the ROMX 0.2.0 namespaces.

A mutable error never invalidates or blocks immutable payload access. A
structurally valid container may carry an unknown non-zero registry ID; inspect
it with `romx_platform_status`, `romx_launch_format_status`, or
`romx_file_format_status` and report it as unsupported instead of silently
auto-detecting a profile. A `0xFFFF` ID is prohibited and fails structural
validation. Normal readers fail closed on an invalid footer; libromx 0.2.0
does not expose salvage handles.

## Optional entrypoint probing

`romx_probe_open_io` and `romx_probe_open_path` inspect a caller-declared file
format for embedded title, serial, and artwork/icon data. The native writer can
invoke the same mechanism with `ROMX_WRITER_PROBE_PAYLOAD` when metadata or
cover is missing. Probe results are descriptive and never replace footer/RIDX
platform and format declarations.

## Lifetimes and concurrency

Borrowed entrypoint views and VFS/mutable cursors require the reader to outlive
them. Path-backed readers use positional I/O. A custom `romx_io_t` source must
keep `user_data` valid, report a stable size, and make `read_at` thread-safe if
used concurrently. Reader metadata and independent positional reads are
safe to share when the callback is safe; a VFS, payload-file, or mutable-file
cursor has mutable seek state and must not be shared between concurrent
seek/read callers. A bundle's `romx_mutable_bundle_read_entry` is backed by an
internal mutable-file cursor as well, so serialize calls or open one bundle
handle per consumer thread.

The bundle API is intentionally host-agnostic. A RetroArch (or other
frontend) adapter owns staging, destination selection, symlink-safe extraction,
conflict prompts, and atomic replacement of only the selected per-content
paths. It must never replace a shared save root. That adapter boundary is part
of the 0.2.0 integration contract; libromx only validates, projects, and
streams the bundle bytes.

The optional C++ header is a move-only RAII convenience layer, not a second
ABI. It covers the common reader, writer, mutable-object, and STATS-merge
operations. VFS, mappings, bundle projection, probing, and detailed reports
remain available through the complete C ABI and are not promised by the C++
wrapper.
