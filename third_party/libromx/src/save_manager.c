#if !defined(_WIN32)
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "romx_internal.h"
#include "save_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

typedef struct save_file_record {
    char *path;
    char *source_path;
    uint64_t size;
} save_file_record_t;

typedef struct save_candidate_record {
    char *key;
    char *display_name;
    char *source_path;
    char title_id[ROMX_SAVE_TITLE_ID_CAPACITY + 1U];
    char extdata_id[ROMX_SAVE_EXTDATA_ID_CAPACITY + 1U];
    romx_save_candidate_flags_t flags;
    romx_save_source_format_t source_format;
    romx_save_grouping_t grouping;
    romx_save_scope_t scope;
    save_file_record_t *files;
    uint32_t file_count;
    uint64_t data_size;
} save_candidate_record_t;

struct romx_save_catalog {
    romx_save_scan_options_t options;
    romx_save_profile_info_t profile;
    save_candidate_record_t *candidates;
    uint32_t candidate_count;
    uint32_t total_file_count;
    uint64_t total_size;
};

typedef enum save_node_kind {
    SAVE_NODE_OTHER = 0,
    SAVE_NODE_FILE = 1,
    SAVE_NODE_DIRECTORY = 2
} save_node_kind_t;

typedef struct save_directory_entry {
    char *name;
    char *path;
    save_node_kind_t kind;
    uint64_t size;
} save_directory_entry_t;

typedef struct save_directory_entries {
    save_directory_entry_t *values;
    uint32_t count;
} save_directory_entries_t;

static char *save_strdup(const char *value)
{
    size_t size;
    char *copy;
    if (value == NULL) return NULL;
    size = strlen(value);
    if (size == SIZE_MAX) return NULL;
    copy = (char *)malloc(size + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, value, size + 1U);
    return copy;
}

static int save_array_allocation_size(uint32_t current_count,
    size_t element_size, size_t *allocation_size)
{
    uintmax_t item_count = (uintmax_t)current_count + UINTMAX_C(1);
    if (element_size == 0U ||
        item_count > (uintmax_t)(SIZE_MAX / element_size)) return 0;
    *allocation_size = (size_t)item_count * element_size;
    return 1;
}

static char *save_join_path(const char *base, const char *name)
{
    size_t base_size = strlen(base);
    size_t name_size = strlen(name);
    int needs_separator = base_size != 0U &&
        base[base_size - 1U] != '/' && base[base_size - 1U] != '\\';
    size_t total;
    char *joined;
    {
        size_t separator_size = needs_separator ? 1U : 0U;
        if (name_size > SIZE_MAX - separator_size ||
            base_size > SIZE_MAX - separator_size - name_size ||
            base_size + separator_size + name_size == SIZE_MAX)
            return NULL;
    }
    total = base_size + (needs_separator ? 1U : 0U) + name_size;
    joined = (char *)malloc(total + 1U);
    if (joined == NULL) return NULL;
    memcpy(joined, base, base_size);
    if (needs_separator) joined[base_size++] = '/';
    memcpy(joined + base_size, name, name_size + 1U);
    return joined;
}

static const char *save_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash))
        slash = backslash;
    return slash == NULL ? path : slash + 1U;
}

static char *save_parent_copy(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    size_t size;
    char *parent;
    if (backslash != NULL && (slash == NULL || backslash > slash))
        slash = backslash;
    if (slash == NULL || slash == path) return NULL;
    size = (size_t)(slash - path);
    parent = (char *)malloc(size + 1U);
    if (parent == NULL) return NULL;
    memcpy(parent, path, size);
    parent[size] = '\0';
    return parent;
}

static char *save_stem_copy(const char *path)
{
    const char *base = save_basename(path);
    const char *dot = strrchr(base, '.');
    size_t size = strlen(base);
    char *stem;
    if (dot != NULL && dot != base) size = (size_t)(dot - base);
    if (size == 0U) return save_strdup("save");
    stem = (char *)malloc(size + 1U);
    if (stem == NULL) return NULL;
    memcpy(stem, base, size);
    stem[size] = '\0';
    return stem;
}

static int save_name_in(const char *name, const char *const *values,
    size_t value_count)
{
    size_t index;
    for (index = 0U; index < value_count; ++index)
        if (romx_ascii_fold_equal(name, values[index])) return 1;
    return 0;
}

static int save_path_valid(const char *path)
{
    return romx_path_valid(path, ROMX_MUTABLE_BUNDLE_PATH_CAPACITY);
}

static int save_is_hex_title_id(const char *value)
{
    return romx_hex_string(value, ROMX_SAVE_TITLE_ID_CAPACITY);
}

static void save_copy_title_id(const char *value,
    char output[ROMX_SAVE_TITLE_ID_CAPACITY + 1U])
{
    romx_copy_hex_upper(value, ROMX_SAVE_TITLE_ID_CAPACITY, output);
}

static void save_copy_title_id_parts(const char *high, const char *low,
    char output[ROMX_SAVE_TITLE_ID_CAPACITY + 1U])
{
    romx_copy_hex_upper(high, 8U, output);
    romx_copy_hex_upper(low, 8U, output + 8U);
}

static int save_title_id_from_path(const char *path,
    char output[ROMX_SAVE_TITLE_ID_CAPACITY + 1U])
{
    char *cursor = save_strdup(path);
    if (cursor == NULL) return 0;
    while (cursor != NULL) {
        char *parent;
        char *stem = save_stem_copy(cursor);
        if (stem == NULL) {
            free(cursor);
            return 0;
        }
        if (save_is_hex_title_id(stem)) {
            save_copy_title_id(stem, output);
            free(stem);
            free(cursor);
            return 1;
        }
        free(stem);
        parent = save_parent_copy(cursor);
        free(cursor);
        cursor = parent;
    }
    return 0;
}

static int save_is_known_extension(const char *name)
{
    static const char *const extensions[] = {
        "sav", "save", "srm", "dsv", "eep", "eeprom", "ram", "sra",
        "fla", "flash", "rtc", "mcr", "gci", "dat"
    };
    const char *base = save_basename(name);
    const char *dot = strrchr(base, '.');
    size_t index;
    if (dot == NULL || dot[1] == '\0') return 0;
    for (index = 0U; index < sizeof(extensions) / sizeof(extensions[0]);
         ++index)
        if (romx_ascii_fold_equal(dot + 1U, extensions[index])) return 1;
    return 0;
}

static int save_is_3ds_noise_file(const char *name)
{
    static const char *const extensions[] = {
        "cia", "3ds", "3dsx", "rar", "zip", "7z", "tar", "gz",
        "bz2", "jpg", "jpeg", "png", "gif", "bmp", "webp", "txt",
        "md", "html", "htm", "pdf", "exe", "dll", "dylib", "app"
    };
    const char *base = save_basename(name);
    const char *dot = strrchr(base, '.');
    size_t index;
    if (dot == NULL || dot[1] == '\0') return 0;
    for (index = 0U; index < sizeof(extensions) / sizeof(extensions[0]);
         ++index)
        if (romx_ascii_fold_equal(dot + 1U, extensions[index])) return 1;
    return 0;
}

static int save_is_3ds_candidate_file(const char *name)
{
    return !save_is_3ds_noise_file(name);
}

static int save_is_3ds_likely_file(const char *name)
{
    static const char *const names[] = {
        "Data0", "Data1", "Data2", "save00.bin", "save_data",
        "system.dat", "system_data", "account_data", "securevalue",
        "system", "user1", "user2", "user3", "game0", "option", "pass0"
    };
    const char *base = save_basename(name);
    const char *dot;
    if (save_name_in(base, names, sizeof(names) / sizeof(names[0]))) return 1;
    dot = strrchr(base, '.');
    return dot != NULL && (romx_ascii_fold_equal(dot + 1U, "sav") ||
        romx_ascii_fold_equal(dot + 1U, "save") ||
        romx_ascii_fold_equal(dot + 1U, "bin") ||
        romx_ascii_fold_equal(dot + 1U, "dat"));
}

