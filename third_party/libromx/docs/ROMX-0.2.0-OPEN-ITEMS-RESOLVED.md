# ROMX 0.2.0 open-item resolution

This is the release-gate decision record for the `libretro` libromx branch. It
supersedes the earlier open-item annotations in the technical audit draft. The
wire snapshot is pinned in `ROMX-0.2.0-PROVENANCE.md`.

| # | Former open item | Decision and current adaptation |
|---:|---|---|
| 1 | Standard version and fixture ownership | **Resolved.** Footer wire `2`, RIDX `1`, metadata `0.2.0`, RMUT/RMBL `1`, and STATS `1` are frozen. The ROMX specification/fixture source commit and document hashes are recorded in the provenance file; game bytes are never vendored. |
| 2 | Unicode NFC/full case-fold | **Resolved for 0.2.0.** Paths and keys require well-formed UTF-8; non-ASCII bytes are preserved and only ASCII `A`–`Z` is folded for collision checks. Full Unicode normalization is a future profile, not an unadvertised dependency. Chinese titles remain valid UTF-8. |
| 3 | Unknown platform/launch/file IDs | **Resolved.** `romx_*_status()` reports `KNOWN`, `UNSPECIFIED`, `UNKNOWN`, `PRIVATE`, or `PROHIBITED`; zero is `UNSPECIFIED` for platform/launch and `UNKNOWN` for RIDX file format. Unknown non-zero IDs remain numerically readable but are unsupported; `0xFFFF` is structurally invalid. RetroArch must show a compatibility error instead of auto-detecting. |
| 4 | Salvage API | **Resolved out of scope.** Normal readers fail closed on a missing/invalid footer. libromx 0.2.0 exposes no salvage handle; a consumer may ship a separate explicitly labelled recovery tool that cannot write back mutable data. |
| 5 | STATS session merge/conflict | **Resolved in library.** `romx_mutable_stats_merge_session_delta()` adds safe-integer counters, takes min/max timestamps, and lets session-provided user state/achievement summaries replace the baseline. The frontend still owns user confirmation and latest-generation reread. |
| 6 | Bundle host restore | **Resolved as an adapter boundary.** libromx validates, projects and streams bundle bytes only. RetroArch owns staging, destination selection, symlink checks, conflict prompts, and atomic replacement of selected per-content paths; it never replaces a shared save root. |
| 7 | Interrupted-write/power-loss fixture | **Protocol resolved; hardware acceptance remains explicit.** The durable WRITING → data → ACTIVE and DELETING → clear ordering is normative, and readers quarantine incomplete entries. Unit coverage exercises malformed/intermediate states; a real power-cut run is a platform release test, not a new wire rule. |
| 8 | Durable publish boundary | **Resolved.** `ROMX_*_DURABLE` flushes the temporary file and, on POSIX, fsyncs the containing directory after atomic rename/link. Windows documents file-level durability via `FlushFileBuffers`/write-through move because directory handles have no portable flush contract. |
| 9 | Payload SHA status | **Resolved as derived data.** Payload SHA is an extraction self-check only; it is not a footer field, metadata requirement, or trust signal. Immutable SHA remains the only stored container-wide hash. |
| 10 | Same-cursor thread safety | **Resolved by contract.** Immutable reader metadata and positional reads may be concurrent when the caller's IO callback is safe. VFS, payload-file, mutable-file, and bundle cursors own seek state and must not be shared across concurrent seek/read calls; use one handle per consumer. |
| 11 | C++ wrapper coverage | **Resolved by scope.** The header-only wrapper is optional move-only RAII convenience for reader, writer, mutable-object and STATS merge calls. It is not an ABI and does not promise the complete VFS/mapping/bundle/probe/report surface; those remain in the C ABI. |
| 12 | SAVE projection for unknown platforms | **Resolved.** Unknown non-zero and `UNSPECIFIED` platform IDs use conservative one-file-per-slot projection. Only an explicit registered platform policy can opt into directory grouping; no directory-count heuristic is permitted. |
| 13 | Nested PSP roots | **Resolved.** Every valid `PARAM.SFO` root is a candidate. A file belongs to the longest matching valid root, so an inner savedata root owns its subtree and outer roots retain only their remaining members. |
| 14 | PSP PARAM.SFO grammar | **Resolved.** The profile is bounded PSF v1.01 with checked table/record ranges, accepted integer/string/binary formats, NUL-terminated string values, and valid `DISC_ID` or basename-matching `SAVEDATA_DIRECTORY`. Malformed tables are rejected, not salvaged. |
| 15 | Compiler warnings | **Resolved.** The SFO little-endian helpers use explicit casts and the new code builds cleanly with the project's `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` flags on the current macOS toolchain. |

## Evidence

- `ctest --test-dir build --output-on-failure`: 2/2 libromx tests passed after
  the changes (reader/VFS/payload and writer/mutable/bundle/PSP/STATS/probe).
- The writer-mutable test now covers nested PSP roots, strict SFO-version
  rejection, unknown-platform per-file fallback, STATS delta merge/overflow,
  and registry status classification.
- The existing RetroArch ROMX adapter suite remains the owner of host-directory
  restore and frontend-path tests; no core ABI or emulator-core source is
  changed by these libromx decisions.
