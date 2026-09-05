# ROMX 0.2.0 release-gate provenance

This file closes the version-ownership question for the `libretro` branches.
The following byte snapshot is the only ROMX 0.2.0 wire contract consumed by
this libromx branch:

| Artifact | Pinned value |
|---|---|
| Container format | ROMX 0.2.0 |
| Footer wire | `2` |
| RIDX wire | `1` |
| Metadata schema | `0.2.0` |
| Mutable header | `RMUT` v1 |
| Bundle profile | `RMBL` v1 |
| STATS profile | `romx.stats` v1 |
| ROMX specification source commit | `d0f7bb7d979ea4a0189e6d4d8b2cbf3536eded82` |
| English specification SHA-256 | `a9b67cea76c5c44cf68c124c4d4a7c848eae28dcc3f9bfa4213816e16ef06e67` |
| Chinese specification SHA-256 | `61c8535fca58f0880f978c214506d378b6a9b7e4271c818599ced4ed57eda7e2` |
| Metadata schema SHA-256 | `edb162ef3107081eeaf78ba20150327c63aac460de5f483b1eb30ef08b8ebb88` |
| English fixture README SHA-256 | `95df77a3158d898b004d6e063d4b0e7ef2c97a93a893fbe9ccfc28e818fc6097` |
| Chinese fixture README SHA-256 | `cfc18ebd3a35828bd2918a8fe2866f51514f2f42357b18a04d047790c642c7c1` |

The source commit identifies the `romx` `libretro` baseline; the document and
schema hashes identify the frozen working-tree snapshot after the decisions in
this release gate. The commit plus these hashes form the immutable 0.2.0
provenance boundary; a future wire change must publish a new provenance record.

The specification repository is a separate dependency; libromx does not copy
copyrighted game fixtures into its source tree. CI or a release build must
obtain the fixture directory from the pinned source and record the hashes of
the generated fixture files. A change to serialized semantics requires a new
wire version or a new provenance record; implementation fixes that preserve
these values remain 0.2.0-compatible.

## Compatibility decisions fixed by this snapshot

- Paths and mutable keys require well-formed UTF-8, `/` separators, and the
  ASCII-only fold implemented by libromx. Full Unicode NFC/case folding is a
  future profile, not a hidden dependency of 0.2.0.
- A zero platform or launch-format ID is `UNSPECIFIED`; a zero RIDX
  file-format ID is `UNKNOWN` and is valid only for non-entrypoint files.
- Unknown non-zero registry IDs remain readable numerically but are reported as
  unsupported. Unknown platform IDs use the conservative per-file SAVE-slot
  projection; they never opt into PSP grouping.
- Reader wire-version checks fail closed. There is no salvage handle in the
  0.2.0 C ABI.
- The PSP slot profile uses validated v1.01 PARAM.SFO records and assigns
  nested roots by longest validated path prefix.