static int save_should_skip_name(const char *name, uint32_t flags)
{
    static const char *const noise[] = {
        ".DS_Store", "Thumbs.db", "desktop.ini"
    };
    if (name == NULL || name[0] == '\0' ||
        romx_ascii_fold_equal(name, ".") || romx_ascii_fold_equal(name, "..")) return 1;
    if ((flags & ROMX_SAVE_SCAN_INCLUDE_HIDDEN) == 0U && name[0] == '.')
        return 1;
    return save_name_in(name, noise, sizeof(noise) / sizeof(noise[0]));
}

#if defined(_WIN32)
static wchar_t *save_utf8_to_wide(const char *value)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value, -1, NULL, 0);
    wchar_t *wide;
    if (count <= 0) return NULL;
    wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value, -1, wide, count) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static char *save_wide_to_utf8(const wchar_t *value)
{
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value, -1, NULL, 0, NULL, NULL);
    char *utf8;
    if (count <= 0) return NULL;
    utf8 = (char *)malloc((size_t)count);
    if (utf8 == NULL) return NULL;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            value, -1, utf8, count, NULL, NULL) <= 0) {
        free(utf8);
        return NULL;
    }
    return utf8;
}
#endif

static save_node_kind_t save_stat_path(const char *path, uint64_t *size,
    romx_error_t *error)
{
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA data;
    wchar_t *wide = save_utf8_to_wide(path);
    if (wide == NULL || !GetFileAttributesExW(wide, GetFileExInfoStandard,
            &data)) {
        DWORD code = GetLastError();
        free(wide);
        (void)romx_error_set(error, ROMX_E_IO, (int32_t)code,
            ROMX_OFFSET_UNKNOWN, "failed to inspect SAVE source path");
        return SAVE_NODE_OTHER;
    }
    free(wide);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        return SAVE_NODE_OTHER;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        if (size != NULL) *size = UINT64_C(0);
        return SAVE_NODE_DIRECTORY;
    }
    if (size != NULL) {
        *size = ((uint64_t)data.nFileSizeHigh << 32U) |
            (uint64_t)data.nFileSizeLow;
    }
    return SAVE_NODE_FILE;
#else
    struct stat status;
    if (lstat(path, &status) != 0) {
        (void)romx_error_set(error, ROMX_E_IO, errno, ROMX_OFFSET_UNKNOWN,
            "failed to inspect SAVE source path");
        return SAVE_NODE_OTHER;
    }
    if (S_ISLNK(status.st_mode)) return SAVE_NODE_OTHER;
    if (S_ISDIR(status.st_mode)) {
        if (size != NULL) *size = UINT64_C(0);
        return SAVE_NODE_DIRECTORY;
    }
    if (S_ISREG(status.st_mode)) {
        if (status.st_size < 0) {
            (void)romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "SAVE file size is negative");
            return SAVE_NODE_OTHER;
        }
        if (size != NULL) *size = (uint64_t)status.st_size;
        return SAVE_NODE_FILE;
    }
    return SAVE_NODE_OTHER;
#endif
}

static void save_directory_entries_destroy(save_directory_entries_t *entries)
{
    uint32_t index;
    if (entries == NULL) return;
    for (index = 0U; index < entries->count; ++index) {
        free(entries->values[index].name);
        free(entries->values[index].path);
    }
    free(entries->values);
    memset(entries, 0, sizeof(*entries));
}

static romx_result_t save_directory_entries_add(
    save_directory_entries_t *entries, const char *name, const char *path,
    save_node_kind_t kind, uint64_t size, romx_error_t *error)
{
    save_directory_entry_t *grown;
    char *name_copy;
    char *path_copy;
    size_t allocation_size;
    if (entries->count == UINT32_MAX) {
        return romx_error_set(error, ROMX_E_RANGE, 0, ROMX_OFFSET_UNKNOWN,
            "too many entries in SAVE directory");
    }
    if (!save_array_allocation_size(entries->count, sizeof(*grown),
            &allocation_size)) {
        return romx_error_set(error, ROMX_E_RANGE, 0, ROMX_OFFSET_UNKNOWN,
            "SAVE directory listing size overflows");
    }
    grown = (save_directory_entry_t *)realloc(entries->values,
        allocation_size);
    if (grown == NULL) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to grow SAVE directory listing");
    }
    name_copy = save_strdup(name);
    path_copy = save_strdup(path);
    if (name_copy == NULL || path_copy == NULL) {
        free(name_copy);
        free(path_copy);
        entries->values = grown;
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to copy SAVE directory entry");
    }
    entries->values = grown;
    entries->values[entries->count].name = name_copy;
    entries->values[entries->count].path = path_copy;
    entries->values[entries->count].kind = kind;
    entries->values[entries->count].size = size;
    ++entries->count;
    return ROMX_OK;
}

static int save_directory_entry_compare(const void *left, const void *right)
{
    const save_directory_entry_t *a =
        (const save_directory_entry_t *)left;
    const save_directory_entry_t *b =
        (const save_directory_entry_t *)right;
    return strcmp(a->name, b->name);
}

static romx_result_t save_directory_entries_read(const char *path,
    save_directory_entries_t *entries, romx_error_t *error)
{
    memset(entries, 0, sizeof(*entries));
#if defined(_WIN32)
    {
        wchar_t *wide = save_utf8_to_wide(path);
        wchar_t *pattern;
        size_t path_size;
        WIN32_FIND_DATAW data;
        HANDLE handle;
        if (wide == NULL) {
            return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
                ROMX_OFFSET_UNKNOWN, "SAVE directory path is not valid UTF-8");
        }
        path_size = wcslen(wide);
        pattern = (wchar_t *)malloc((path_size + 3U) * sizeof(*pattern));
        if (pattern == NULL) {
            free(wide);
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN, "failed to allocate SAVE directory path");
        }
        memcpy(pattern, wide, path_size * sizeof(*pattern));
        if (path_size != 0U && pattern[path_size - 1U] != L'/' &&
            pattern[path_size - 1U] != L'\\') pattern[path_size++] = L'/';
        pattern[path_size++] = L'*';
        pattern[path_size] = L'\0';
        handle = FindFirstFileW(pattern, &data);
        free(pattern);
        free(wide);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            return romx_error_set(error, ROMX_E_IO, (int32_t)code,
                ROMX_OFFSET_UNKNOWN, "failed to read SAVE directory");
        }
        do {
            char *name = save_wide_to_utf8(data.cFileName);
            char *child;
            save_node_kind_t kind;
            uint64_t size = UINT64_C(0);
            romx_result_t result;
            if (name == NULL) {
                FindClose(handle);
                save_directory_entries_destroy(entries);
                return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
                    ROMX_OFFSET_UNKNOWN,
                    "SAVE directory contains invalid UTF-8");
            }
            if (romx_ascii_fold_equal(name, ".") || romx_ascii_fold_equal(name, "..")) {
                free(name);
            } else {
                child = save_join_path(path, name);
                if (child == NULL) {
                    free(name);
                    FindClose(handle);
                    save_directory_entries_destroy(entries);
                    return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                        ROMX_OFFSET_UNKNOWN, "failed to allocate SAVE child path");
                }
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
                    free(child);
                    free(name);
                } else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
                    kind = SAVE_NODE_DIRECTORY;
                    result = save_directory_entries_add(entries, name, child,
                        kind, size, error);
                    free(child);
                    free(name);
                    if (result != ROMX_OK) {
                        FindClose(handle);
                        save_directory_entries_destroy(entries);
                        return result;
                    }
                } else {
                    size = ((uint64_t)data.nFileSizeHigh << 32U) |
                        (uint64_t)data.nFileSizeLow;
                    kind = SAVE_NODE_FILE;
                    result = save_directory_entries_add(entries, name, child,
                        kind, size, error);
                    free(child);
                    free(name);
                    if (result != ROMX_OK) {
                        FindClose(handle);
                        save_directory_entries_destroy(entries);
                        return result;
                    }
                }
            }
        } while (FindNextFileW(handle, &data));
        FindClose(handle);
    }
