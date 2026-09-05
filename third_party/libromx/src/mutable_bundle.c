#if !defined(_WIN32)
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "romx_internal.h"
#include "save_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <wchar.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define BUNDLE_HEADER_SIZE UINT64_C(64)
#define BUNDLE_ENTRY_SIZE UINT64_C(64)

static int allocation_size_overflows(size_t count, size_t element_size)
{
    return element_size != 0U && count > SIZE_MAX / element_size;
}

#if defined(_WIN32)
static wchar_t *path_to_wide(const char *path)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path, -1, NULL, 0);
    wchar_t *wide;
    if (count <= 0) return NULL;
    wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            path, -1, wide, count) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static FILE *file_open_read(const char *path)
{
    wchar_t *wide = path_to_wide(path);
    FILE *file = NULL;
    if (wide != NULL) (void)_wfopen_s(&file, wide, L"rb");
    free(wide);
    return file;
}
#else
static FILE *file_open_read(const char *path)
{
    return fopen(path, "rb");
}
#endif

typedef struct bundle_source_entry {
    char *path;
    FILE *source;
    uint64_t size;
    uint64_t data_offset;
    uint32_t crc32;
} bundle_source_entry_t;

typedef struct bundle_source {
    uint8_t *prefix;
    uint64_t prefix_size;
    uint64_t bundle_size;
    bundle_source_entry_t *entries;
    uint32_t entry_count;
} bundle_source_t;

typedef struct bundle_entry {
    char *path;
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t data_crc32;
} bundle_entry_t;

typedef struct bundle_save_slot {
    char *key;
    char *display_name;
    uint32_t *entry_indices;
    uint32_t entry_count;
    uint32_t entry_capacity;
    uint64_t data_size;
    int is_directory;
} bundle_save_slot_t;

struct romx_mutable_bundle {
    romx_mutable_file_t *file;
    char *object_key;
    bundle_entry_t *entries;
    uint32_t entry_count;
    uint32_t io_chunk_size;
    romx_mutable_namespace_t object_namespace;
    romx_save_profile_info_t profile;
    romx_mutable_save_layout_info_t save_layout;
    bundle_save_slot_t *save_slots;
    uint32_t save_slot_count;
};

static uint64_t align64(uint64_t value)
{
    if (value > UINT64_MAX - UINT64_C(63)) return UINT64_MAX;
    return (value + UINT64_C(63)) & ~UINT64_C(63);
}

static uint32_t header_crc(uint8_t header[64])
{
    uint8_t saved[4];
    uint32_t crc;
    memcpy(saved, header + 0x38U, sizeof(saved));
    memset(header + 0x38U, 0, sizeof(saved));
    crc = romx_crc32_begin();
    crc = romx_crc32_update(crc, header, 64U);
    crc = romx_crc32_finish(crc);
    memcpy(header + 0x38U, saved, sizeof(saved));
    return crc;
}

static int path_compare_bytes(const char *left, const char *right)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    while (*a != 0U && *b != 0U) {
        if (*a != *b) return *a < *b ? -1 : 1;
        ++a;
        ++b;
    }
    return *a == *b ? 0 : (*a == 0U ? -1 : 1);
}

static int source_compare(const void *left, const void *right)
{
    const bundle_source_entry_t *a = (const bundle_source_entry_t *)left;
    const bundle_source_entry_t *b = (const bundle_source_entry_t *)right;
    return path_compare_bytes(a->path, b->path);
}

static int folded_path_pointer_compare(const void *left, const void *right)
{
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return romx_ascii_fold_compare(*a, *b);
}

static char *duplicate_path(const char *path)
{
    const size_t size = strlen(path);
    char *copy = (char *)malloc(size + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, path, size + 1U);
    return copy;
}

static void destroy_save_slot_array(bundle_save_slot_t *slots,
    uint32_t count)
{
    uint32_t index;
    if (slots == NULL) return;
    for (index = 0U; index < count; ++index) {
        free(slots[index].key);
        free(slots[index].display_name);
        free(slots[index].entry_indices);
    }
    free(slots);
}

