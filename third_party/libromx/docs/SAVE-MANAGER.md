# Host-side SAVE catalog

`romx_save_catalog_*` is the library-owned boundary between a host save tree
and the ROMX mutable SAVE namespace. It keeps filesystem traversal, save
boundary rules, source classification, path normalization, and RMBL writing in
one implementation so a frontend only renders candidates and chooses where to
commit them.

This API does not depend on GBAStation, libretro, a UI, an emulator installation
directory, or an automatic-import policy. The host owns those decisions. The
library reports save identity and relative layout; the host supplies the source,
destination, overwrite confirmation, and selection of the active save.

## Profiles

`romx_save_profile_get` returns the grouping policy for a ROMX platform and
format:

- PSP, or a PBP payload, uses a `PARAM.SFO` marker directory. The marker
  directory and all regular files below it are one candidate.
- Nintendo 3DS uses directory-per-save. Each directory directly below the
  selected collection directory is one candidate and every regular save file
  below that directory stays together. A regular file directly below the
  collection directory is its own candidate. A Citra/Azahar Title Save may be
  passed as the `00000001` leaf, as a `data` tree containing that leaf, or as a
  folder with an arbitrary name containing `saveData.bin`; all files in the
  identified Title Save root remain one candidate. Set
  `ROMX_SAVE_SCAN_TREAT_ROOT_AS_SAVE` when the selected path itself is one 3DS
  save directory.
- Other platforms use one candidate per recognized save file extension.

The profile is based on the ROM type, not on whether a directory happens to
contain one or many files. This is what lets a 3DS `save00.bin`/`system.dat`
directory remain one save while two single-file saves in the same collection
remain two saves.

## Scan and write

```c
romx_save_scan_options_t options = ROMX_SAVE_SCAN_OPTIONS_INIT;
romx_save_catalog_t *catalog = NULL;
romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
romx_mutable_object_info_t written = ROMX_MUTABLE_OBJECT_INFO_INIT;
romx_error_t error;
uint32_t count;

options.platform_id = ROMX_PLATFORM_NINTENDO_3DS;
options.format_id = ROMX_FORMAT_N3DS;

if (romx_save_catalog_open_path("/path/to/saves", &options,
        &catalog, &error) == ROMX_OK) {
    romx_save_catalog_get_candidate_count(catalog, &count, &error);
    for (uint32_t index = 0; index < count; ++index) {
        candidate = (romx_save_candidate_info_t)
            ROMX_SAVE_CANDIDATE_INFO_INIT;
        romx_save_catalog_get_candidate(catalog, index, &candidate, &error);
        /* Render candidate.key, candidate.display_name, and candidate flags. */
    }
    romx_save_catalog_write_candidate(catalog, 0, "/path/game.romx", NULL,
        NULL, NULL, &written, &error);
    romx_save_catalog_close(catalog);
}
```

The candidate file list is available through `romx_save_catalog_get_file*`.
Paths returned there are relative to the candidate root. For ordinary saves,
those paths are written unchanged to the RMBL bundle path table.
PSP marker-directory candidates retain their directory as a bundle path prefix
so a reader can reconstruct the same multi-file slot. Recognized directory
saves include every regular file, regardless of extension; the extension filter
is only used while discovering standalone save candidates. Thus a game's text
or image files are not silently dropped from an otherwise recognized save.
`romx_save_catalog_write_candidate` writes an uncompressed RMBL object in the
SAVE namespace. Each call writes one logical candidate, so a frontend can map
each 3DS candidate to its own SAVE object/folder without flattening several
saves into one file set.

## 3DS save scopes

`romx_save_candidate_info_t.scope` is the semantic storage area, independent
of the descriptive `source_format` value:

- `ROMX_SAVE_SCOPE_3DS_TITLE` is a normal Title Save. Its source files are
  written as paths relative to the candidate root, and the frontend can map
  them below the title's `title/<high>/<low>/data/00000001` directory.
- `ROMX_SAVE_SCOPE_3DS_EXTDATA` is an Extra Data archive. The normalized
  `extdata_id` field contains its 16 hexadecimal digit ID.

SaveDataFiler ExtData is intentionally strict. The selected candidate must
contain exactly the portable shape below (desktop metadata such as
`.DS_Store` is ignored):

```text
<editable-label>/
├── <id>/                 # one eight-digit hexadecimal directory
│   └── all ExtData files
├── <id>.dat
├── <id>_.dat
└── export.log
```

The outer label becomes the candidate/object label but is never used as the
ExtData ID. The inner ID and both matching sidecars determine the ID. The
writer preserves this relative shape inside the ROMX SAVE object, so a mutable
reader can recognize it again without relying on the object key. The public
`romx_mutable_bundle_get_save_layout` API performs that read-time analysis and
also recognizes legacy `extdata/<high>/<low>/...` bundles.

Native Citra/Azahar ExtData is written as `extdata/<high>/<low>/user/...` and
remains a distinct supported layout. The writer never invents SaveDataFiler
sidecars. A bare eight-digit directory, or a caller-provided source hint alone,
is insufficient evidence of an ExtData archive.

When restoring a strict SaveDataFiler object to Citra/Azahar, the frontend
adapter maps `<id>/...` to the native
`extdata/00000000/<id>/user/...` tree. `export.log`, `<id>.dat`, and
`<id>_.dat` stay in ROMX as interchange metadata and are not copied into the
emulator's `user` directory.

The scanner recognizes descriptive source forms for PSP savedata, 3DS
Gateway-style title-ID filenames, SaveDataFiler directories, Citra data
directories (including Azahar's `title/<high-8>/<low-8>/data/00000001`
layout), and ordinary 3DS backup directories. Source classification does not
perform console-specific decryption or title-key conversion. A caller
that needs such a conversion must supply a platform adapter before scanning,
or import the resulting files as a normal directory candidate.

The scanner skips symlinks and common desktop metadata by default, validates
UTF-8 and portable relative paths, sorts directory entries deterministically,
and enforces candidate, file-count, total-size, and recursion-depth limits.