#else
    {
        DIR *directory = opendir(path);
        struct dirent *entry;
        if (directory == NULL) {
            return romx_error_set(error, ROMX_E_IO, errno,
                ROMX_OFFSET_UNKNOWN, "failed to read SAVE directory");
        }
        for (;;) {
            char *child;
            save_node_kind_t kind;
            uint64_t size = UINT64_C(0);
            romx_result_t result;
            romx_error_t detail;
            errno = 0;
            entry = readdir(directory);
            if (entry == NULL) {
                int code = errno;
                if (code == 0) break;
                closedir(directory);
                save_directory_entries_destroy(entries);
                return romx_error_set(error, ROMX_E_IO, code,
                    ROMX_OFFSET_UNKNOWN, "failed to read SAVE directory");
            }
            if (romx_ascii_fold_equal(entry->d_name, ".") ||
                romx_ascii_fold_equal(entry->d_name, "..")) continue;
            child = save_join_path(path, entry->d_name);
            if (child == NULL) {
                closedir(directory);
                save_directory_entries_destroy(entries);
                return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                    ROMX_OFFSET_UNKNOWN, "failed to allocate SAVE child path");
            }
            romx_error_clear(&detail);
            kind = save_stat_path(child, &size, &detail);
            if (kind == SAVE_NODE_OTHER) {
                free(child);
                if (detail.code != ROMX_OK) {
                    closedir(directory);
                    save_directory_entries_destroy(entries);
                    if (error != NULL) *error = detail;
                    return detail.code;
                }
                continue;
            }
            result = save_directory_entries_add(entries, entry->d_name, child,
                kind, size, error);
            free(child);
            if (result != ROMX_OK) {
                closedir(directory);
                save_directory_entries_destroy(entries);
                return result;
            }
        }
        if (closedir(directory) != 0) {
            save_directory_entries_destroy(entries);
            return romx_error_set(error, ROMX_E_IO, errno,
                ROMX_OFFSET_UNKNOWN, "failed to close SAVE directory");
        }
    }
#endif
    if (entries->count > 1U)
        qsort(entries->values, entries->count, sizeof(*entries->values),
            save_directory_entry_compare);
    romx_error_clear(error);
    return ROMX_OK;
}

static void save_file_records_destroy(save_file_record_t *files,
    uint32_t count)
{
    uint32_t index;
    if (files == NULL) return;
    for (index = 0U; index < count; ++index) {
        free(files[index].path);
        free(files[index].source_path);
    }
    free(files);
}

static void save_candidate_destroy(save_candidate_record_t *candidate)
{
    if (candidate == NULL) return;
    free(candidate->key);
    free(candidate->display_name);
    free(candidate->source_path);
    save_file_records_destroy(candidate->files, candidate->file_count);
    memset(candidate, 0, sizeof(*candidate));
}

static void save_catalog_destroy_records(romx_save_catalog_t *catalog)
{
    uint32_t index;
    if (catalog == NULL) return;
    for (index = 0U; index < catalog->candidate_count; ++index)
        save_candidate_destroy(&catalog->candidates[index]);
    free(catalog->candidates);
    catalog->candidates = NULL;
    catalog->candidate_count = 0U;
}

static romx_result_t save_options_effective(
    const romx_save_scan_options_t *provided,
    romx_save_scan_options_t *options, romx_error_t *error)
{
    *options = (romx_save_scan_options_t)ROMX_SAVE_SCAN_OPTIONS_INIT;
    if (provided != NULL) {
        if (provided->struct_size < (uint32_t)sizeof(*provided) ||
            (provided->flags & ~ROMX_SAVE_SCAN_FLAGS_MASK) != 0U ||
            provided->reserved != UINT32_C(0) ||
            provided->source_format_hint > ROMX_SAVE_SOURCE_ROMX_BUNDLE) {
            return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
                ROMX_OFFSET_UNKNOWN, "invalid SAVE scan options");
        }
        *options = *provided;
    }
    if (options->max_candidate_count == UINT32_C(0))
        options->max_candidate_count = ROMX_SAVE_DEFAULT_MAX_CANDIDATES;
    if (options->max_file_count == UINT32_C(0))
        options->max_file_count = ROMX_SAVE_DEFAULT_MAX_FILES;
    if (options->max_total_size == UINT64_C(0))
        options->max_total_size = ROMX_SAVE_DEFAULT_MAX_SIZE;
    if (options->max_depth == UINT32_C(0))
        options->max_depth = ROMX_SAVE_DEFAULT_MAX_DEPTH;
    return ROMX_OK;
}

static void save_profile_compute(uint16_t platform_id, uint16_t format_id,
    uint16_t launch_format_id, romx_save_profile_info_t *profile)
{
    *profile = (romx_save_profile_info_t)ROMX_SAVE_PROFILE_INFO_INIT;
    profile->platform_id = platform_id;
    profile->format_id = format_id;
    profile->launch_format_id = launch_format_id;
    if (platform_id == ROMX_PLATFORM_PSP || format_id == ROMX_FORMAT_PBP) {
        profile->grouping = ROMX_SAVE_GROUP_MARKER_DIRECTORY;
        (void)snprintf(profile->marker, sizeof(profile->marker), "%s",
            "PARAM.SFO");
        profile->marker_size = (uint32_t)strlen(profile->marker);
    } else if (platform_id == ROMX_PLATFORM_NINTENDO_3DS) {
        profile->grouping = ROMX_SAVE_GROUP_DIRECTORY_PER_SAVE;
    } else {
        profile->grouping = ROMX_SAVE_GROUP_SINGLE_FILE;
    }
}

romx_result_t romx_save_profile_get(uint16_t platform_id, uint16_t format_id,
    uint16_t launch_format_id, romx_save_profile_info_t *profile,
    romx_error_t *error)
{
    uint32_t supplied_size;
    if (profile == NULL || profile->struct_size < (uint32_t)sizeof(*profile))
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid SAVE profile output");
    supplied_size = profile->struct_size;
    save_profile_compute(platform_id, format_id, launch_format_id, profile);
    profile->struct_size = supplied_size;
    romx_error_clear(error);
    return ROMX_OK;
}

static romx_result_t save_unique_key(const romx_save_catalog_t *catalog,
    const char *base, char **output, romx_error_t *error)
{
    char candidate[ROMX_MUTABLE_KEY_CAPACITY + 1U];
    const char *fallback = base != NULL && base[0] != '\0' ? base : "save";
    uint32_t suffix = 1U;
    int used;
    int written;
    if (!save_path_valid(fallback)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "SAVE candidate key is not portable");
    }
    for (;;) {
        uint32_t index;
        if (suffix == 1U) {
            written = snprintf(candidate, sizeof(candidate), "%s", fallback);
        } else {
            written = snprintf(candidate, sizeof(candidate), "%s (%u)",
                fallback, (unsigned int)suffix);
        }
        if (written < 0 || (size_t)written >= sizeof(candidate)) {
            return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
                ROMX_OFFSET_UNKNOWN, "SAVE candidate key is too long");
        }
        used = 0;
        for (index = 0U; index < catalog->candidate_count; ++index) {
            if (romx_ascii_fold_equal(catalog->candidates[index].key,
                    candidate)) {
                used = 1;
                break;
            }
        }
        if (!used) {
            *output = save_strdup(candidate);
            if (*output == NULL) {
                return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                    ROMX_OFFSET_UNKNOWN, "failed to allocate SAVE candidate key");
            }
            return ROMX_OK;
        }
        if (suffix == UINT32_MAX) {
            return romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "SAVE candidate key suffix overflows");
        }
        ++suffix;
    }
}

static int save_directory_contains_name(const save_directory_entries_t *entries,
    const char *name)
{
    uint32_t index;
    for (index = 0U; index < entries->count; ++index)
        if (entries->values[index].kind == SAVE_NODE_FILE &&
            romx_ascii_fold_equal(entries->values[index].name, name)) return 1;
    return 0;
}

/* Recognizes only the strict SaveDataFiler interchange shape:
 *   <editable-label>/export.log
 *   <editable-label>/<id>.dat
 *   <editable-label>/<id>_.dat
 *   <editable-label>/<id>/<all save files>
 * The label is intentionally ignored; the eight-digit child ID is the
 * ExtData identity used by both the writer and the mutable reader. */