static int ascii_name_equal(const char *left, size_t left_size,
    const char *right)
{
    size_t index;
    if (strlen(right) != left_size) return 0;
    for (index = 0U; index < left_size; ++index) {
        unsigned char a = (unsigned char)left[index];
        unsigned char b = (unsigned char)right[index];
        if (a >= 'a' && a <= 'z') a = (unsigned char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (unsigned char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

/* ROMX 0.2.0 PSP profile accepts the on-disk PARAM.SFO v1.01 table and the
 * three parameter encodings used by retail savedata.  We validate the entire
 * bounded table before looking up an identity key, so a malformed unrelated
 * record cannot be mistaken for a valid save directory. */
static int sfo_validate(const uint8_t *bytes, size_t size)
{
    uint32_t key_table, data_table, count;
    uint64_t records_end;
    uint32_t index;

    if (bytes == NULL || size < 20U ||
        bytes[0] != 0U || bytes[1] != 'P' || bytes[2] != 'S' ||
        bytes[3] != 'F' || romx_read_le32(bytes + 4U) != UINT32_C(0x00000101))
        return 0;
    key_table = romx_read_le32(bytes + 8U);
    data_table = romx_read_le32(bytes + 12U);
    count = romx_read_le32(bytes + 16U);
    if (count == 0U || count > UINT32_C(4096)) return 0;
    records_end = UINT64_C(20) + (uint64_t)count * UINT64_C(16);
    if (records_end > (uint64_t)size || key_table < records_end ||
        key_table > data_table || data_table > (uint64_t)size)
        return 0;
    for (index = 0U; index < count; ++index) {
        const uint8_t *record = bytes + 20U + (size_t)index * 16U;
        uint16_t format = romx_read_le16(record + 2U);
        uint32_t data_length = romx_read_le32(record + 4U);
        uint32_t data_maximum = romx_read_le32(record + 8U);
        uint32_t data_offset = romx_read_le32(record + 12U);
        uint64_t key_position = (uint64_t)key_table +
            (uint64_t)romx_read_le16(record);
        uint64_t data_position = (uint64_t)data_table + data_offset;
        uint64_t key_limit = (uint64_t)data_table;
        uint64_t key_end;
        uint32_t other;

        if (key_position >= key_limit || data_position > (uint64_t)size ||
            data_length > (uint64_t)size - data_position ||
            data_maximum < data_length ||
            (format != UINT16_C(0x0004) && format != UINT16_C(0x0204) &&
             format != UINT16_C(0x0404))) return 0;
        key_end = key_position;
        while (key_end < key_limit && bytes[key_end] != 0U) ++key_end;
        if (key_end >= key_limit || key_end == key_position) return 0;
        for (other = 0U; other < index; ++other) {
            const uint8_t *other_record = bytes + 20U +
                (size_t)other * 16U;
            uint64_t other_key_position = (uint64_t)key_table +
                (uint64_t)romx_read_le16(other_record);
            uint64_t other_key_end = other_key_position;
            while (other_key_end < key_limit &&
                   bytes[other_key_end] != 0U) ++other_key_end;
            if (other_key_end >= key_limit ||
                other_key_end - other_key_position != key_end - key_position)
                return 0;
            {
                uint64_t key_cursor = key_position;
                uint64_t other_cursor = other_key_position;
                while (key_cursor < key_end) {
                    unsigned char current = bytes[key_cursor++];
                    unsigned char previous = bytes[other_cursor++];
                    if (current >= (unsigned char)'a' &&
                        current <= (unsigned char)'z')
                        current = (unsigned char)(current - 'a' + 'A');
                    if (previous >= (unsigned char)'a' &&
                        previous <= (unsigned char)'z')
                        previous = (unsigned char)(previous - 'a' + 'A');
                    if (current != previous) break;
                    if (key_cursor == key_end) return 0;
                }
            }
        }
        if (format == UINT16_C(0x0204)) {
            uint64_t value_end = data_position;
            while (value_end < data_position + data_length &&
                   bytes[value_end] != 0U) ++value_end;
            if (data_length == 0U || value_end >= data_position + data_length)
                return 0;
        }
    }
    return 1;
}

static int sfo_get_string(const uint8_t *bytes, size_t size,
    const char *wanted, char *value, size_t capacity)
{
    uint32_t key_table, data_table, count, index;
    /* The public inspector validates the complete table once before lookup. */
    if (value == NULL || capacity == 0U || wanted == NULL) return 0;
    key_table = romx_read_le32(bytes + 8U);
    data_table = romx_read_le32(bytes + 12U);
    count = romx_read_le32(bytes + 16U);
    for (index = 0U; index < count; ++index) {
        const uint8_t *record = bytes + 20U + (size_t)index * 16U;
        uint16_t key_offset = romx_read_le16(record);
        uint32_t data_length = romx_read_le32(record + 4U);
        uint32_t data_offset = romx_read_le32(record + 12U);
        uint64_t key_position = (uint64_t)key_table + key_offset;
        uint64_t data_position = (uint64_t)data_table + data_offset;
        size_t key_length = 0U, value_length = 0U;
        if (key_position >= size || data_position > size ||
            data_length > size - data_position ||
            romx_read_le16(record + 2U) != UINT16_C(0x0204)) continue;
        while (key_position + key_length < size &&
               bytes[key_position + key_length] != 0U) ++key_length;
        if (key_position + key_length >= size ||
            !ascii_name_equal((const char *)bytes + key_position,
                key_length, wanted)) continue;
        while (value_length < data_length &&
               bytes[data_position + value_length] != 0U) ++value_length;
        if (value_length == 0U || value_length >= capacity) return 0;
        memcpy(value, bytes + data_position, value_length);
        value[value_length] = '\0';
        return 1;
    }
    return 0;
}

static int psp_identity_valid(const char *value, int require_digit)
{
    size_t index, size;
    int has_digit = 0;
    if (value == NULL || (size = strlen(value)) < 4U || size > 64U)
        return 0;
    for (index = 0U; index < size; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte >= '0' && byte <= '9') has_digit = 1;
        else if (!((byte >= 'A' && byte <= 'Z') ||
                   (byte >= 'a' && byte <= 'z') || byte == '-' ||
                   byte == '_')) return 0;
    }
    return !require_digit || has_digit;
}

static int normalized_identity_equal(const char *left, const char *right)
{
    size_t a = 0U, b = 0U;
    for (;;) {
        unsigned char x, y;
        while (left[a] == '-' || left[a] == '_') ++a;
        while (right[b] == '-' || right[b] == '_') ++b;
        x = (unsigned char)left[a++];
        y = (unsigned char)right[b++];
        if (x >= 'a' && x <= 'z') x = (unsigned char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (unsigned char)(y - 'a' + 'A');
        if (x != y) return 0;
        if (x == 0U) return 1;
    }
}

romx_result_t romx_mutable_psp_savedata_inspect_sfo(const void *sfo_bytes,
    uint64_t sfo_size, const char *expected_directory_basename,
    romx_mutable_psp_savedata_info_t *info, romx_error_t *error)
{
    const uint8_t *bytes = (const uint8_t *)sfo_bytes;
    romx_mutable_psp_savedata_info_t parsed =
        ROMX_MUTABLE_PSP_SAVEDATA_INFO_INIT;
    size_t bad_offset = 0U;
    int has_disc_id;
    int has_savedata_directory;
    if (bytes == NULL || info == NULL ||
        info->struct_size < (uint32_t)sizeof(*info) ||
        sfo_size == 0U || sfo_size > UINT64_C(4194304) ||
        sfo_size > (uint64_t)SIZE_MAX)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid PSP PARAM.SFO arguments");
    if (!sfo_validate(bytes, (size_t)sfo_size))
        return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "PSP PARAM.SFO table is invalid");
    has_disc_id = sfo_get_string(bytes, (size_t)sfo_size, "DISC_ID",
        parsed.disc_id, sizeof(parsed.disc_id)) &&
        psp_identity_valid(parsed.disc_id, 1);
    has_savedata_directory = sfo_get_string(bytes, (size_t)sfo_size,
        "SAVEDATA_DIRECTORY", parsed.savedata_directory,
        sizeof(parsed.savedata_directory)) &&
        psp_identity_valid(parsed.savedata_directory, 0);
    if (has_savedata_directory && expected_directory_basename != NULL &&
        (!*expected_directory_basename ||
         !normalized_identity_equal(parsed.savedata_directory,
            expected_directory_basename)))
        has_savedata_directory = 0;
    if (!has_disc_id) parsed.disc_id[0] = '\0';
    if (!has_savedata_directory) parsed.savedata_directory[0] = '\0';
    if (!has_disc_id && !has_savedata_directory)
        return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN,
            "PSP PARAM.SFO has no valid savedata identity");
    if (has_disc_id)
        parsed.flags |= ROMX_MUTABLE_PSP_SAVEDATA_HAS_DISC_ID;
    if (has_savedata_directory)
        parsed.flags |= ROMX_MUTABLE_PSP_SAVEDATA_HAS_DIRECTORY;
    if (!sfo_get_string(bytes, (size_t)sfo_size, "SAVEDATA_TITLE",
            parsed.title, sizeof(parsed.title)))
        (void)sfo_get_string(bytes, (size_t)sfo_size, "TITLE",
            parsed.title, sizeof(parsed.title));
    if (parsed.title[0] != '\0' &&
        romx_utf8_validate((const uint8_t *)parsed.title,
            strlen(parsed.title), &bad_offset))
        parsed.flags |= ROMX_MUTABLE_PSP_SAVEDATA_HAS_TITLE;
    else
        parsed.title[0] = '\0';
    *info = parsed;
    romx_error_clear(error);
    return ROMX_OK;
}

static int path_is_member(const char *path, const char *root)
{
    size_t size = strlen(root);
    return strncmp(path, root, size) == 0 &&
        (path[size] == '/' || path[size] == '\0');
}

static const char *path_basename_pointer(const char *path)
{
    const char *separator = strrchr(path, '/');
    return separator == NULL ? path : separator + 1U;
}

static char *path_parent_copy(const char *path)
{
    const char *separator = strrchr(path, '/');
    size_t size;
    char *copy;
    if (separator == NULL || separator == path) return NULL;
    size = (size_t)(separator - path);
    copy = (char *)malloc(size + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, path, size);
    copy[size] = '\0';
    return copy;
}

static int path_component_copy(const char *path, uint32_t wanted,
    char *output, size_t capacity)
{
    const char *start = path;
    uint32_t component = 0U;
    const char *cursor;
    if (path == NULL || output == NULL || capacity == 0U) return 0;
    for (cursor = path;; ++cursor) {
        if (*cursor == '/' || *cursor == '\0') {
            size_t size;
            if (component != wanted) {
                if (*cursor == '\0') break;
                start = cursor + 1U;
                ++component;
                continue;
            }
            size = (size_t)(cursor - start);
            if (size == 0U || size >= capacity) return 0;
            memcpy(output, start, size);
            output[size] = '\0';
            return 1;
        }
    }
    return 0;
}

/* SaveDataFiler's portable interchange form deliberately uses the outer
 * directory only as a user label.  The real ExtData identity is the single
 * eight-digit directory and its two matching sidecars. */
static int mutable_bundle_strict_extdata_low(
    const romx_mutable_bundle_t *bundle, char low[9])
{
    romx_savedatafiler_shape_t shape = { { 0 }, 0U };
    uint32_t index;
    for (index = 0U; index < bundle->entry_count; ++index) {
        const char *path = bundle->entries[index].path;
        char component[14];
        if (!path_component_copy(path, 0U, component, sizeof(component)) ||
            !romx_savedatafiler_add(&shape, component,
                strchr(path, '/') != NULL)) return 0;
    }
    return romx_savedatafiler_finish(&shape, low);
}

static int mutable_bundle_canonical_extdata_id(
    const romx_mutable_bundle_t *bundle, char id[17])
{
    char high[9] = { 0 };
    char low[9] = { 0 };
    uint32_t index;
    if (bundle == NULL || id == NULL || bundle->entry_count == 0U)
        return 0;
    for (index = 0U; index < bundle->entry_count; ++index) {
        char prefix[9];
        char current_high[9];
        char current_low[9];
        if (!path_component_copy(bundle->entries[index].path, 0U,
                prefix, sizeof(prefix)) ||
            !path_component_copy(bundle->entries[index].path, 1U,
                current_high, sizeof(current_high)) ||
            !romx_ascii_fold_equal(prefix, "extdata") ||
            !romx_hex_string(current_high, 8U) ||
            !path_component_copy(bundle->entries[index].path, 2U,
                current_low, sizeof(current_low)) ||
            !romx_hex_string(current_low, 8U)) {
            return 0;
        }
        if (high[0] == '\0') {
            romx_copy_hex_upper(current_high, 8U, high);
            romx_copy_hex_upper(current_low, 8U, low);
        } else if (!romx_ascii_fold_equal(high, current_high) ||
                   !romx_ascii_fold_equal(low, current_low)) {
            return 0;
        }
    }
    memcpy(id, high, 8U);
    memcpy(id + 8U, low, 8U);
    id[16] = '\0';
    return 1;
}

static romx_result_t mutable_bundle_analyze_save_layout(
    const romx_mutable_bundle_t *bundle,
    romx_mutable_save_layout_info_t *layout, romx_error_t *error)
{
    char low[9];
    char canonical[17];
    if (bundle == NULL || layout == NULL)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable SAVE layout arguments");
    *layout = (romx_mutable_save_layout_info_t)
        ROMX_MUTABLE_SAVE_LAYOUT_INFO_INIT;
    layout->entry_count = bundle->entry_count;
    if (bundle->object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "SAVE layout requested for non-SAVE bundle");
    if (bundle->profile.platform_id != ROMX_PLATFORM_NINTENDO_3DS ||
        bundle->entry_count == 0U) return ROMX_OK;
    if (mutable_bundle_strict_extdata_low(bundle, low)) {
        memcpy(layout->extdata_id, "00000000", 8U);
        memcpy(layout->extdata_id + 8U, low, 8U);
        layout->extdata_id[16] = '\0';
        layout->extdata_id_size = 16U;
        layout->flags = ROMX_MUTABLE_SAVE_LAYOUT_HAS_EXTDATA_ID |
            ROMX_MUTABLE_SAVE_LAYOUT_STRICT_EXTDATA;
        layout->scope = ROMX_SAVE_SCOPE_3DS_EXTDATA;
    } else if (mutable_bundle_canonical_extdata_id(bundle, canonical)) {
        memcpy(layout->extdata_id, canonical, sizeof(layout->extdata_id));
        layout->extdata_id_size = 16U;
        layout->flags = ROMX_MUTABLE_SAVE_LAYOUT_HAS_EXTDATA_ID;
        layout->scope = ROMX_SAVE_SCOPE_3DS_EXTDATA;
    } else {
        layout->scope = ROMX_SAVE_SCOPE_3DS_TITLE;
    }
    return ROMX_OK;
}

static romx_result_t bundle_read_exact(romx_mutable_file_t *file,
    uint64_t offset, void *buffer, uint64_t size, romx_error_t *error);

static romx_result_t add_save_slot(romx_mutable_bundle_t *bundle,
    const char *key, const char *display_name, int is_directory,
    romx_error_t *error)
{
    bundle_save_slot_t *grown;
    bundle_save_slot_t *slot;
    if (bundle->save_slot_count == UINT32_MAX ||
        (uintmax_t)bundle->save_slot_count + 1U >
            (uintmax_t)(SIZE_MAX / sizeof(*grown)))
        return romx_error_set(error, ROMX_E_RANGE, 0,
            ROMX_OFFSET_UNKNOWN, "SAVE slot count overflows");
    grown = (bundle_save_slot_t *)realloc(bundle->save_slots,
        ((size_t)bundle->save_slot_count + 1U) * sizeof(*grown));
    if (grown == NULL)
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to grow SAVE slots");
    bundle->save_slots = grown;
    slot = &grown[bundle->save_slot_count];
    memset(slot, 0, sizeof(*slot));
    slot->key = duplicate_path(key);
    slot->display_name = duplicate_path(
        display_name && *display_name ? display_name : path_basename_pointer(key));
    if (slot->key == NULL || slot->display_name == NULL) {
        free(slot->key);
        free(slot->display_name);
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate SAVE slot");
    }
    slot->is_directory = is_directory;
    ++bundle->save_slot_count;
    return ROMX_OK;
}

static romx_result_t append_save_slot_entry(romx_mutable_bundle_t *bundle,
    uint32_t slot_index, uint32_t entry_index, romx_error_t *error)
{
    bundle_save_slot_t *slot = &bundle->save_slots[slot_index];
    if (slot->entry_count == UINT32_MAX ||
        slot->data_size > UINT64_MAX - bundle->entries[entry_index].data_size)
        return romx_error_set(error, ROMX_E_RANGE, 0,
            ROMX_OFFSET_UNKNOWN, "SAVE slot entry data size overflows");
    if (slot->entry_count == slot->entry_capacity) {
        uint32_t capacity = slot->entry_capacity == 0U ? 1U :
            (slot->entry_capacity > bundle->entry_count / 2U
                ? bundle->entry_count : slot->entry_capacity * 2U);
        uint32_t *indices;
        if (allocation_size_overflows((size_t)capacity, sizeof(*indices)))
            return romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "SAVE slot index size overflows");
        indices = (uint32_t *)realloc(slot->entry_indices,
            (size_t)capacity * sizeof(*indices));
        if (indices == NULL)
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN, "failed to grow SAVE slot entries");
        slot->entry_indices = indices;
        slot->entry_capacity = capacity;
    }
    slot->entry_indices[slot->entry_count++] = entry_index;
    slot->data_size += bundle->entries[entry_index].data_size;
    return ROMX_OK;
}