static int save_directory_is_strict_savedatafiler(
    const save_directory_entries_t *entries, char low[9])
{
    romx_savedatafiler_shape_t shape = { { 0 }, 0U };
    uint32_t index;
    for (index = 0U; index < entries->count; ++index) {
        const save_directory_entry_t *entry = &entries->values[index];
        if (save_should_skip_name(entry->name, 0U)) continue;
        if (!romx_savedatafiler_add(&shape, entry->name,
                entry->kind == SAVE_NODE_DIRECTORY)) return 0;
    }
    return romx_savedatafiler_finish(&shape, low);
}

static int save_directory_has_citra_save_data(
    const save_directory_entries_t *entries)
{
    uint32_t index;
    for (index = 0U; index < entries->count; ++index) {
        const save_directory_entry_t *entry = &entries->values[index];
        if (entry->kind == SAVE_NODE_FILE &&
            romx_ascii_fold_equal(entry->name, "saveData.bin")) return 1;
    }
    return 0;
}

static int save_directory_has_3ds_markers(
    const save_directory_entries_t *entries)
{
    static const char *const markers[] = {
        "save00.bin", "save_data", "system.dat", "system_data", "Data0",
        "Data1", "Data2", "account_data", "securevalue"
    };
    uint32_t index;
    for (index = 0U; index < entries->count; ++index) {
        if (entries->values[index].kind == SAVE_NODE_FILE &&
            save_name_in(entries->values[index].name, markers,
                sizeof(markers) / sizeof(markers[0]))) return 1;
    }
    return 0;
}

static int save_directory_has_gateway_file(
    const save_directory_entries_t *entries,
    char title_id[ROMX_SAVE_TITLE_ID_CAPACITY + 1U])
{
    uint32_t index;
    for (index = 0U; index < entries->count; ++index) {
        const save_directory_entry_t *entry = &entries->values[index];
        const char *dot;
        char *stem;
        if (entry->kind != SAVE_NODE_FILE) continue;
        dot = strrchr(save_basename(entry->name), '.');
        if (dot == NULL || !romx_ascii_fold_equal(dot + 1U, "sav")) continue;
        stem = save_stem_copy(entry->name);
        if (stem == NULL) return 0;
        if (save_is_hex_title_id(stem)) {
            save_copy_title_id(stem, title_id);
            free(stem);
            return 1;
        }
        free(stem);
    }
    return 0;
}

static int save_citra_candidate_title(const char *path,
    char output[ROMX_SAVE_TITLE_ID_CAPACITY + 1U])
{
    char *parent = save_parent_copy(path);
    char *grandparent;
    char *greatgrandparent = NULL;
    int found = 0;
    if (parent == NULL || !romx_ascii_fold_equal(save_basename(parent), "data")) {
        free(parent);
        return 0;
    }
    grandparent = save_parent_copy(parent);
    if (grandparent != NULL && save_is_hex_title_id(save_basename(grandparent))) {
        save_copy_title_id(save_basename(grandparent), output);
        found = 1;
    } else if (grandparent != NULL &&
               romx_hex_string(save_basename(grandparent), 8U)) {
        // Azahar/Citra's on-disk SD layout splits the 16-digit Title ID into
        // title/<high-8>/<low-8>/data/00000001. Treat that leaf exactly like
        // the older title/<16-digit>/data/00000001 layout.
        greatgrandparent = save_parent_copy(grandparent);
        if (greatgrandparent != NULL &&
            romx_hex_string(save_basename(greatgrandparent), 8U)) {
            save_copy_title_id_parts(save_basename(greatgrandparent),
                save_basename(grandparent), output);
            found = 1;
        }
    }
    free(greatgrandparent);
    free(grandparent);
    free(parent);
    return found && romx_ascii_fold_equal(save_basename(path), "00000001");
}

static int save_citra_candidate_extdata(const char *path,
    char output[ROMX_SAVE_EXTDATA_ID_CAPACITY + 1U])
{
    char *parent = save_parent_copy(path);
    char *grandparent = NULL;
    int found = 0;
    if (parent != NULL && romx_hex_string(save_basename(path), 8U) &&
        romx_hex_string(save_basename(parent), 8U)) {
        grandparent = save_parent_copy(parent);
        if (grandparent != NULL &&
            romx_ascii_fold_equal(save_basename(grandparent), "extdata")) {
            save_copy_title_id_parts(save_basename(parent),
                save_basename(path), output);
            found = 1;
        }
    }
    free(grandparent);
    free(parent);
    return found;
}

static void save_copy_extdata_id_from_leaf(const char *value,
    char output[ROMX_SAVE_EXTDATA_ID_CAPACITY + 1U])
{
    static const char zero_high[] = "00000000";
    save_copy_title_id_parts(zero_high, value, output);
}

static romx_save_source_format_t save_classify_file(
    const romx_save_catalog_t *catalog, const char *path,
    char title_id[ROMX_SAVE_TITLE_ID_CAPACITY + 1U])
{
    if (catalog->options.source_format_hint != ROMX_SAVE_SOURCE_AUTO)
        return catalog->options.source_format_hint;
    if (catalog->profile.platform_id == ROMX_PLATFORM_NINTENDO_3DS &&
        save_title_id_from_path(path, title_id) &&
        romx_ascii_fold_equal(strrchr(save_basename(path), '.') == NULL
                ? "" : strrchr(save_basename(path), '.') + 1U, "sav"))
        return ROMX_SAVE_SOURCE_3DS_GATEWAY;
    return ROMX_SAVE_SOURCE_FILE;
}

static romx_save_source_format_t save_classify_directory(
    const romx_save_catalog_t *catalog, const char *path,
    const save_directory_entries_t *entries,
    char title_id[ROMX_SAVE_TITLE_ID_CAPACITY + 1U],
    char extdata_id[ROMX_SAVE_EXTDATA_ID_CAPACITY + 1U])
{
    romx_save_source_format_t source = ROMX_SAVE_SOURCE_DIRECTORY;
    char low[9];
    if (catalog->profile.grouping == ROMX_SAVE_GROUP_MARKER_DIRECTORY) {
        source = ROMX_SAVE_SOURCE_PSP_SAVEDATA;
    } else if (catalog->profile.platform_id == ROMX_PLATFORM_NINTENDO_3DS) {
        if (save_directory_is_strict_savedatafiler(entries, low)) {
            save_copy_extdata_id_from_leaf(low, extdata_id);
            source = ROMX_SAVE_SOURCE_3DS_SAVEDATAFILER;
        } else if (save_citra_candidate_extdata(path, extdata_id) ||
                   save_citra_candidate_title(path, title_id) ||
                   save_directory_has_citra_save_data(entries) ||
                   romx_ascii_fold_equal(save_basename(path), "00000001")) {
            source = ROMX_SAVE_SOURCE_3DS_CITRA;
        } else if (save_directory_has_gateway_file(entries, title_id)) {
            source = ROMX_SAVE_SOURCE_3DS_GATEWAY;
        } else {
            source = ROMX_SAVE_SOURCE_3DS_BACKUP;
        }
    }
    return catalog->options.source_format_hint == ROMX_SAVE_SOURCE_AUTO
        ? source : catalog->options.source_format_hint;
}