static romx_result_t build_single_file_save_slots(
    romx_mutable_bundle_t *bundle, romx_error_t *error)
{
    uint32_t index;
    for (index = 0U; index < bundle->entry_count; ++index) {
        romx_result_t result = add_save_slot(bundle,
            bundle->entries[index].path,
            path_basename_pointer(bundle->entries[index].path), 0, error);
        if (result != ROMX_OK) return result;
        result = append_save_slot_entry(bundle,
            bundle->save_slot_count - 1U, index, error);
        if (result != ROMX_OK) return result;
    }
    return ROMX_OK;
}

static romx_result_t build_strict_extdata_save_slots(
    romx_mutable_bundle_t *bundle, romx_error_t *error)
{
    uint32_t index;
    romx_result_t result = add_save_slot(bundle, bundle->object_key,
        bundle->object_key, 1, error);
    if (result != ROMX_OK) return result;
    for (index = 0U; index < bundle->entry_count; ++index) {
        result = append_save_slot_entry(bundle,
            bundle->save_slot_count - 1U, index, error);
        if (result != ROMX_OK) return result;
    }
    return ROMX_OK;
}

static romx_result_t count_3ds_directory_roots(
    const romx_mutable_bundle_t *bundle, uint32_t *directory_root_count,
    int *has_root_file, romx_error_t *error)
{
    uint32_t index;
    *directory_root_count = UINT32_C(0);
    *has_root_file = 0;
    for (index = 0U; index < bundle->entry_count; ++index) {
        char root[ROMX_MUTABLE_BUNDLE_PATH_CAPACITY + 1U];
        uint32_t previous;
        int seen = 0;
        if (strchr(bundle->entries[index].path, '/') == NULL) {
            *has_root_file = 1;
            continue;
        }
        if (!path_component_copy(bundle->entries[index].path, 0U,
                root, sizeof(root))) {
            return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
                ROMX_OFFSET_UNKNOWN,
                "3DS SAVE entry has no valid top-level directory");
        }
        for (previous = 0U; previous < index; ++previous) {
            char previous_root[ROMX_MUTABLE_BUNDLE_PATH_CAPACITY + 1U];
            if (strchr(bundle->entries[previous].path, '/') == NULL ||
                !path_component_copy(bundle->entries[previous].path, 0U,
                    previous_root, sizeof(previous_root))) continue;
            if (romx_ascii_fold_equal(root, previous_root)) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            if (*directory_root_count == UINT32_MAX) {
                return romx_error_set(error, ROMX_E_RANGE, 0,
                    ROMX_OFFSET_UNKNOWN,
                    "3DS SAVE directory root count overflows");
            }
            ++*directory_root_count;
        }
    }
    return ROMX_OK;
}