static romx_result_t save_candidate_add(
    romx_save_catalog_t *catalog, const char *path, int is_directory,
    romx_save_grouping_t grouping, romx_save_source_format_t source_format,
    romx_error_t *error, save_candidate_record_t **output)
{
    save_candidate_record_t candidate;
    save_directory_entries_t features;
    char *base = NULL;
    char *key = NULL;
    char title_id[ROMX_SAVE_TITLE_ID_CAPACITY + 1U] = { 0 };
    char extdata_id[ROMX_SAVE_EXTDATA_ID_CAPACITY + 1U] = { 0 };
    romx_result_t result;
    save_node_kind_t kind;
    uint64_t unused_size = UINT64_C(0);
    memset(&candidate, 0, sizeof(candidate));
    memset(&features, 0, sizeof(features));
    if (catalog->candidate_count >= catalog->options.max_candidate_count)
        return romx_error_set(error, ROMX_E_RANGE, 0, ROMX_OFFSET_UNKNOWN,
            "SAVE candidate count exceeds the configured limit");
    romx_error_clear(error);
    kind = save_stat_path(path, &unused_size, error);
    if ((is_directory && kind != SAVE_NODE_DIRECTORY) ||
        (!is_directory && kind != SAVE_NODE_FILE)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "SAVE candidate source type changed");
    }
    if (is_directory) {
        result = save_directory_entries_read(path, &features, error);
        if (result != ROMX_OK) return result;
        base = save_strdup(save_basename(path));
        if (base == NULL) {
            save_directory_entries_destroy(&features);
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN, "failed to allocate SAVE directory name");
        }
        {
            romx_save_source_format_t detected = save_classify_directory(
                catalog, path, &features, title_id, extdata_id);
            if (source_format == ROMX_SAVE_SOURCE_AUTO) source_format = detected;
        }
    } else {
        base = save_stem_copy(path);
        if (base == NULL) {
            save_directory_entries_destroy(&features);
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN, "failed to allocate SAVE file name");
        }
        if (source_format == ROMX_SAVE_SOURCE_AUTO)
            source_format = save_classify_file(catalog, path, title_id);
    }
    if (title_id[0] == '\0' && catalog->profile.platform_id == ROMX_PLATFORM_NINTENDO_3DS)
        (void)save_title_id_from_path(path, title_id);
    result = save_unique_key(catalog, base, &key, error);
    if (result != ROMX_OK) {
        free(base);
        save_directory_entries_destroy(&features);
        return result;
    }
    candidate.key = key;
    candidate.display_name = base;
    candidate.source_path = save_strdup(path);
    candidate.source_format = source_format;
    candidate.grouping = grouping;
    if (catalog->profile.platform_id == ROMX_PLATFORM_NINTENDO_3DS) {
        if (extdata_id[0] != '\0') {
            memcpy(candidate.extdata_id, extdata_id,
                sizeof(candidate.extdata_id));
            candidate.scope = ROMX_SAVE_SCOPE_3DS_EXTDATA;
        } else {
            /* For 3DS, every non-ExtData candidate is a Title Save.  This
             * includes a flat Citra/Azahar folder containing only
             * saveData.bin and a Gateway/single-file export; the actual native
             * title path is supplied by the consuming ROMX adapter. */
            candidate.scope = ROMX_SAVE_SCOPE_3DS_TITLE;
        }
    }
    if (candidate.source_path == NULL) {
        save_candidate_destroy(&candidate);
        save_directory_entries_destroy(&features);
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to copy SAVE candidate source path");
    }
    candidate.flags = is_directory ? ROMX_SAVE_CANDIDATE_IS_DIRECTORY : 0U;
    if (title_id[0] != '\0') {
        memcpy(candidate.title_id, title_id, sizeof(candidate.title_id));
        candidate.flags |= ROMX_SAVE_CANDIDATE_HAS_TITLE_ID;
    }
    if (source_format == ROMX_SAVE_SOURCE_PSP_SAVEDATA)
        candidate.flags |= ROMX_SAVE_CANDIDATE_HAS_MARKER;
    if (source_format == ROMX_SAVE_SOURCE_3DS_SAVEDATAFILER &&
        (candidate.flags & ROMX_SAVE_CANDIDATE_HAS_TITLE_ID) == 0U)
        candidate.flags |= ROMX_SAVE_CANDIDATE_NEEDS_TITLE_MAP;
    candidate.files = NULL;
    candidate.file_count = 0U;
    candidate.data_size = UINT64_C(0);
    if (catalog->candidate_count == UINT32_MAX) {
        save_candidate_destroy(&candidate);
        save_directory_entries_destroy(&features);
        return romx_error_set(error, ROMX_E_RANGE, 0, ROMX_OFFSET_UNKNOWN,
            "SAVE candidate count overflows");
    }
    {
        save_candidate_record_t *grown;
        size_t allocation_size;
        if (!save_array_allocation_size(catalog->candidate_count,
                sizeof(*grown), &allocation_size)) {
            save_candidate_destroy(&candidate);
            save_directory_entries_destroy(&features);
            return romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "SAVE catalog size overflows");
        }
        grown = (save_candidate_record_t *)realloc(catalog->candidates,
            allocation_size);
        if (grown == NULL) {
            save_candidate_destroy(&candidate);
            save_directory_entries_destroy(&features);
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN, "failed to grow SAVE catalog");
        }
        catalog->candidates = grown;
    }
    catalog->candidates[catalog->candidate_count] = candidate;
    *output = &catalog->candidates[catalog->candidate_count];
    ++catalog->candidate_count;
    save_directory_entries_destroy(&features);
    return ROMX_OK;
}

static void save_remove_last_candidate(romx_save_catalog_t *catalog)
{
    if (catalog->candidate_count == 0U) return;
    save_candidate_destroy(&catalog->candidates[catalog->candidate_count - 1U]);
    --catalog->candidate_count;
}

static romx_result_t save_candidate_add_file(
    romx_save_catalog_t *catalog, save_candidate_record_t *candidate,
    const char *relative_path, const char *source_path, uint64_t size,
    romx_error_t *error)
{
    save_file_record_t *grown;
    uint32_t index;
    char *path_copy;
    char *source_copy;
    size_t allocation_size;
    if (!save_path_valid(relative_path))
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "SAVE file path is not portable");
    if (catalog->total_file_count >= catalog->options.max_file_count ||
        candidate->file_count >= catalog->options.max_file_count)
        return romx_error_set(error, ROMX_E_RANGE, 0, ROMX_OFFSET_UNKNOWN,
            "SAVE file count exceeds the configured limit");
    if (size > catalog->options.max_total_size ||
        catalog->total_size > catalog->options.max_total_size - size)
        return romx_error_set(error, ROMX_E_RANGE, 0, ROMX_OFFSET_UNKNOWN,
            "SAVE data size exceeds the configured limit");
    for (index = 0U; index < candidate->file_count; ++index) {
        if (romx_ascii_fold_equal(candidate->files[index].path, relative_path))
            return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
                ROMX_OFFSET_UNKNOWN, "SAVE file paths are not portable-unique");
    }
    if (candidate->data_size > UINT64_MAX - size ||
        catalog->total_size > UINT64_MAX - size)
        return romx_error_set(error, ROMX_E_RANGE, 0, ROMX_OFFSET_UNKNOWN,
            "SAVE data size overflows");
    if (candidate->file_count == UINT32_MAX) {
        return romx_error_set(error, ROMX_E_RANGE, 0, ROMX_OFFSET_UNKNOWN,
            "SAVE file list count overflows");
    }
    if (!save_array_allocation_size(candidate->file_count, sizeof(*grown),
            &allocation_size)) {
        return romx_error_set(error, ROMX_E_RANGE, 0, ROMX_OFFSET_UNKNOWN,
            "SAVE file list size overflows");
    }
    grown = (save_file_record_t *)realloc(candidate->files,
        allocation_size);
    if (grown == NULL)
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to grow SAVE file list");
    path_copy = save_strdup(relative_path);
    source_copy = save_strdup(source_path);
    if (path_copy == NULL || source_copy == NULL) {
        free(path_copy);
        free(source_copy);
        candidate->files = grown;
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to copy SAVE file path");
    }
    candidate->files = grown;
    candidate->files[candidate->file_count].path = path_copy;
    candidate->files[candidate->file_count].source_path = source_copy;
    candidate->files[candidate->file_count].size = size;
    ++candidate->file_count;
    ++catalog->total_file_count;
    candidate->data_size += size;
    catalog->total_size += size;
    return ROMX_OK;
}

static romx_result_t save_collect_candidate_files(
    romx_save_catalog_t *catalog, save_candidate_record_t *candidate,
    const char *directory, const char *prefix,
    uint32_t depth, romx_error_t *error)
{
    save_directory_entries_t entries;
    uint32_t index;
    romx_result_t result;
    if (depth > catalog->options.max_depth)
        return romx_error_set(error, ROMX_E_RANGE, 0, ROMX_OFFSET_UNKNOWN,
            "SAVE directory depth exceeds the configured limit");
    result = save_directory_entries_read(directory, &entries, error);
    if (result != ROMX_OK) return result;
    for (index = 0U; index < entries.count; ++index) {
        const save_directory_entry_t *entry = &entries.values[index];
        char *relative;
        if (save_should_skip_name(entry->name, catalog->options.flags)) continue;
        if (entry->kind == SAVE_NODE_DIRECTORY) {
            if (depth == catalog->options.max_depth) {
                save_directory_entries_destroy(&entries);
                return romx_error_set(error, ROMX_E_RANGE, 0,
                    ROMX_OFFSET_UNKNOWN,
                    "SAVE directory depth exceeds the configured limit");
            }
            relative = save_join_path(prefix, entry->name);
            if (relative == NULL) {
                save_directory_entries_destroy(&entries);
                return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                    ROMX_OFFSET_UNKNOWN, "failed to allocate SAVE relative path");
            }
            result = save_collect_candidate_files(catalog, candidate,
                entry->path, relative, depth + 1U, error);
            free(relative);
            if (result != ROMX_OK) {
                save_directory_entries_destroy(&entries);
                return result;
            }
        } else if (entry->kind == SAVE_NODE_FILE) {
            relative = save_join_path(prefix, entry->name);
            if (relative == NULL) {
                save_directory_entries_destroy(&entries);
                return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                    ROMX_OFFSET_UNKNOWN, "failed to allocate SAVE relative path");
            }
            result = save_candidate_add_file(catalog, candidate, relative,
                entry->path, entry->size, error);
            free(relative);
            if (result != ROMX_OK) {
                save_directory_entries_destroy(&entries);
                return result;
            }
        }
    }
    save_directory_entries_destroy(&entries);
    return ROMX_OK;
}

static romx_result_t save_add_file_candidate(romx_save_catalog_t *catalog,
    const char *source_path, const char *relative_path, uint64_t size,
    romx_error_t *error)
{
    save_candidate_record_t *candidate = NULL;
    romx_save_source_format_t source_format = ROMX_SAVE_SOURCE_AUTO;
    romx_result_t result = save_candidate_add(catalog, source_path, 0,
        ROMX_SAVE_GROUP_SINGLE_FILE, source_format, error, &candidate);
    if (result != ROMX_OK) return result;
    result = save_candidate_add_file(catalog, candidate, relative_path,
        source_path, size, error);
    if (result != ROMX_OK) {
        save_remove_last_candidate(catalog);
        return result;
    }
    return ROMX_OK;
}

static romx_result_t save_scan_single_file_tree(
    romx_save_catalog_t *catalog, const char *directory, uint32_t depth,
    romx_error_t *error)
{
    save_directory_entries_t entries;
    uint32_t index;
    romx_result_t result = save_directory_entries_read(directory, &entries,
        error);
    if (result != ROMX_OK) return result;
    for (index = 0U; index < entries.count; ++index) {
        const save_directory_entry_t *entry = &entries.values[index];
        if (save_should_skip_name(entry->name, catalog->options.flags)) continue;
        if (entry->kind == SAVE_NODE_DIRECTORY) {
            if (depth == catalog->options.max_depth) {
                save_directory_entries_destroy(&entries);
                return romx_error_set(error, ROMX_E_RANGE, 0,
                    ROMX_OFFSET_UNKNOWN,
                    "SAVE directory depth exceeds the configured limit");
            }
            result = save_scan_single_file_tree(catalog, entry->path,
                depth + 1U, error);
        } else if (entry->kind == SAVE_NODE_FILE &&
                   save_is_known_extension(entry->name)) {
            result = save_add_file_candidate(catalog, entry->path,
                entry->name, entry->size, error);
        }
        if (result != ROMX_OK) {
            save_directory_entries_destroy(&entries);
            return result;
        }
    }
    save_directory_entries_destroy(&entries);
    return ROMX_OK;
}

static romx_result_t save_scan_marker_tree(romx_save_catalog_t *catalog,
    const char *directory, uint32_t depth, romx_error_t *error)
{
    save_directory_entries_t entries;
    save_candidate_record_t *candidate = NULL;
    uint32_t index;
    romx_result_t result = save_directory_entries_read(directory, &entries,
        error);
    if (result != ROMX_OK) return result;
    if (save_directory_contains_name(&entries, "PARAM.SFO")) {
        save_directory_entries_destroy(&entries);
        result = save_candidate_add(catalog, directory, 1,
            ROMX_SAVE_GROUP_MARKER_DIRECTORY, ROMX_SAVE_SOURCE_PSP_SAVEDATA,
            error, &candidate);
        if (result != ROMX_OK) return result;
        result = save_collect_candidate_files(catalog, candidate,
            directory, "", 0U, error);
        if (result != ROMX_OK || candidate->file_count == 0U) {
            save_remove_last_candidate(catalog);
            if (result == ROMX_OK) {
                return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
                    ROMX_OFFSET_UNKNOWN,
                    "PSP SAVE marker directory contains no files");
            }
            return result;
        }
        candidate->flags |= ROMX_SAVE_CANDIDATE_HAS_MARKER;
        return ROMX_OK;
    }
    for (index = 0U; index < entries.count; ++index) {
        const save_directory_entry_t *entry = &entries.values[index];
        if (save_should_skip_name(entry->name, catalog->options.flags)) continue;
        if (entry->kind == SAVE_NODE_DIRECTORY) {
            if (depth == catalog->options.max_depth) {
                save_directory_entries_destroy(&entries);
                return romx_error_set(error, ROMX_E_RANGE, 0,
                    ROMX_OFFSET_UNKNOWN,
                    "SAVE directory depth exceeds the configured limit");
            }
            result = save_scan_marker_tree(catalog, entry->path, depth + 1U,
                error);
        } else if (entry->kind == SAVE_NODE_FILE &&
                   save_is_known_extension(entry->name)) {
            result = save_add_file_candidate(catalog, entry->path,
                entry->name, entry->size, error);
        }
        if (result != ROMX_OK) {
            save_directory_entries_destroy(&entries);
            return result;
        }
    }
    save_directory_entries_destroy(&entries);
    return ROMX_OK;
}

static romx_result_t save_add_3ds_directory_candidate(
    romx_save_catalog_t *catalog, const char *directory,
    romx_save_source_format_t source_format, romx_error_t *error)
{
    save_candidate_record_t *candidate = NULL;
    romx_result_t result = save_candidate_add(catalog, directory, 1,
        ROMX_SAVE_GROUP_DIRECTORY_PER_SAVE, source_format, error, &candidate);
    if (result != ROMX_OK) return result;
    result = save_collect_candidate_files(catalog, candidate,
        directory, "", 0U, error);
    if (result != ROMX_OK) {
        save_remove_last_candidate(catalog);
        return result;
    }
    if (candidate->file_count == 0U) save_remove_last_candidate(catalog);
    return ROMX_OK;
}

/* 3DS collections found in the wild often have two or three wrapper levels:
 * game name -> timestamp -> 000015d8 for SaveDataFiler, while Gateway exports
 * normally have game name -> title-id.sav.  Recognize those leaves before
 * applying the generic direct-child directory rule, otherwise DLC and tool
 * folders would be imported as saves. */