static romx_result_t build_single_object_save_slot(
    romx_mutable_bundle_t *bundle, romx_error_t *error)
{
    uint32_t index;
    const int is_directory = bundle->entry_count > UINT32_C(1) ||
        (bundle->entry_count != UINT32_C(0) &&
            strchr(bundle->entries[0].path, '/') != NULL);
    romx_result_t result = add_save_slot(bundle, bundle->object_key,
        bundle->object_key, is_directory, error);
    if (result != ROMX_OK) return result;
    for (index = 0U; index < bundle->entry_count; ++index) {
        result = append_save_slot_entry(bundle,
            bundle->save_slot_count - 1U, index, error);
        if (result != ROMX_OK) return result;
    }
    return ROMX_OK;
}

/* A 3DS mutable SAVE object is normally one logical save.  The split by
 * top-level directory is retained only for old containers that clearly carry
 * several directory roots and no root-level files. */
static romx_result_t build_directory_save_slots(
    romx_mutable_bundle_t *bundle, romx_error_t *error)
{
    uint32_t directory_root_count;
    int has_root_file;
    uint32_t index;
    romx_result_t result = count_3ds_directory_roots(bundle,
        &directory_root_count, &has_root_file, error);
    if (result != ROMX_OK) return result;
    if (bundle->entry_count == UINT32_C(0)) return ROMX_OK;
    if (has_root_file || directory_root_count <= UINT32_C(1))
        return build_single_object_save_slot(bundle, error);
    for (index = 0U; index < bundle->entry_count; ++index) {
        const char *path = bundle->entries[index].path;
        const char *separator = strchr(path, '/');
        char root[ROMX_MUTABLE_BUNDLE_PATH_CAPACITY + 1U];
        size_t size = separator == NULL ? strlen(path) :
            (size_t)(separator - path);
        uint32_t slot_index;
        memcpy(root, path, size);
        root[size] = '\0';
        for (slot_index = 0U; slot_index < bundle->save_slot_count; ++slot_index)
            if (romx_ascii_fold_equal(bundle->save_slots[slot_index].key, root))
                break;
        if (slot_index == bundle->save_slot_count) {
            result = add_save_slot(bundle, root, root, separator != NULL, error);
            if (result != ROMX_OK) return result;
        }
        result = append_save_slot_entry(bundle, slot_index, index, error);
        if (result != ROMX_OK) return result;
    }
    return ROMX_OK;
}

static romx_result_t build_psp_save_slots(romx_mutable_bundle_t *bundle,
    romx_error_t *error)
{
    const uint64_t maximum_sfo_size = UINT64_C(4194304);
    uint32_t index;

    for (index = 0U; index < bundle->entry_count; ++index) {
        const bundle_entry_t *entry = &bundle->entries[index];
        const char *name = path_basename_pointer(entry->path);
        char *root;
        uint8_t *sfo;
        romx_mutable_psp_savedata_info_t savedata =
            ROMX_MUTABLE_PSP_SAVEDATA_INFO_INIT;
        const char *root_name;
        romx_result_t result;
        uint32_t existing;

        if (!ascii_name_equal(name, strlen(name), "PARAM.SFO") ||
            entry->data_size == 0U || entry->data_size > maximum_sfo_size)
            continue;
        root = path_parent_copy(entry->path);
        if (root == NULL) continue;
        sfo = (uint8_t *)malloc((size_t)entry->data_size);
        if (sfo == NULL) {
            free(root);
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                entry->data_offset, "failed to allocate PSP PARAM.SFO");
        }
        result = bundle_read_exact(bundle->file, entry->data_offset, sfo,
            entry->data_size, error);
        if (result != ROMX_OK) {
            free(sfo);
            free(root);
            return result;
        }
        root_name = path_basename_pointer(root);
        result = romx_mutable_psp_savedata_inspect_sfo(sfo,
            entry->data_size, root_name, &savedata, error);
        if (result != ROMX_OK) {
            free(sfo);
            free(root);
            continue;
        }
        free(sfo);
        for (existing = 0U; existing < bundle->save_slot_count; ++existing)
            if (romx_ascii_fold_equal(bundle->save_slots[existing].key, root)) break;
        if (existing == bundle->save_slot_count) {
            result = add_save_slot(bundle, root,
                (savedata.flags & ROMX_MUTABLE_PSP_SAVEDATA_HAS_TITLE)
                    ? savedata.title : root_name, 1, error);
            if (result != ROMX_OK) {
                free(root);
                return result;
            }
        }
        free(root);
    }

    for (index = 0U; index < bundle->entry_count; ++index) {
        uint32_t slot_index, selected = UINT32_MAX;
        size_t selected_size = 0U;
        for (slot_index = 0U; slot_index < bundle->save_slot_count;
             ++slot_index) {
            const char *root = bundle->save_slots[slot_index].key;
            size_t root_size = strlen(root);
            if (root_size > selected_size &&
                path_is_member(bundle->entries[index].path, root)) {
                selected = slot_index;
                selected_size = root_size;
            }
        }
        if (selected != UINT32_MAX) {
            romx_result_t result = append_save_slot_entry(bundle, selected,
                index, error);
            if (result != ROMX_OK) return result;
        }
    }
    return ROMX_OK;
}

static romx_result_t build_save_slots(romx_mutable_bundle_t *bundle,
    romx_error_t *error)
{
    if (bundle->save_layout.flags & ROMX_MUTABLE_SAVE_LAYOUT_STRICT_EXTDATA)
        return build_strict_extdata_save_slots(bundle, error);
    if (bundle->profile.grouping == ROMX_SAVE_GROUP_MARKER_DIRECTORY)
        return build_psp_save_slots(bundle, error);
    if (bundle->profile.grouping == ROMX_SAVE_GROUP_DIRECTORY_PER_SAVE)
        return build_directory_save_slots(bundle, error);
    return build_single_file_save_slots(bundle, error);
}

/* Returns 1 when unique, 0 for a collision, and -1 on allocation failure. */
static int paths_portable_unique(char *const *paths, uint32_t count)
{
    const char **sorted;
    uint32_t index;
    int unique = 1;
    if (count < UINT32_C(2)) return 1;
    sorted = (const char **)malloc((size_t)count * sizeof(*sorted));
    if (sorted == NULL) return -1;
    for (index = 0U; index < count; ++index) sorted[index] = paths[index];
    qsort(sorted, count, sizeof(*sorted), folded_path_pointer_compare);
    for (index = 1U; index < count; ++index) {
        if (romx_ascii_fold_equal(sorted[index - 1U], sorted[index])) {
            unique = 0;
            break;
        }
    }
    free(sorted);
    return unique;
}

static int effective_options(const romx_mutable_bundle_options_t *provided,
    romx_mutable_bundle_options_t *options)
{
    *options = (romx_mutable_bundle_options_t)ROMX_MUTABLE_BUNDLE_OPTIONS_INIT;
    if (provided != NULL) {
        if (provided->struct_size < (uint32_t)sizeof(*provided)) return 0;
        *options = *provided;
    }
    if (options->max_entry_count == UINT32_C(0))
        options->max_entry_count = ROMX_MUTABLE_BUNDLE_DEFAULT_MAX_ENTRIES;
    if (options->max_path_size == UINT32_C(0))
        options->max_path_size = ROMX_MUTABLE_BUNDLE_PATH_CAPACITY;
    if (options->max_bundle_size == UINT64_C(0))
        options->max_bundle_size = ROMX_MUTABLE_BUNDLE_DEFAULT_MAX_SIZE;
    if (options->io_chunk_size == UINT32_C(0))
        options->io_chunk_size = ROMX_DEFAULT_IO_CHUNK_SIZE;
    return options->flags == 0U && options->reserved == 0U &&
        options->max_path_size <= ROMX_MUTABLE_BUNDLE_PATH_CAPACITY &&
        options->max_bundle_size >= BUNDLE_HEADER_SIZE;
}

static int file_seek(FILE *file, uint64_t offset)
{
#if defined(_WIN32)
    if (offset > (uint64_t)INT64_MAX) return 0;
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static int file_get_size(FILE *file, uint64_t *size)
{
#if defined(_WIN32)
    __int64 value;
    if (_fseeki64(file, 0, SEEK_END) != 0 ||
        (value = _ftelli64(file)) < 0 || _fseeki64(file, 0, SEEK_SET) != 0)
        return 0;
#else
    off_t value;
    if (fseeko(file, 0, SEEK_END) != 0 ||
        (value = ftello(file)) < 0 || fseeko(file, 0, SEEK_SET) != 0)
        return 0;
#endif
    *size = (uint64_t)value;
    return 1;
}

static int source_path_is_regular(const char *path)
{
#if defined(_WIN32)
    wchar_t *wide = path_to_wide(path);
    DWORD attributes;
    if (wide == NULL) return 0;
    attributes = GetFileAttributesW(wide);
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY |
            FILE_ATTRIBUTE_REPARSE_POINT)) == 0U;
#else
    struct stat status;
    if (lstat(path, &status) != 0 || !S_ISREG(status.st_mode))
        return 0;
    return 1;
#endif
}

static romx_result_t file_crc(FILE *file, uint64_t size, uint32_t chunk_size,
    uint32_t *finished, romx_error_t *error)
{
    uint8_t *buffer = (uint8_t *)malloc(chunk_size);
    uint64_t position = UINT64_C(0);
    uint32_t crc = romx_crc32_begin();
    if (buffer == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        ROMX_OFFSET_UNKNOWN, "failed to allocate bundle CRC buffer");
    if (!file_seek(file, UINT64_C(0))) {
        free(buffer);
        return romx_error_set(error, ROMX_E_IO, errno, ROMX_OFFSET_UNKNOWN,
            "failed to seek mutable bundle source");
    }
    while (position < size) {
        size_t wanted = (size - position) > chunk_size
            ? (size_t)chunk_size : (size_t)(size - position);
        size_t actual = fread(buffer, 1U, wanted, file);
        if (actual != wanted) {
            free(buffer);
            return romx_error_set(error, ROMX_E_IO, errno,
                ROMX_OFFSET_UNKNOWN, "failed to read mutable bundle source");
        }
        crc = romx_crc32_update(crc, buffer, actual);
        position += (uint64_t)actual;
    }
    free(buffer);
    *finished = romx_crc32_finish(crc);
    return ROMX_OK;
}

static void source_destroy(bundle_source_t *source)
{
    uint32_t index;
    if (source == NULL) return;
    for (index = 0U; index < source->entry_count; ++index) {
        if (source->entries[index].source != NULL)
            fclose(source->entries[index].source);
        free(source->entries[index].path);
    }
    free(source->entries);
    free(source->prefix);
    memset(source, 0, sizeof(*source));
}

static romx_result_t source_get_size(void *user_data, uint64_t *size,
    romx_error_t *error)
{
    bundle_source_t *source = (bundle_source_t *)user_data;
    (void)error;
    *size = source->bundle_size;
    return ROMX_OK;
}

static romx_result_t source_read_at(void *user_data, uint64_t offset,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    bundle_source_t *source = (bundle_source_t *)user_data;
    uint8_t *output = (uint8_t *)buffer;
    uint64_t count;
    uint32_t index;
    if (offset > source->bundle_size)
        return romx_error_set(error, ROMX_E_RANGE, 0, offset,
            "mutable bundle read offset is out of range");
    count = source->bundle_size - offset;
    if (count > size) count = size;
    if (count > (uint64_t)SIZE_MAX)
        return romx_error_set(error, ROMX_E_RANGE, 0, offset,
            "mutable bundle read exceeds address space");
    memset(output, 0, (size_t)count);
    if (offset < source->prefix_size) {
        uint64_t prefix_count = source->prefix_size - offset;
        if (prefix_count > count) prefix_count = count;
        memcpy(output, source->prefix + (size_t)offset, (size_t)prefix_count);
    }
    for (index = 0U; index < source->entry_count; ++index) {
        bundle_source_entry_t *entry = &source->entries[index];
        uint64_t request_end = offset + count;
        uint64_t entry_end = entry->data_offset + entry->size;
        uint64_t start;
        uint64_t end;
        size_t wanted;
        if (request_end <= entry->data_offset || offset >= entry_end) continue;
        start = offset > entry->data_offset ? offset : entry->data_offset;
        end = request_end < entry_end ? request_end : entry_end;
        wanted = (size_t)(end - start);
        if (!file_seek(entry->source, start - entry->data_offset) ||
            fread(output + (size_t)(start - offset), 1U, wanted,
                entry->source) != wanted) {
            return romx_error_set(error, ROMX_E_IO, errno, start,
                "failed to read mutable bundle source");
        }
    }
    *bytes_read = count;
    romx_error_clear(error);
    return ROMX_OK;
}