static romx_result_t save_scan_3ds_tree(romx_save_catalog_t *catalog,
    const char *directory, uint32_t depth, int is_root, romx_error_t *error)
{
    save_directory_entries_t entries;
    uint32_t index;
    uint32_t before;
    romx_result_t result = save_directory_entries_read(directory, &entries,
        error);
    if (result != ROMX_OK) return result;
    {
        char savedatafiler_id[9] = { 0 };
        if (save_directory_is_strict_savedatafiler(&entries,
                savedatafiler_id)) {
            save_directory_entries_destroy(&entries);
            return save_add_3ds_directory_candidate(catalog, directory,
                ROMX_SAVE_SOURCE_3DS_SAVEDATAFILER, error);
        }
    }
    if (save_citra_candidate_extdata(directory,
            (char[ROMX_SAVE_EXTDATA_ID_CAPACITY + 1U]){ 0 })) {
        save_directory_entries_destroy(&entries);
        return save_add_3ds_directory_candidate(catalog, directory,
            ROMX_SAVE_SOURCE_3DS_CITRA, error);
    }
    if (save_citra_candidate_title(directory,
            (char[ROMX_SAVE_TITLE_ID_CAPACITY + 1U]){ 0 })) {
        save_directory_entries_destroy(&entries);
        return save_add_3ds_directory_candidate(catalog, directory,
            ROMX_SAVE_SOURCE_3DS_CITRA, error);
    }
    if (save_directory_has_citra_save_data(&entries) ||
        romx_ascii_fold_equal(save_basename(directory), "00000001")) {
        save_directory_entries_destroy(&entries);
        return save_add_3ds_directory_candidate(catalog, directory,
            ROMX_SAVE_SOURCE_3DS_CITRA, error);
    }
    {
        char title_id[ROMX_SAVE_TITLE_ID_CAPACITY + 1U] = { 0 };
        if (save_directory_has_gateway_file(&entries, title_id)) {
            save_directory_entries_destroy(&entries);
            return save_add_3ds_directory_candidate(catalog, directory,
                ROMX_SAVE_SOURCE_3DS_GATEWAY, error);
        }
    }
    if (save_directory_has_3ds_markers(&entries)) {
        save_directory_entries_destroy(&entries);
        return save_add_3ds_directory_candidate(catalog, directory,
            ROMX_SAVE_SOURCE_3DS_BACKUP, error);
    }
    before = catalog->candidate_count;
    for (index = 0U; index < entries.count; ++index) {
        const save_directory_entry_t *entry = &entries.values[index];
        if (save_should_skip_name(entry->name, catalog->options.flags)) continue;
        if (entry->kind == SAVE_NODE_DIRECTORY) {
            if (depth == catalog->options.max_depth) {
                save_directory_entries_destroy(&entries);
                return romx_error_set(error, ROMX_E_RANGE, 0,
                    ROMX_OFFSET_UNKNOWN,
                    "SAVE directory depth exceeds the configured limit");
            }
            result = save_scan_3ds_tree(catalog, entry->path, depth + 1U, 0,
                error);
        } else if (is_root && entry->kind == SAVE_NODE_FILE &&
                   save_is_3ds_candidate_file(entry->name) &&
                   save_is_3ds_likely_file(entry->name)) {
            result = save_add_file_candidate(catalog, entry->path,
                entry->name, entry->size, error);
        }
        if (result != ROMX_OK) {
            save_directory_entries_destroy(&entries);
            return result;
        }
    }
    if (!is_root && catalog->candidate_count == before) {
        for (index = 0U; index < entries.count; ++index) {
            const save_directory_entry_t *entry = &entries.values[index];
            if (entry->kind == SAVE_NODE_FILE &&
                save_is_3ds_candidate_file(entry->name) &&
                save_is_3ds_likely_file(entry->name)) {
                result = save_add_3ds_directory_candidate(catalog, directory,
                    ROMX_SAVE_SOURCE_3DS_BACKUP, error);
                if (result != ROMX_OK) {
                    save_directory_entries_destroy(&entries);
                    return result;
                }
                break;
            }
        }
    }
    save_directory_entries_destroy(&entries);
    return ROMX_OK;
}

static char *save_extdata_bundle_path(const save_candidate_record_t *candidate,
    const char *relative)
{
    char high[9];
    char low[9];
    char prefix[64];
    char *result;
    size_t index;
    if (candidate == NULL || relative == NULL ||
        !save_is_hex_title_id(candidate->extdata_id) ||
        !save_path_valid(relative)) return NULL;
    memcpy(high, candidate->extdata_id, 8U);
    memcpy(low, candidate->extdata_id + 8U, 8U);
    high[8] = '\0';
    low[8] = '\0';
    for (index = 0U; index < 8U; ++index) {
        if (high[index] >= 'A' && high[index] <= 'F')
            high[index] = (char)(high[index] - 'A' + 'a');
        if (low[index] >= 'A' && low[index] <= 'F')
            low[index] = (char)(low[index] - 'A' + 'a');
    }
    if (snprintf(prefix, sizeof(prefix), "extdata/%s/%s", high, low) < 0)
        return NULL;
    result = save_join_path(prefix, relative);
    if (result == NULL || !save_path_valid(result)) {
        free(result);
        return NULL;
    }
    return result;
}

static romx_result_t save_scan_directory_per_save(
    romx_save_catalog_t *catalog, const char *directory, romx_error_t *error)
{
    romx_result_t result;
    if ((catalog->options.flags & ROMX_SAVE_SCAN_TREAT_ROOT_AS_SAVE) != 0U) {
        save_candidate_record_t *candidate = NULL;
        result = save_candidate_add(catalog, directory, 1,
            ROMX_SAVE_GROUP_DIRECTORY_PER_SAVE, ROMX_SAVE_SOURCE_AUTO, error,
            &candidate);
        if (result != ROMX_OK) return result;
        result = save_collect_candidate_files(catalog, candidate,
            directory, "", 0U, error);
        if (result != ROMX_OK || candidate->file_count == 0U) {
            save_remove_last_candidate(catalog);
            return result != ROMX_OK ? result : romx_error_set(error,
                ROMX_E_MUTABLE_BUNDLE, 0, ROMX_OFFSET_UNKNOWN,
                "SAVE directory contains no regular files");
        }
        return ROMX_OK;
    }
    return save_scan_3ds_tree(catalog, directory, 0U, 1, error);
}

static int save_candidate_compare(const void *left, const void *right)
{
    const save_candidate_record_t *a =
        (const save_candidate_record_t *)left;
    const save_candidate_record_t *b =
        (const save_candidate_record_t *)right;
    return strcmp(a->key, b->key);
}

romx_result_t romx_save_catalog_open_path(const char *utf8_source_path,
    const romx_save_scan_options_t *provided,
    romx_save_catalog_t **out_catalog, romx_error_t *error)
{
    romx_save_catalog_t *catalog;
    romx_save_scan_options_t options;
    save_node_kind_t kind;
    uint64_t source_size = UINT64_C(0);
    romx_result_t result;
    if (out_catalog != NULL) *out_catalog = NULL;
    if (utf8_source_path == NULL || utf8_source_path[0] == '\0' ||
        out_catalog == NULL)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "SAVE source path and output must be provided");
    result = save_options_effective(provided, &options, error);
    if (result != ROMX_OK) return result;
    catalog = (romx_save_catalog_t *)calloc(1U, sizeof(*catalog));
    if (catalog == NULL)
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate SAVE catalog");
    catalog->options = options;
    save_profile_compute(options.platform_id, options.format_id,
        options.launch_format_id, &catalog->profile);
    romx_error_clear(error);
    kind = save_stat_path(utf8_source_path, &source_size, error);
    if (kind == SAVE_NODE_FILE) {
        result = save_add_file_candidate(catalog, utf8_source_path,
            save_basename(utf8_source_path), source_size, error);
    } else if (kind == SAVE_NODE_DIRECTORY) {
        if (catalog->profile.grouping == ROMX_SAVE_GROUP_MARKER_DIRECTORY)
            result = save_scan_marker_tree(catalog, utf8_source_path, 0U,
                error);
        else if (catalog->profile.grouping == ROMX_SAVE_GROUP_DIRECTORY_PER_SAVE)
            result = save_scan_directory_per_save(catalog, utf8_source_path,
                error);
        else
            result = save_scan_single_file_tree(catalog, utf8_source_path, 0U,
                error);
    } else {
        result = romx_error_set(error, ROMX_E_IO, 0, ROMX_OFFSET_UNKNOWN,
            "SAVE source path is not a regular file or directory");
    }
    if (result != ROMX_OK) {
        save_catalog_destroy_records(catalog);
        free(catalog);
        return result;
    }
    if (catalog->candidate_count > 1U)
        qsort(catalog->candidates, catalog->candidate_count,
            sizeof(*catalog->candidates), save_candidate_compare);
    romx_error_clear(error);
    *out_catalog = catalog;
    return ROMX_OK;
}