static romx_result_t build_source(
    const romx_mutable_bundle_path_entry_t *provided, uint32_t entry_count,
    romx_mutable_namespace_t object_namespace,
    const romx_mutable_bundle_options_t *options, bundle_source_t *source,
    romx_error_t *error)
{
    uint64_t path_offset;
    uint64_t data_cursor;
    uint32_t index;
    memset(source, 0, sizeof(*source));
    source->entry_count = entry_count;
    if (entry_count != UINT32_C(0)) {
        source->entries = (bundle_source_entry_t *)calloc(entry_count,
            sizeof(*source->entries));
        if (source->entries == NULL)
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN, "failed to allocate mutable bundle entries");
    }
    for (index = 0U; index < entry_count; ++index) {
        const romx_mutable_bundle_path_entry_t *input = &provided[index];
        bundle_source_entry_t *entry = &source->entries[index];
        size_t path_size;
        romx_result_t result;
        if (input->struct_size < (uint32_t)sizeof(*input) ||
            input->reserved != UINT32_C(0) ||
            !romx_path_valid(input->relative_path, options->max_path_size) ||
            input->source_path == NULL) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
                ROMX_OFFSET_UNKNOWN, "invalid mutable bundle path entry");
        }
        path_size = strlen(input->relative_path);
        entry->path = (char *)malloc(path_size + 1U);
        if (entry->path == NULL) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN, "failed to copy mutable bundle path");
        }
        memcpy(entry->path, input->relative_path, path_size + 1U);
        if (!source_path_is_regular(input->source_path)) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
                ROMX_OFFSET_UNKNOWN,
                "mutable bundle source is not a regular file");
        }
        entry->source = file_open_read(input->source_path);
        if (entry->source == NULL || !file_get_size(entry->source, &entry->size)) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_IO, errno,
                ROMX_OFFSET_UNKNOWN, "failed to open mutable bundle source");
        }
        result = file_crc(entry->source, entry->size, options->io_chunk_size,
            &entry->crc32, error);
        if (result != ROMX_OK) {
            source_destroy(source);
            return result;
        }
    }
    if (entry_count > UINT32_C(1))
        qsort(source->entries, entry_count, sizeof(*source->entries),
            source_compare);
    if (entry_count > UINT32_C(1)) {
        char **paths = (char **)malloc((size_t)entry_count * sizeof(*paths));
        int unique;
        if (paths == NULL) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN,
                "failed to validate mutable bundle path uniqueness");
        }
        for (index = 0U; index < entry_count; ++index)
            paths[index] = source->entries[index].path;
        unique = paths_portable_unique(paths, entry_count);
        free(paths);
        if (unique <= 0) {
            source_destroy(source);
            return romx_error_set(error,
                unique < 0 ? ROMX_E_OUT_OF_MEMORY : ROMX_E_MUTABLE_BUNDLE,
                0, ROMX_OFFSET_UNKNOWN,
                unique < 0
                    ? "failed to validate mutable bundle path uniqueness"
                    : "mutable bundle paths are not portable-unique");
        }
    }

    path_offset = BUNDLE_HEADER_SIZE +
        (uint64_t)entry_count * BUNDLE_ENTRY_SIZE;
    if (path_offset > options->max_bundle_size) {
        source_destroy(source);
        return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle directory exceeds the limit");
    }
    for (index = 0U; index < entry_count; ++index) {
        size_t path_size = strlen(source->entries[index].path);
        if ((uint64_t)path_size > UINT64_MAX - path_offset) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "mutable bundle path table overflows");
        }
        path_offset += (uint64_t)path_size;
    }
    data_cursor = align64(path_offset);
    if (data_cursor == UINT64_MAX) {
        source_destroy(source);
        return romx_error_set(error, ROMX_E_RANGE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle alignment overflows");
    }
    for (index = 0U; index < entry_count; ++index) {
        source->entries[index].data_offset = data_cursor;
        if (source->entries[index].size > UINT64_MAX - data_cursor) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "mutable bundle data overflows");
        }
        data_cursor = align64(data_cursor + source->entries[index].size);
        if (data_cursor == UINT64_MAX) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "mutable bundle alignment overflows");
        }
    }
    if (data_cursor > options->max_bundle_size ||
        data_cursor > (uint64_t)INT64_MAX ||
        data_cursor > (uint64_t)SIZE_MAX) {
        source_destroy(source);
        return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle exceeds the configured limit");
    }
    source->prefix_size = entry_count == UINT32_C(0)
        ? BUNDLE_HEADER_SIZE : source->entries[0].data_offset;
    source->bundle_size = data_cursor;
    source->prefix = (uint8_t *)calloc(1U, (size_t)source->prefix_size);
    if (source->prefix == NULL) {
        source_destroy(source);
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate mutable bundle directory");
    }
    memcpy(source->prefix, "RMBL", 4U);
    romx_write_le16(source->prefix + 0x04U, ROMX_MUTABLE_BUNDLE_VERSION);
    romx_write_le16(source->prefix + 0x06U, UINT16_C(64));
    romx_write_le16(source->prefix + 0x08U, object_namespace);
    romx_write_le16(source->prefix + 0x0AU, UINT16_C(0));
    romx_write_le32(source->prefix + 0x0CU, UINT32_C(64));
    romx_write_le32(source->prefix + 0x10U, entry_count);
    romx_write_le64(source->prefix + 0x18U, BUNDLE_HEADER_SIZE);
    romx_write_le64(source->prefix + 0x20U, BUNDLE_HEADER_SIZE +
        (uint64_t)entry_count * BUNDLE_ENTRY_SIZE);
    romx_write_le64(source->prefix + 0x28U, source->prefix_size);
    romx_write_le64(source->prefix + 0x30U, source->bundle_size);

    path_offset = BUNDLE_HEADER_SIZE +
        (uint64_t)entry_count * BUNDLE_ENTRY_SIZE;
    for (index = 0U; index < entry_count; ++index) {
        uint8_t *stored = source->prefix + BUNDLE_HEADER_SIZE +
            (uint64_t)index * BUNDLE_ENTRY_SIZE;
        size_t path_size = strlen(source->entries[index].path);
        romx_write_le64(stored + 0x00U, path_offset);
        romx_write_le32(stored + 0x08U, (uint32_t)path_size);
        romx_write_le64(stored + 0x10U, source->entries[index].data_offset);
        romx_write_le64(stored + 0x18U, source->entries[index].size);
        romx_write_le32(stored + 0x20U, source->entries[index].crc32);
        memcpy(source->prefix + (size_t)path_offset,
            source->entries[index].path, path_size);
        path_offset += (uint64_t)path_size;
    }
    romx_write_le32(source->prefix + 0x38U, header_crc(source->prefix));
    return ROMX_OK;
}

romx_result_t romx_mutable_bundle_measure_path_entries(
    romx_mutable_namespace_t object_namespace,
    const romx_mutable_bundle_path_entry_t *entries, uint32_t entry_count,
    const romx_mutable_bundle_options_t *bundle_options,
    uint64_t *serialized_size, romx_error_t *error)
{
    romx_mutable_bundle_options_t options;
    bundle_source_t source;
    romx_result_t result;
    if (serialized_size != NULL) *serialized_size = UINT64_C(0);
    if (serialized_size == NULL || !effective_options(bundle_options, &options) ||
        (entries == NULL && entry_count != UINT32_C(0)) ||
        entry_count > options.max_entry_count ||
        (object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE &&
         object_namespace != ROMX_MUTABLE_NAMESPACE_CHEAT)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable bundle measure arguments");
    }
    result = build_source(entries, entry_count, object_namespace, &options,
        &source, error);
    if (result != ROMX_OK) return result;
    *serialized_size = source.bundle_size;
    source_destroy(&source);
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_mutable_bundle_write_path_entries(const char *romx_path,
    romx_mutable_namespace_t object_namespace, const char *key,
    const romx_mutable_bundle_path_entry_t *entries, uint32_t entry_count,
    const romx_mutable_bundle_options_t *bundle_options,
    const romx_mutable_write_options_t *write_options,
    romx_mutable_object_info_t *written, romx_error_t *error)
{
    romx_mutable_bundle_options_t options;
    bundle_source_t source;
    romx_io_t io = ROMX_IO_INIT;
    romx_result_t result;
    if (!effective_options(bundle_options, &options) ||
        romx_path == NULL || key == NULL ||
        (entries == NULL && entry_count != UINT32_C(0)) ||
        entry_count > options.max_entry_count ||
        (object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE &&
         object_namespace != ROMX_MUTABLE_NAMESPACE_CHEAT)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable bundle write arguments");
    }
    result = build_source(entries, entry_count, object_namespace, &options,
        &source, error);
    if (result != ROMX_OK) return result;
    io.user_data = &source;
    io.get_size = source_get_size;
    io.read_at = source_read_at;
    result = romx_mutable_write_io_path(romx_path, object_namespace, key, &io,
        write_options, written, error);
    source_destroy(&source);
    return result;
}

static romx_result_t bundle_read_exact(romx_mutable_file_t *file,
    uint64_t offset, void *buffer, uint64_t size, romx_error_t *error)
{
    uint64_t position = UINT64_C(0);
    uint64_t read = UINT64_C(0);
    romx_result_t result = romx_mutable_file_seek(file, (int64_t)offset,
        ROMX_PAYLOAD_SEEK_START, &position, error);
    if (result != ROMX_OK) return result;
    result = romx_mutable_file_read(file, buffer, size, &read, error);
    if (result != ROMX_OK) return result;
    if (read != size) return romx_error_set(error, ROMX_E_TRUNCATED, 0,
        offset + read, "mutable bundle is truncated");
    return ROMX_OK;
}

static romx_result_t validate_zero_range(romx_mutable_file_t *file,
    uint64_t offset, uint64_t size, uint32_t chunk_size, romx_error_t *error)
{
    uint8_t *buffer;
    romx_result_t result = ROMX_OK;
    if (size == UINT64_C(0)) return ROMX_OK;
    buffer = (uint8_t *)malloc(chunk_size);
    if (buffer == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        offset, "failed to allocate mutable bundle padding buffer");
    while (size != UINT64_C(0)) {
        uint64_t count = size > chunk_size ? chunk_size : size;
        result = bundle_read_exact(file, offset, buffer, count, error);
        if (result != ROMX_OK || !romx_bytes_zero(buffer, (size_t)count)) {
            if (result == ROMX_OK)
                result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
                    offset, "mutable bundle padding is not zero");
            break;
        }
        offset += count;
        size -= count;
    }
    free(buffer);
    return result;
}

static romx_result_t validate_entry_crc(romx_mutable_file_t *file,
    const bundle_entry_t *entry, uint32_t chunk_size, romx_error_t *error)
{
    uint8_t *buffer = (uint8_t *)malloc(chunk_size);
    uint64_t position = UINT64_C(0);
    uint32_t crc = romx_crc32_begin();
    romx_result_t result = ROMX_OK;
    if (buffer == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        entry->data_offset, "failed to allocate mutable bundle CRC buffer");
    while (position < entry->data_size) {
        uint64_t count = entry->data_size - position;
        if (count > chunk_size) count = chunk_size;
        result = bundle_read_exact(file, entry->data_offset + position,
            buffer, count, error);
        if (result != ROMX_OK) break;
        crc = romx_crc32_update(crc, buffer, (size_t)count);
        position += count;
    }
    free(buffer);
    if (result != ROMX_OK) return result;
    if (romx_crc32_finish(crc) != entry->data_crc32)
        return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            entry->data_offset, "mutable bundle entry CRC32 mismatch");
    return ROMX_OK;
}

void romx_mutable_bundle_close(romx_mutable_bundle_t *bundle)
{
    uint32_t index;
    if (bundle == NULL) return;
    for (index = 0U; index < bundle->entry_count; ++index)
        free(bundle->entries[index].path);
    free(bundle->entries);
    destroy_save_slot_array(bundle->save_slots, bundle->save_slot_count);
    free(bundle->object_key);
    romx_mutable_file_close(bundle->file);
    free(bundle);
}

romx_result_t romx_mutable_bundle_open(const romx_reader_t *reader,
    romx_mutable_namespace_t object_namespace, const char *key,
    const romx_mutable_bundle_options_t *provided,
    romx_mutable_bundle_t **out_bundle, romx_error_t *error)
{
    romx_mutable_bundle_options_t options;
    romx_mutable_bundle_t *bundle = NULL;
    uint8_t header[64];
    uint8_t *directory = NULL;
    uint8_t *paths = NULL;
    uint64_t object_size = UINT64_C(0);
    uint64_t directory_size;
    uint64_t path_table_offset;
    uint64_t data_offset;
    uint64_t bundle_size;
    uint64_t path_cursor;
    uint64_t data_cursor;
    uint32_t entry_count;
    uint32_t index;
    romx_result_t result;

    if (out_bundle != NULL) *out_bundle = NULL;
    if (!effective_options(provided, &options) ||
        reader == NULL || key == NULL || out_bundle == NULL ||
        (object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE &&
         object_namespace != ROMX_MUTABLE_NAMESPACE_CHEAT)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable bundle open arguments");
    }
    *out_bundle = NULL;
    bundle = (romx_mutable_bundle_t *)calloc(1U, sizeof(*bundle));
    if (bundle == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        ROMX_OFFSET_UNKNOWN, "failed to allocate mutable bundle");
    bundle->object_namespace = object_namespace;
    bundle->object_key = duplicate_path(key);
    if (bundle->object_key == NULL) {
        free(bundle);
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate mutable bundle key");
    }
    {
        romx_info_t info = ROMX_INFO_INIT;
        result = romx_reader_get_info(reader, &info, error);
        if (result != ROMX_OK) {
            free(bundle);
            return result;
        }
        bundle->profile = (romx_save_profile_info_t)ROMX_SAVE_PROFILE_INFO_INIT;
        result = romx_save_profile_get(info.platform_id,
            reader->entries[info.entrypoint_index].format_id,
            info.launch_format_id, &bundle->profile, error);
        if (result != ROMX_OK) {
            free(bundle);
            return result;
        }
    }
    result = romx_mutable_file_open(reader, object_namespace, key,
        &bundle->file, error);
    if (result != ROMX_OK) goto fail;
    result = romx_mutable_file_get_size(bundle->file, &object_size, error);
    if (result != ROMX_OK) goto fail;
    if (object_size < BUNDLE_HEADER_SIZE ||
        object_size > (uint64_t)INT64_MAX ||
        object_size > options.max_bundle_size) {
        result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle size is outside the limit");
        goto fail;
    }
    result = bundle_read_exact(bundle->file, UINT64_C(0), header,
        sizeof(header), error);
    if (result != ROMX_OK) goto fail;
    entry_count = romx_read_le32(header + 0x10U);
    directory_size = (uint64_t)entry_count * BUNDLE_ENTRY_SIZE;
    path_table_offset = romx_read_le64(header + 0x20U);
    data_offset = romx_read_le64(header + 0x28U);
    bundle_size = romx_read_le64(header + 0x30U);
    if (memcmp(header, "RMBL", 4U) != 0 ||
        romx_read_le16(header + 0x04U) != ROMX_MUTABLE_BUNDLE_VERSION ||
        romx_read_le16(header + 0x06U) != UINT16_C(64) ||
        romx_read_le16(header + 0x08U) != object_namespace ||
        romx_read_le16(header + 0x0AU) != UINT16_C(0) ||
        romx_read_le32(header + 0x0CU) != UINT32_C(64) ||
        entry_count > options.max_entry_count ||
        romx_read_le32(header + 0x14U) != UINT32_C(0) ||
        romx_read_le64(header + 0x18U) != BUNDLE_HEADER_SIZE ||
        path_table_offset != BUNDLE_HEADER_SIZE + directory_size ||
        data_offset < path_table_offset || data_offset % UINT64_C(64) != 0U ||
        bundle_size != object_size || data_offset > bundle_size ||
        romx_read_le32(header + 0x38U) != header_crc(header) ||
        !romx_bytes_zero(header + 0x3CU, 4U)) {
        result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle header is invalid");
        goto fail;
    }
    if (directory_size > (uint64_t)SIZE_MAX ||
        data_offset - path_table_offset > (uint64_t)SIZE_MAX) {
        result = romx_error_set(error, ROMX_E_RANGE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle directory exceeds address space");
        goto fail;
    }
    if (directory_size != UINT64_C(0)) {
        directory = (uint8_t *)malloc((size_t)directory_size);
        bundle->entries = (bundle_entry_t *)calloc(entry_count,
            sizeof(*bundle->entries));
        if (directory == NULL || bundle->entries == NULL) {
            result = romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN,
                "failed to allocate mutable bundle directory");
            goto fail;
        }
        result = bundle_read_exact(bundle->file, BUNDLE_HEADER_SIZE,
            directory, directory_size, error);
        if (result != ROMX_OK) goto fail;
    }
    if (data_offset != path_table_offset) {
        paths = (uint8_t *)malloc((size_t)(data_offset - path_table_offset));
        if (paths == NULL) {
            result = romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN, "failed to allocate mutable bundle paths");
            goto fail;
        }
        result = bundle_read_exact(bundle->file, path_table_offset, paths,
            data_offset - path_table_offset, error);
        if (result != ROMX_OK) goto fail;
    }
    bundle->entry_count = entry_count;
    bundle->io_chunk_size = options.io_chunk_size;
    path_cursor = path_table_offset;
    data_cursor = data_offset;
    for (index = 0U; index < entry_count; ++index) {
        const uint8_t *stored = directory + (uint64_t)index * BUNDLE_ENTRY_SIZE;
        bundle_entry_t *entry = &bundle->entries[index];
        uint64_t stored_path_offset = romx_read_le64(stored + 0x00U);
        uint32_t path_size = romx_read_le32(stored + 0x08U);
        uint64_t stored_data_offset = romx_read_le64(stored + 0x10U);
        uint64_t data_size = romx_read_le64(stored + 0x18U);
        if (stored_path_offset != path_cursor || path_size == UINT32_C(0) ||
            path_size > options.max_path_size ||
            path_cursor > data_offset ||
            path_size > data_offset - path_cursor ||
            romx_read_le32(stored + 0x0CU) != UINT32_C(0) ||
            stored_data_offset != data_cursor ||
            data_size > bundle_size - stored_data_offset ||
            !romx_bytes_zero(stored + 0x24U, 28U)) {
            result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
                BUNDLE_HEADER_SIZE + (uint64_t)index * BUNDLE_ENTRY_SIZE,
                "mutable bundle entry is invalid");
            goto fail;
        }
        entry->path = (char *)malloc((size_t)path_size + 1U);
        if (entry->path == NULL) {
            result = romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                stored_path_offset, "failed to allocate mutable bundle path");
            goto fail;
        }
        memcpy(entry->path, paths + (size_t)(path_cursor - path_table_offset),
            path_size);
        entry->path[path_size] = '\0';
        if (!romx_path_bytes_valid((const uint8_t *)entry->path, path_size) ||
            (index != UINT32_C(0) &&
             (path_compare_bytes(bundle->entries[index - 1U].path,
                    entry->path) >= 0 ||
              romx_ascii_fold_equal(bundle->entries[index - 1U].path,
                    entry->path)))) {
            result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
                stored_path_offset,
                "mutable bundle paths are invalid or not canonically ordered");
            goto fail;
        }
        entry->data_offset = stored_data_offset;
        entry->data_size = data_size;
        entry->data_crc32 = romx_read_le32(stored + 0x20U);
        path_cursor += path_size;
        data_cursor = align64(stored_data_offset + data_size);
        if (data_cursor == UINT64_MAX || data_cursor > bundle_size) {
            result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
                stored_data_offset, "mutable bundle entry range overflows");
            goto fail;
        }
    }
    if (entry_count > UINT32_C(1)) {
        char **entry_paths = (char **)malloc(
            (size_t)entry_count * sizeof(*entry_paths));
        int unique;
        if (entry_paths == NULL) {
            result = romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN,
                "failed to validate mutable bundle path uniqueness");
            goto fail;
        }
        for (index = 0U; index < entry_count; ++index)
            entry_paths[index] = bundle->entries[index].path;
        unique = paths_portable_unique(entry_paths, entry_count);
        free(entry_paths);
        if (unique <= 0) {
            result = romx_error_set(error,
                unique < 0 ? ROMX_E_OUT_OF_MEMORY : ROMX_E_MUTABLE_BUNDLE,
                0, ROMX_OFFSET_UNKNOWN,
                unique < 0
                    ? "failed to validate mutable bundle path uniqueness"
                    : "mutable bundle paths are not portable-unique");
            goto fail;
        }
    }
    if (align64(path_cursor) != data_offset ||
        data_cursor != bundle_size) {
        result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle has non-canonical layout");
        goto fail;
    }
    result = validate_zero_range(bundle->file, path_cursor,
        data_offset - path_cursor, options.io_chunk_size, error);
    if (result != ROMX_OK) goto fail;
    for (index = 0U; index < entry_count; ++index) {
        const bundle_entry_t *entry = &bundle->entries[index];
        uint64_t end = entry->data_offset + entry->data_size;
        uint64_t aligned_end = align64(end);
        result = validate_entry_crc(bundle->file, entry,
            options.io_chunk_size, error);
        if (result != ROMX_OK) goto fail;
        result = validate_zero_range(bundle->file, end, aligned_end - end,
            options.io_chunk_size, error);
        if (result != ROMX_OK) goto fail;
    }
    if (object_namespace == ROMX_MUTABLE_NAMESPACE_SAVE) {
        result = mutable_bundle_analyze_save_layout(bundle,
            &bundle->save_layout, error);
        if (result != ROMX_OK) goto fail;
        result = build_save_slots(bundle, error);
        if (result != ROMX_OK) goto fail;
    }
    free(directory);
    free(paths);
    *out_bundle = bundle;
    romx_error_clear(error);
    return ROMX_OK;