romx_result_t romx_save_catalog_get_profile(
    const romx_save_catalog_t *catalog, romx_save_profile_info_t *profile,
    romx_error_t *error)
{
    uint32_t supplied_size;
    if (catalog == NULL || profile == NULL ||
        profile->struct_size < (uint32_t)sizeof(*profile))
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid SAVE catalog profile arguments");
    supplied_size = profile->struct_size;
    *profile = catalog->profile;
    profile->struct_size = supplied_size;
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_save_catalog_get_candidate_count(
    const romx_save_catalog_t *catalog, uint32_t *count, romx_error_t *error)
{
    if (catalog == NULL || count == NULL)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid SAVE catalog count arguments");
    *count = catalog->candidate_count;
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_save_catalog_get_candidate(
    const romx_save_catalog_t *catalog, uint32_t index,
    romx_save_candidate_info_t *candidate, romx_error_t *error)
{
    const save_candidate_record_t *stored;
    uint32_t supplied_size;
    size_t key_size;
    size_t display_size;
    size_t title_size;
    if (catalog == NULL || candidate == NULL ||
        candidate->struct_size < (uint32_t)sizeof(*candidate) ||
        index >= catalog->candidate_count)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid SAVE candidate arguments");
    supplied_size = candidate->struct_size;
    stored = &catalog->candidates[index];
    key_size = strlen(stored->key);
    display_size = strlen(stored->display_name);
    title_size = strlen(stored->title_id);
    *candidate = (romx_save_candidate_info_t)ROMX_SAVE_CANDIDATE_INFO_INIT;
    candidate->struct_size = supplied_size;
    candidate->index = index;
    candidate->flags = stored->flags |
        (stored->file_count > 1U ? ROMX_SAVE_CANDIDATE_IS_MULTI_FILE : 0U);
    candidate->source_format = stored->source_format;
    candidate->grouping = stored->grouping;
    candidate->scope = stored->scope;
    candidate->file_count = stored->file_count;
    candidate->data_size = stored->data_size;
    candidate->key_size = (uint32_t)key_size;
    candidate->display_name_size = (uint32_t)display_size;
    candidate->title_id_size = (uint32_t)title_size;
    candidate->extdata_id_size = (uint32_t)strlen(stored->extdata_id);
    memcpy(candidate->key, stored->key, key_size + 1U);
    memcpy(candidate->display_name, stored->display_name, display_size + 1U);
    memcpy(candidate->title_id, stored->title_id, title_size + 1U);
    memcpy(candidate->extdata_id, stored->extdata_id,
        candidate->extdata_id_size + 1U);
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_save_catalog_get_file_count(
    const romx_save_catalog_t *catalog, uint32_t candidate_index,
    uint32_t *count, romx_error_t *error)
{
    if (catalog == NULL || count == NULL ||
        candidate_index >= catalog->candidate_count)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid SAVE catalog file count arguments");
    *count = catalog->candidates[candidate_index].file_count;
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_save_catalog_get_file(
    const romx_save_catalog_t *catalog, uint32_t candidate_index,
    uint32_t file_index, romx_save_file_info_t *file, romx_error_t *error)
{
    const save_file_record_t *stored;
    uint32_t supplied_size;
    size_t path_size;
    if (catalog == NULL || file == NULL ||
        file->struct_size < (uint32_t)sizeof(*file) ||
        candidate_index >= catalog->candidate_count ||
        file_index >= catalog->candidates[candidate_index].file_count)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid SAVE catalog file arguments");
    supplied_size = file->struct_size;
    stored = &catalog->candidates[candidate_index].files[file_index];
    path_size = strlen(stored->path);
    *file = (romx_save_file_info_t)ROMX_SAVE_FILE_INFO_INIT;
    file->struct_size = supplied_size;
    file->index = file_index;
    file->data_size = stored->size;
    file->path_size = (uint32_t)path_size;
    memcpy(file->path, stored->path, path_size + 1U);
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_save_catalog_copy_candidate_source_path(
    const romx_save_catalog_t *catalog, uint32_t candidate_index,
    void *buffer, uint64_t capacity, uint64_t *required_size,
    romx_error_t *error)
{
    const char *path;
    size_t size;
    if (catalog == NULL || required_size == NULL ||
        (buffer == NULL && capacity != UINT64_C(0)) ||
        candidate_index >= catalog->candidate_count)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid SAVE source path arguments");
    path = catalog->candidates[candidate_index].source_path;
    size = strlen(path) + 1U;
    *required_size = (uint64_t)size;
    if (buffer == NULL || capacity < (uint64_t)size)
        return romx_error_set(error, ROMX_E_BUFFER_TOO_SMALL, 0,
            ROMX_OFFSET_UNKNOWN, "SAVE source path buffer is too small");
    memcpy(buffer, path, size);
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_save_catalog_write_candidate(
    const romx_save_catalog_t *catalog, uint32_t candidate_index,
    const char *utf8_romx_path, const char *object_key,
    const romx_mutable_bundle_options_t *bundle_options,
    const romx_mutable_write_options_t *write_options,
    romx_mutable_object_info_t *written_object, romx_error_t *error)
{
    const save_candidate_record_t *candidate;
    romx_mutable_bundle_path_entry_t *entries;
    char **relative_paths = NULL;
    const char *key;
    int wrap_psp;
    int canonical_extdata;
    uint32_t index;
    romx_result_t result;
    if (catalog == NULL || utf8_romx_path == NULL || utf8_romx_path[0] == '\0' ||
        candidate_index >= catalog->candidate_count)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid SAVE candidate write arguments");
    candidate = &catalog->candidates[candidate_index];
    if (candidate->file_count == 0U)
        return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "SAVE candidate contains no files");
    key = object_key != NULL && object_key[0] != '\0'
        ? object_key : candidate->key;
    if (!save_path_valid(key))
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "SAVE object key is not portable");
    entries = (romx_mutable_bundle_path_entry_t *)calloc(candidate->file_count,
        sizeof(*entries));
    if (entries == NULL)
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate SAVE bundle entries");
    wrap_psp = candidate->grouping == ROMX_SAVE_GROUP_MARKER_DIRECTORY;
    canonical_extdata = candidate->scope == ROMX_SAVE_SCOPE_3DS_EXTDATA &&
        candidate->source_format != ROMX_SAVE_SOURCE_3DS_SAVEDATAFILER;
    if (wrap_psp || canonical_extdata) {
        if (canonical_extdata && !save_is_hex_title_id(candidate->extdata_id)) {
            free(entries);
            return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
                ROMX_OFFSET_UNKNOWN,
                "3DS ExtData candidate has no valid 16-digit ExtData ID");
        }
        relative_paths = (char **)calloc(candidate->file_count,
            sizeof(*relative_paths));
        if (relative_paths == NULL) {
            free(entries);
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN,
                "failed to allocate SAVE bundle paths");
        }
    }
    for (index = 0U; index < candidate->file_count; ++index) {
        entries[index] = (romx_mutable_bundle_path_entry_t)
            ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT;
        if (relative_paths != NULL) {
            relative_paths[index] = wrap_psp
                ? save_join_path(save_basename(candidate->source_path),
                    candidate->files[index].path)
                : save_extdata_bundle_path(candidate, candidate->files[index].path);
            if (relative_paths[index] == NULL) {
                uint32_t cleanup_index;
                for (cleanup_index = 0U; cleanup_index <= index;
                     ++cleanup_index)
                    free(relative_paths[cleanup_index]);
                free(relative_paths);
                free(entries);
                return romx_error_set(error, ROMX_E_RANGE, 0,
                    ROMX_OFFSET_UNKNOWN,
                    "SAVE bundle path is invalid or too long");
            }
            entries[index].relative_path = relative_paths[index];
        } else {
            entries[index].relative_path = candidate->files[index].path;
        }
        entries[index].source_path = candidate->files[index].source_path;
    }
    result = romx_mutable_bundle_write_path_entries(utf8_romx_path,
        ROMX_MUTABLE_NAMESPACE_SAVE, key, entries, candidate->file_count,
        bundle_options, write_options, written_object, error);
    if (relative_paths != NULL) {
        for (index = 0U; index < candidate->file_count; ++index)
            free(relative_paths[index]);
        free(relative_paths);
    }
    free(entries);
    return result;
}

void romx_save_catalog_close(romx_save_catalog_t *catalog)
{
    if (catalog == NULL) return;
    save_catalog_destroy_records(catalog);
    free(catalog);
}