fail:
    free(directory);
    free(paths);
    romx_mutable_bundle_close(bundle);
    return result;
}

romx_result_t romx_mutable_bundle_get_entry_count(
    const romx_mutable_bundle_t *bundle, uint32_t *count, romx_error_t *error)
{
    if (bundle == NULL || count == NULL)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable bundle count arguments");
    *count = bundle->entry_count;
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_mutable_bundle_get_entry(
    const romx_mutable_bundle_t *bundle, uint32_t index,
    romx_mutable_bundle_entry_info_t *entry, romx_error_t *error)
{
    const bundle_entry_t *stored;
    size_t path_size;
    if (bundle == NULL || entry == NULL ||
        entry->struct_size < (uint32_t)sizeof(*entry) ||
        index >= bundle->entry_count)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable bundle entry arguments");
    stored = &bundle->entries[index];
    path_size = strlen(stored->path);
    memset(entry, 0, sizeof(*entry));
    entry->struct_size = (uint32_t)sizeof(*entry);
    entry->index = index;
    entry->data_size = stored->data_size;
    entry->data_crc32 = stored->data_crc32;
    entry->path_size = (uint32_t)path_size;
    memcpy(entry->path, stored->path, path_size + 1U);
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_mutable_bundle_get_save_layout(
    const romx_mutable_bundle_t *bundle,
    romx_mutable_save_layout_info_t *layout, romx_error_t *error)
{
    uint32_t supplied_size;
    if (bundle == NULL || layout == NULL ||
        layout->struct_size < (uint32_t)sizeof(*layout) ||
        bundle->object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable SAVE layout arguments");
    supplied_size = layout->struct_size;
    *layout = bundle->save_layout;
    layout->struct_size = supplied_size;
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_mutable_bundle_get_save_slot_count(
    const romx_mutable_bundle_t *bundle, uint32_t *count, romx_error_t *error)
{
    if (bundle == NULL || count == NULL ||
        bundle->object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid SAVE slot count arguments");
    *count = bundle->save_slot_count;
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_mutable_bundle_get_save_slot(
    const romx_mutable_bundle_t *bundle, uint32_t index,
    romx_mutable_save_slot_info_t *slot, romx_error_t *error)
{
    const bundle_save_slot_t *stored;
    size_t key_size, display_name_size;
    if (bundle == NULL || slot == NULL ||
        slot->struct_size < (uint32_t)sizeof(*slot) ||
        bundle->object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE ||
        index >= bundle->save_slot_count)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid SAVE slot arguments");
    stored = &bundle->save_slots[index];
    key_size = strlen(stored->key);
    display_name_size = strlen(stored->display_name);
    memset(slot, 0, sizeof(*slot));
    slot->struct_size = (uint32_t)sizeof(*slot);
    slot->index = index;
    slot->entry_count = stored->entry_count;
    slot->key_size = (uint32_t)key_size;
    slot->data_size = stored->data_size;
    slot->display_name_size = (uint32_t)display_name_size;
    slot->is_directory = stored->is_directory ? UINT32_C(1) : UINT32_C(0);
    memcpy(slot->key, stored->key, key_size + 1U);
    memcpy(slot->display_name, stored->display_name,
        display_name_size + 1U);
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_mutable_bundle_get_save_slot_entry(
    const romx_mutable_bundle_t *bundle, uint32_t slot_index,
    uint32_t entry_index, romx_mutable_bundle_entry_info_t *entry,
    romx_error_t *error)
{
    const bundle_save_slot_t *slot;
    if (bundle == NULL || entry == NULL ||
        bundle->object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE ||
        slot_index >= bundle->save_slot_count)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid SAVE slot entry arguments");
    slot = &bundle->save_slots[slot_index];
    if (entry_index >= slot->entry_count)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "SAVE slot entry index is out of range");
    return romx_mutable_bundle_get_entry(bundle,
        slot->entry_indices[entry_index], entry, error);
}

romx_result_t romx_mutable_bundle_read_entry(romx_mutable_bundle_t *bundle,
    uint32_t index, uint64_t entry_offset, void *buffer,
    uint64_t buffer_size, uint64_t *bytes_read, romx_error_t *error)
{
    const bundle_entry_t *entry;
    uint64_t count;
    uint64_t position = UINT64_C(0);
    romx_result_t result;
    if (bundle == NULL || index >= bundle->entry_count ||
        bytes_read == NULL || (buffer == NULL && buffer_size != UINT64_C(0)))
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable bundle read arguments");
    entry = &bundle->entries[index];
    if (entry_offset > entry->data_size)
        return romx_error_set(error, ROMX_E_RANGE, 0, entry_offset,
            "mutable bundle entry offset is out of range");
    count = entry->data_size - entry_offset;
    if (count > buffer_size) count = buffer_size;
    result = romx_mutable_file_seek(bundle->file,
        (int64_t)(entry->data_offset + entry_offset),
        ROMX_PAYLOAD_SEEK_START, &position, error);
    if (result != ROMX_OK) return result;
    result = romx_mutable_file_read(bundle->file, buffer, count,
        bytes_read, error);
    if (result == ROMX_OK && *bytes_read != count)
        return romx_error_set(error, ROMX_E_TRUNCATED, 0,
            entry->data_offset + entry_offset + *bytes_read,
            "mutable bundle entry is truncated");
    return result;
}
