#include <romx/romx.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define get_process_id _getpid
#define make_directory(path) _mkdir(path)
#define remove_directory(path) _rmdir(path)
#define remove_file(path) _unlink(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define get_process_id getpid
#define make_directory(path) mkdir((path), 0700)
#define remove_directory(path) rmdir(path)
#define remove_file(path) unlink(path)
#endif

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #expression); \
        return 0; \
    } \
} while (0)

typedef struct memory_source {
    const uint8_t *bytes;
    uint64_t size;
} memory_source_t;

static romx_result_t memory_get_size(void *user_data, uint64_t *size,
    romx_error_t *error)
{
    memory_source_t *source = (memory_source_t *)user_data;
    (void)error;
    *size = source->size;
    return ROMX_OK;
}

static romx_result_t memory_read_at(void *user_data, uint64_t offset,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    memory_source_t *source = (memory_source_t *)user_data;
    uint64_t count;
    (void)error;
    if (offset > source->size) return ROMX_E_RANGE;
    count = source->size - offset;
    if (count > size) count = size;
    if (count != UINT64_C(0))
        memcpy(buffer, source->bytes + (size_t)offset, (size_t)count);
    *bytes_read = count;
    return ROMX_OK;
}

static romx_io_t memory_io(memory_source_t *source)
{
    romx_io_t io = ROMX_IO_INIT;
    io.user_data = source;
    io.get_size = memory_get_size;
    io.read_at = memory_read_at;
    return io;
}

static int make_dir(const char *path)
{
    if (make_directory(path) == 0 || errno == EEXIST) return 1;
    return 0;
}

static int join_path(char *output, size_t capacity, const char *base,
    const char *name)
{
    int written = snprintf(output, capacity, "%s/%s", base, name);
    return written >= 0 && (size_t)written < capacity;
}

static int write_bytes(const char *path, const void *bytes, size_t size)
{
    FILE *stream = fopen(path, "wb");
    int result;
    if (stream == NULL) return 0;
    result = size == 0U || fwrite(bytes, 1U, size, stream) == size;
    if (fclose(stream) != 0) result = 0;
    return result;
}

static int create_platform_romx(const char *path, uint16_t platform,
    uint16_t format)
{
    static const uint8_t payload[] = { 0x43U, 0x54U, 0x52U, 0x4FU };
    static const uint8_t metadata[] =
        "{\"schema_version\":\"0.2.0\",\"name\":\"save manager test\"}";
    memory_source_t source = { payload, sizeof(payload) };
    romx_io_t io = memory_io(&source);
    romx_writer_io_entry_t entry = ROMX_WRITER_IO_ENTRY_INIT;
    romx_writer_options_t options = ROMX_WRITER_OPTIONS_INIT;
    romx_writer_report_t report = ROMX_WRITER_REPORT_INIT;
    romx_error_t error;
    entry.flags = ROMX_RIDX_ENTRYPOINT | ROMX_RIDX_HAS_CRC32;
    entry.virtual_path = "game.3ds";
    entry.source = &io;
    entry.format_id = format;
    options.flags = ROMX_WRITER_IMMUTABLE_SHA256 | ROMX_WRITER_REPLACE_EXISTING;
    options.platform_id = platform;
    options.launch_format_id = ROMX_LAUNCH_RAW_SINGLE_FILE;
    options.mutable_capacity = UINT64_C(16384);
    options.mutable_entry_capacity = UINT32_C(8);
    return romx_writer_write_io_entries(path, &entry, 1U, metadata,
        sizeof(metadata) - 1U, NULL, &options, &report, &error) == ROMX_OK;
}

static int create_test_romx(const char *path)
{
    return create_platform_romx(path, ROMX_PLATFORM_NINTENDO_3DS,
        ROMX_FORMAT_N3DS);
}

static int check_candidate(const romx_save_catalog_t *catalog, uint32_t index,
    const char *key, uint32_t expected_files, int is_directory,
    romx_save_source_format_t source_format)
{
    romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
    romx_save_file_info_t file = ROMX_SAVE_FILE_INFO_INIT;
    uint32_t count;
    romx_error_t error;
    CHECK(romx_save_catalog_get_candidate(catalog, index, &candidate,
        &error) == ROMX_OK);
    CHECK(strcmp(candidate.key, key) == 0);
    CHECK(candidate.file_count == expected_files);
    CHECK((candidate.flags & ROMX_SAVE_CANDIDATE_IS_DIRECTORY) != 0U ==
        (is_directory != 0));
    CHECK(candidate.source_format == source_format);
    CHECK((candidate.flags & ROMX_SAVE_CANDIDATE_IS_MULTI_FILE) != 0U ==
        (expected_files > 1U));
    CHECK(romx_save_catalog_get_file_count(catalog, index, &count,
        &error) == ROMX_OK && count == expected_files);
    if (expected_files != 0U) {
        CHECK(romx_save_catalog_get_file(catalog, index, 0U, &file,
            &error) == ROMX_OK);
        CHECK(file.path_size != 0U && file.path[0] != '/');
    }
    return 1;
}

static int test_3ds_directory_grouping(const char *root)
{
    char directory_root[1024];
    char save_one[1024];
    char save_two[1024];
    char path[1024];
    static const uint8_t first[] = { 1U, 2U, 3U };
    static const uint8_t second[] = { 4U, 5U };
    static const uint8_t third[] = { 6U, 7U, 8U, 9U };
    romx_save_scan_options_t options = ROMX_SAVE_SCAN_OPTIONS_INIT;
    romx_save_catalog_t *catalog = NULL;
    romx_save_profile_info_t profile = ROMX_SAVE_PROFILE_INFO_INIT;
    romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
    romx_error_t error;
    uint32_t count;
    char source_path[1024];
    uint64_t required;

    CHECK(join_path(directory_root, sizeof(directory_root), root,
        "3ds-directories"));
    CHECK(join_path(save_one, sizeof(save_one), directory_root, "save-1"));
    CHECK(join_path(save_two, sizeof(save_two), directory_root, "save-2"));
    CHECK(make_dir(directory_root));
    CHECK(make_dir(save_one));
    CHECK(make_dir(save_two));
    CHECK(join_path(path, sizeof(path), save_one, "save00.bin"));
    CHECK(write_bytes(path, first, sizeof(first)));
    CHECK(join_path(path, sizeof(path), save_one, "system.dat"));
    CHECK(write_bytes(path, second, sizeof(second)));
    CHECK(join_path(path, sizeof(path), save_two, "Data0"));
    CHECK(write_bytes(path, third, sizeof(third)));

    options.platform_id = ROMX_PLATFORM_NINTENDO_3DS;
    options.format_id = ROMX_FORMAT_N3DS;
    CHECK(romx_save_profile_get(options.platform_id, options.format_id,
        options.launch_format_id, &profile, &error) == ROMX_OK);
    CHECK(profile.grouping == ROMX_SAVE_GROUP_DIRECTORY_PER_SAVE);
    CHECK(romx_save_catalog_open_path(directory_root, &options, &catalog,
        &error) == ROMX_OK);
    CHECK(romx_save_catalog_get_candidate_count(catalog, &count, &error) ==
        ROMX_OK && count == 2U);
    CHECK(check_candidate(catalog, 0U, "save-1", 2U, 1,
        ROMX_SAVE_SOURCE_3DS_BACKUP));
    CHECK(check_candidate(catalog, 1U, "save-2", 1U, 1,
        ROMX_SAVE_SOURCE_3DS_BACKUP));
    CHECK(romx_save_catalog_get_candidate(catalog, 0U, &candidate,
        &error) == ROMX_OK);
    CHECK(romx_save_catalog_copy_candidate_source_path(catalog, 0U,
        source_path, sizeof(source_path), &required, &error) == ROMX_OK);
    CHECK(strcmp(source_path, save_one) == 0);
    romx_save_catalog_close(catalog);
    return 1;
}

static int test_3ds_single_files(const char *root)
{
    char directory_root[1024];
    char first_path[1024];
    char second_path[1024];
    static const uint8_t first[] = { 0x10U };
    static const uint8_t second[] = { 0x20U, 0x21U };
    romx_save_scan_options_t options = ROMX_SAVE_SCAN_OPTIONS_INIT;
    romx_save_catalog_t *catalog = NULL;
    romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
    romx_error_t error;
    uint32_t count;

    CHECK(join_path(directory_root, sizeof(directory_root), root,
        "3ds-single-files"));
    CHECK(make_dir(directory_root));
    CHECK(join_path(first_path, sizeof(first_path), directory_root, "slot-1.sav"));
    CHECK(join_path(second_path, sizeof(second_path), directory_root, "slot-2.sav"));
    CHECK(write_bytes(first_path, first, sizeof(first)));
    CHECK(write_bytes(second_path, second, sizeof(second)));
    options.platform_id = ROMX_PLATFORM_NINTENDO_3DS;
    options.format_id = ROMX_FORMAT_N3DS;
    CHECK(romx_save_catalog_open_path(directory_root, &options, &catalog,
        &error) == ROMX_OK);
    CHECK(romx_save_catalog_get_candidate_count(catalog, &count, &error) ==
        ROMX_OK && count == 2U);
    CHECK(romx_save_catalog_get_candidate(catalog, 0U, &candidate,
        &error) == ROMX_OK);
    CHECK(strcmp(candidate.key, "slot-1") == 0);
    CHECK(candidate.grouping == ROMX_SAVE_GROUP_SINGLE_FILE);
    CHECK((candidate.flags & ROMX_SAVE_CANDIDATE_IS_DIRECTORY) == 0U);
    CHECK(romx_save_catalog_get_candidate(catalog, 1U, &candidate,
        &error) == ROMX_OK);
    CHECK(strcmp(candidate.key, "slot-2") == 0);
    romx_save_catalog_close(catalog);
    return 1;
}

static int test_gateway_identity(const char *root)
{
    char path[1024];
    static const uint8_t bytes[] = { 0xaaU, 0xbbU };
    romx_save_scan_options_t options = ROMX_SAVE_SCAN_OPTIONS_INIT;
    romx_save_catalog_t *catalog = NULL;
    romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
    romx_error_t error;
    uint32_t count;
    CHECK(join_path(path, sizeof(path), root, "0004000000078B00.sav"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    options.platform_id = ROMX_PLATFORM_NINTENDO_3DS;
    options.format_id = ROMX_FORMAT_N3DS;
    CHECK(romx_save_catalog_open_path(path, &options, &catalog, &error) ==
        ROMX_OK);
    CHECK(romx_save_catalog_get_candidate_count(catalog, &count, &error) ==
        ROMX_OK && count == 1U);
    CHECK(romx_save_catalog_get_candidate(catalog, 0U, &candidate,
        &error) == ROMX_OK);
    CHECK(candidate.source_format == ROMX_SAVE_SOURCE_3DS_GATEWAY);
    CHECK((candidate.flags & ROMX_SAVE_CANDIDATE_HAS_TITLE_ID) != 0U);
    CHECK(strcmp(candidate.title_id, "0004000000078B00") == 0);
    romx_save_catalog_close(catalog);
    return 1;
}

static int test_3ds_special_directories(const char *root)
{
    char fixture[1024];
    char game[1024];
    char timestamp[1024];
    char savedata[1024];
    char citra[1024];
    char title[1024];
    char data[1024];
    char citra_save[1024];
    char azahar[1024];
    char azahar_sdmc[1024];
    char azahar_nintendo[1024];
    char azahar_system[1024];
    char azahar_sdcard[1024];
    char azahar_title[1024];
    char azahar_high[1024];
    char azahar_low[1024];
    char azahar_data[1024];
    char azahar_save[1024];
    char gateway[1024];
    char path[1024];
    static const uint8_t bytes[] = { 0x51U, 0x52U };
    romx_save_scan_options_t options = ROMX_SAVE_SCAN_OPTIONS_INIT;
    romx_save_catalog_t *catalog = NULL;
    romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
    romx_error_t error;
    uint32_t count;

    CHECK(join_path(fixture, sizeof(fixture), root, "3ds-special"));
    CHECK(join_path(game, sizeof(game), fixture, "game-a"));
    CHECK(join_path(timestamp, sizeof(timestamp), game, "20150101010101"));
    CHECK(join_path(savedata, sizeof(savedata), timestamp, "000015d8"));
    CHECK(make_dir(fixture));
    CHECK(make_dir(game));
    CHECK(make_dir(timestamp));
    CHECK(make_dir(savedata));
    CHECK(join_path(path, sizeof(path), timestamp, "export.log"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(join_path(path, sizeof(path), timestamp, "000015d8.dat"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(join_path(path, sizeof(path), timestamp, "000015d8_.dat"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(join_path(path, sizeof(path), savedata, "Data0"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(join_path(path, sizeof(path), savedata, "Data1"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));

    CHECK(join_path(citra, sizeof(citra), fixture, "citra"));
    CHECK(join_path(title, sizeof(title), citra, "title"));
    CHECK(join_path(data, sizeof(data), title, "0004000000030000"));
    CHECK(join_path(citra_save, sizeof(citra_save), data, "data"));
    CHECK(join_path(path, sizeof(path), citra_save, "00000001"));
    CHECK(make_dir(citra));
    CHECK(make_dir(title));
    CHECK(make_dir(data));
    CHECK(make_dir(citra_save));
    CHECK(make_dir(path));
    CHECK(join_path(path, sizeof(path), citra_save, "00000001/data.dat"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));

    CHECK(join_path(azahar, sizeof(azahar), fixture, "azahar"));
    CHECK(join_path(azahar_sdmc, sizeof(azahar_sdmc), azahar, "sdmc"));
    CHECK(join_path(azahar_nintendo, sizeof(azahar_nintendo), azahar_sdmc,
        "Nintendo 3DS"));
    CHECK(join_path(azahar_system, sizeof(azahar_system), azahar_nintendo,
        "00000000000000000000000000000000"));
    CHECK(join_path(azahar_sdcard, sizeof(azahar_sdcard), azahar_system,
        "00000000000000000000000000000000"));
    CHECK(join_path(azahar_title, sizeof(azahar_title), azahar_sdcard,
        "title"));
    CHECK(join_path(azahar_high, sizeof(azahar_high), azahar_title,
        "00040000"));
    CHECK(join_path(azahar_low, sizeof(azahar_low), azahar_high,
        "00040001"));
    CHECK(join_path(azahar_data, sizeof(azahar_data), azahar_low, "data"));
    CHECK(join_path(azahar_save, sizeof(azahar_save), azahar_data,
        "00000001"));
    CHECK(make_dir(azahar));
    CHECK(make_dir(azahar_sdmc));
    CHECK(make_dir(azahar_nintendo));
    CHECK(make_dir(azahar_system));
    CHECK(make_dir(azahar_sdcard));
    CHECK(make_dir(azahar_title));
    CHECK(make_dir(azahar_high));
    CHECK(make_dir(azahar_low));
    CHECK(make_dir(azahar_data));
    CHECK(make_dir(azahar_save));
    CHECK(join_path(path, sizeof(path), azahar_save, "savedata.dat"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));

    CHECK(join_path(gateway, sizeof(gateway), fixture, "gateway-game"));
    CHECK(make_dir(gateway));
    CHECK(join_path(path, sizeof(path), gateway, "0004000000078B00.sav"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(join_path(path, sizeof(path), fixture, "readme.txt"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(join_path(path, sizeof(path), fixture, "download.cia"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(join_path(path, sizeof(path), fixture, "dlc"));
    CHECK(make_dir(path));
    CHECK(join_path(path, sizeof(path), fixture, "dlc/content.cia"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));

    options.platform_id = ROMX_PLATFORM_NINTENDO_3DS;
    options.format_id = ROMX_FORMAT_N3DS;
    CHECK(romx_save_catalog_open_path(fixture, &options, &catalog, &error) ==
        ROMX_OK);
    CHECK(romx_save_catalog_get_candidate_count(catalog, &count, &error) ==
        ROMX_OK && count == 4U);
    {
        uint32_t index;
        int saw_savedatafiler = 0;
        int saw_citra = 0;
        int saw_azahar = 0;
        int saw_gateway = 0;
        for (index = 0U; index < count; ++index) {
            candidate = (romx_save_candidate_info_t)
                ROMX_SAVE_CANDIDATE_INFO_INIT;
            CHECK(romx_save_catalog_get_candidate(catalog, index, &candidate,
                &error) == ROMX_OK);
            if (candidate.source_format == ROMX_SAVE_SOURCE_3DS_SAVEDATAFILER) {
                saw_savedatafiler = 1;
                CHECK(strcmp(candidate.key, "20150101010101") == 0 &&
                    candidate.file_count == 5U &&
                    strcmp(candidate.extdata_id, "00000000000015D8") == 0 &&
                    (candidate.flags & ROMX_SAVE_CANDIDATE_NEEDS_TITLE_MAP) != 0U);
            } else if (candidate.source_format == ROMX_SAVE_SOURCE_3DS_CITRA) {
                if (strcmp(candidate.title_id, "0004000000030000") == 0) {
                    saw_citra = 1;
                    CHECK(candidate.scope == ROMX_SAVE_SCOPE_3DS_TITLE &&
                        candidate.extdata_id_size == 0U);
                } else if (strcmp(candidate.title_id, "0004000000040001") == 0) {
                    saw_azahar = 1;
                    CHECK(candidate.scope == ROMX_SAVE_SCOPE_3DS_TITLE &&
                        candidate.extdata_id_size == 0U);
                }
            } else if (candidate.source_format == ROMX_SAVE_SOURCE_3DS_GATEWAY) {
                saw_gateway = 1;
                CHECK(strcmp(candidate.title_id, "0004000000078B00") == 0 &&
                    candidate.scope == ROMX_SAVE_SCOPE_3DS_TITLE);
            }
        }
        CHECK(saw_savedatafiler && saw_citra && saw_azahar && saw_gateway);
    }
    romx_save_catalog_close(catalog);
    return 1;
}

static int test_3ds_flat_citra_directory(const char *root)
{
    char source[1024];
    char path[1024];
    char romx_path[1024];
    static const uint8_t bytes[] = { 0x71U, 0x72U, 0x73U };
    romx_save_scan_options_t options = ROMX_SAVE_SCAN_OPTIONS_INIT;
    romx_mutable_bundle_options_t bundle_options =
        ROMX_MUTABLE_BUNDLE_OPTIONS_INIT;
    romx_mutable_write_options_t write_options = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
    romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
    romx_save_catalog_t *catalog = NULL;
    romx_reader_t *reader = NULL;
    romx_mutable_bundle_t *bundle = NULL;
    romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
    romx_mutable_save_layout_info_t layout =
        ROMX_MUTABLE_SAVE_LAYOUT_INFO_INIT;
    romx_mutable_bundle_entry_info_t entry = ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
    romx_error_t error;
    uint32_t count;

    CHECK(join_path(source, sizeof(source), root, "3ds-citra-flat"));
    CHECK(join_path(path, sizeof(path), source, "saveData.bin"));
    CHECK(join_path(romx_path, sizeof(romx_path), root,
        "3ds-citra-flat.romx"));
    CHECK(make_dir(source));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    options.platform_id = ROMX_PLATFORM_NINTENDO_3DS;
    options.format_id = ROMX_FORMAT_N3DS;
    CHECK(romx_save_catalog_open_path(source, &options, &catalog, &error) ==
        ROMX_OK);
    CHECK(romx_save_catalog_get_candidate_count(catalog, &count, &error) ==
        ROMX_OK && count == 1U);
    CHECK(romx_save_catalog_get_candidate(catalog, 0U, &candidate, &error) ==
        ROMX_OK);
    CHECK(strcmp(candidate.key, "3ds-citra-flat") == 0 &&
        candidate.source_format == ROMX_SAVE_SOURCE_3DS_CITRA &&
        candidate.scope == ROMX_SAVE_SCOPE_3DS_TITLE &&
        candidate.file_count == 1U &&
        (candidate.flags & ROMX_SAVE_CANDIDATE_IS_DIRECTORY) != 0U);
    CHECK(create_test_romx(romx_path));
    CHECK(romx_save_catalog_write_candidate(catalog, 0U, romx_path, NULL,
        &bundle_options, &write_options, &object, &error) == ROMX_OK);
    romx_save_catalog_close(catalog);
    catalog = NULL;
    CHECK(romx_reader_open_path(romx_path, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_mutable_bundle_open(reader, ROMX_MUTABLE_NAMESPACE_SAVE,
        "3ds-citra-flat", NULL, &bundle, &error) == ROMX_OK);
    CHECK(romx_mutable_bundle_get_save_layout(bundle, &layout, &error) ==
        ROMX_OK && layout.scope == ROMX_SAVE_SCOPE_3DS_TITLE &&
        layout.extdata_id_size == 0U);
    CHECK(romx_mutable_bundle_get_entry(bundle, 0U, &entry, &error) == ROMX_OK &&
        strcmp(entry.path, "saveData.bin") == 0);
    romx_mutable_bundle_close(bundle);
    romx_reader_close(reader);
    return 1;
}

static int test_3ds_extdata_scope_and_write(const char *root)
{
    char fixture[1024];
    char citra[1024];
    char extdata[1024];
    char high[1024];
    char low[1024];
    char user[1024];
    char savedata[1024];
    char timestamp[1024];
    char filer_low[1024];
    char path[1024];
    char romx_path[1024];
    static const uint8_t bytes[] = { 0x61U, 0x62U, 0x63U };
    romx_save_scan_options_t options = ROMX_SAVE_SCAN_OPTIONS_INIT;
    romx_mutable_bundle_options_t bundle_options =
        ROMX_MUTABLE_BUNDLE_OPTIONS_INIT;
    romx_mutable_write_options_t write_options = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
    romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
    romx_save_catalog_t *catalog = NULL;
    romx_reader_t *reader = NULL;
    romx_mutable_bundle_t *bundle = NULL;
    romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
    romx_mutable_save_layout_info_t layout =
        ROMX_MUTABLE_SAVE_LAYOUT_INFO_INIT;
    romx_mutable_bundle_entry_info_t bundle_entry =
        ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
    romx_error_t error;
    uint32_t count;

    CHECK(join_path(fixture, sizeof(fixture), root, "3ds-extdata"));
    CHECK(join_path(citra, sizeof(citra), fixture, "citra"));
    CHECK(join_path(extdata, sizeof(extdata), citra, "extdata"));
    CHECK(join_path(high, sizeof(high), extdata, "00000000"));
    CHECK(join_path(low, sizeof(low), high, "000016e1"));
    CHECK(join_path(user, sizeof(user), low, "user"));
    CHECK(make_dir(fixture));
    CHECK(make_dir(citra));
    CHECK(make_dir(extdata));
    CHECK(make_dir(high));
    CHECK(make_dir(low));
    CHECK(make_dir(user));
    CHECK(join_path(path, sizeof(path), user, "mhr_game0.sav"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(join_path(path, sizeof(path), user, "mhr_sys.sav"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));

    options.platform_id = ROMX_PLATFORM_NINTENDO_3DS;
    options.format_id = ROMX_FORMAT_N3DS;
    CHECK(romx_save_catalog_open_path(citra, &options, &catalog, &error) ==
        ROMX_OK);
    CHECK(romx_save_catalog_get_candidate_count(catalog, &count, &error) ==
        ROMX_OK && count == 1U);
    CHECK(romx_save_catalog_get_candidate(catalog, 0U, &candidate, &error) ==
        ROMX_OK);
    CHECK(candidate.scope == ROMX_SAVE_SCOPE_3DS_EXTDATA);
    CHECK(strcmp(candidate.extdata_id, "00000000000016E1") == 0);
    CHECK(candidate.extdata_id_size == 16U && candidate.title_id_size == 0U);
    CHECK(candidate.file_count == 2U);
    CHECK(romx_save_catalog_get_file(catalog, 0U, 0U,
        &(romx_save_file_info_t)ROMX_SAVE_FILE_INFO_INIT, &error) == ROMX_OK);
    romx_save_catalog_close(catalog);
    catalog = NULL;

    CHECK(join_path(savedata, sizeof(savedata), fixture, "savedatafiler"));
    CHECK(join_path(timestamp, sizeof(timestamp), savedata,
        "20161015140648"));
    CHECK(join_path(filer_low, sizeof(filer_low), timestamp, "000016e1"));
    CHECK(make_dir(savedata));
    CHECK(make_dir(timestamp));
    CHECK(make_dir(filer_low));
    CHECK(join_path(path, sizeof(path), timestamp, "export.log"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(join_path(path, sizeof(path), timestamp, "000016e1.dat"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(join_path(path, sizeof(path), timestamp, "000016e1_.dat"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(join_path(path, sizeof(path), filer_low, "mhr_game0.sav"));
    CHECK(write_bytes(path, bytes, sizeof(bytes)));
    CHECK(romx_save_catalog_open_path(timestamp, &options, &catalog, &error) ==
        ROMX_OK);
    CHECK(romx_save_catalog_get_candidate_count(catalog, &count, &error) ==
        ROMX_OK && count == 1U);
    candidate = (romx_save_candidate_info_t)ROMX_SAVE_CANDIDATE_INFO_INIT;
    CHECK(romx_save_catalog_get_candidate(catalog, 0U, &candidate, &error) ==
        ROMX_OK);
    CHECK(candidate.source_format == ROMX_SAVE_SOURCE_3DS_SAVEDATAFILER);
    CHECK(candidate.scope == ROMX_SAVE_SCOPE_3DS_EXTDATA);
    CHECK(strcmp(candidate.extdata_id, "00000000000016E1") == 0);
    CHECK(strcmp(candidate.key, "20161015140648") == 0);
    CHECK(candidate.file_count == 4U);

    CHECK(join_path(romx_path, sizeof(romx_path), fixture,
        "3ds-extdata.romx"));
    CHECK(create_test_romx(romx_path));
    CHECK(romx_save_catalog_write_candidate(catalog, 0U, romx_path,
        "mhr-extdata", &bundle_options, &write_options, &object,
        &error) == ROMX_OK);
    romx_save_catalog_close(catalog);
    catalog = NULL;
    CHECK(romx_reader_open_path(romx_path, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_mutable_bundle_open(reader, ROMX_MUTABLE_NAMESPACE_SAVE,
        "mhr-extdata", NULL, &bundle, &error) == ROMX_OK);
    CHECK(romx_mutable_bundle_get_entry_count(bundle, &count, &error) ==
        ROMX_OK && count == 4U);
    CHECK(romx_mutable_bundle_get_save_layout(bundle, &layout, &error) ==
        ROMX_OK);
    CHECK(layout.scope == ROMX_SAVE_SCOPE_3DS_EXTDATA &&
        (layout.flags & ROMX_MUTABLE_SAVE_LAYOUT_STRICT_EXTDATA) != 0U &&
        strcmp(layout.extdata_id, "00000000000016E1") == 0);
    {
        uint32_t slot_count;
        romx_mutable_save_slot_info_t slot = ROMX_MUTABLE_SAVE_SLOT_INFO_INIT;
        int saw_data = 0;
        int saw_sidecar = 0;
        int saw_underscore = 0;
        int saw_log = 0;
        CHECK(romx_mutable_bundle_get_save_slot_count(bundle, &slot_count,
            &error) == ROMX_OK && slot_count == 1U);
        CHECK(romx_mutable_bundle_get_save_slot(bundle, 0U, &slot, &error) ==
            ROMX_OK && strcmp(slot.key, "000016E1") == 0 &&
            slot.entry_count == 4U);
        for (uint32_t index = 0U; index < count; ++index) {
            bundle_entry = (romx_mutable_bundle_entry_info_t)
                ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
            CHECK(romx_mutable_bundle_get_entry(bundle, index, &bundle_entry,
                &error) == ROMX_OK);
            if (strcmp(bundle_entry.path, "000016e1/mhr_game0.sav") == 0)
                saw_data = 1;
            else if (strcmp(bundle_entry.path, "000016e1.dat") == 0)
                saw_sidecar = 1;
            else if (strcmp(bundle_entry.path, "000016e1_.dat") == 0)
                saw_underscore = 1;
            else if (strcmp(bundle_entry.path, "export.log") == 0)
                saw_log = 1;
        }
        CHECK(saw_data && saw_sidecar && saw_underscore && saw_log);
    }
    romx_mutable_bundle_close(bundle);
    romx_reader_close(reader);
    bundle = NULL;
    reader = NULL;

    CHECK(romx_save_catalog_open_path(low, &options, &catalog, &error) ==
        ROMX_OK);
    CHECK(romx_save_catalog_write_candidate(catalog, 0U, romx_path,
        "native-extdata", NULL, NULL, NULL, &error) == ROMX_OK);
    romx_save_catalog_close(catalog);
    catalog = NULL;
    CHECK(romx_reader_open_path(romx_path, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_mutable_bundle_open(reader, ROMX_MUTABLE_NAMESPACE_SAVE,
        "native-extdata", NULL, &bundle, &error) == ROMX_OK);
    CHECK(romx_mutable_bundle_get_save_layout(bundle, &layout, &error) ==
        ROMX_OK && layout.scope == ROMX_SAVE_SCOPE_3DS_EXTDATA &&
        (layout.flags & ROMX_MUTABLE_SAVE_LAYOUT_STRICT_EXTDATA) == 0U &&
        strcmp(layout.extdata_id, "00000000000016E1") == 0);
    CHECK(romx_mutable_bundle_get_entry(bundle, 0U, &bundle_entry, &error) ==
        ROMX_OK && strcmp(bundle_entry.path,
            "extdata/00000000/000016e1/user/mhr_game0.sav") == 0);
    romx_mutable_bundle_close(bundle);
    romx_reader_close(reader);

    /* A hint and an eight-digit folder name cannot invent a missing
     * SaveDataFiler sidecar set or classify a Title Save as ExtData. */
    options.flags = ROMX_SAVE_SCAN_TREAT_ROOT_AS_SAVE;
    options.source_format_hint = ROMX_SAVE_SOURCE_3DS_SAVEDATAFILER;
    CHECK(romx_save_catalog_open_path(filer_low, &options, &catalog, &error) ==
        ROMX_OK);
    CHECK(romx_save_catalog_get_candidate(catalog, 0U, &candidate, &error) ==
        ROMX_OK && candidate.scope == ROMX_SAVE_SCOPE_3DS_TITLE &&
        candidate.extdata_id_size == 0U);
    romx_save_catalog_close(catalog);
    return 1;
}

static int test_psp_marker_grouping(const char *root)
{
    char directory_root[1024];
    char save_directory[1024];
    char path[1024];
    static const uint8_t marker[] = {
        0U, 'P', 'S', 'F', 1U, 1U, 0U, 0U,
        36U, 0U, 0U, 0U, 44U, 0U, 0U, 0U, 1U, 0U, 0U, 0U,
        0U, 0U, 4U, 2U, 10U, 0U, 0U, 0U,
        10U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        'D', 'I', 'S', 'C', '_', 'I', 'D', 0U,
        'U', 'L', 'U', 'S', '1', '2', '3', '4', '5', 0U
    };
    static const uint8_t data[] = { 0x30U, 0x31U };
    romx_save_scan_options_t options = ROMX_SAVE_SCAN_OPTIONS_INIT;
    romx_save_catalog_t *catalog = NULL;
    romx_error_t error;
    uint32_t count;

    CHECK(join_path(directory_root, sizeof(directory_root), root,
        "psp-saves"));
    CHECK(join_path(save_directory, sizeof(save_directory), directory_root,
        "ULUS12345"));
    CHECK(make_dir(directory_root));
    CHECK(make_dir(save_directory));
    CHECK(join_path(path, sizeof(path), save_directory, "PARAM.SFO"));
    CHECK(write_bytes(path, marker, sizeof(marker)));
    CHECK(join_path(path, sizeof(path), save_directory, "DATA.BIN"));
    CHECK(write_bytes(path, data, sizeof(data)));
    options.platform_id = ROMX_PLATFORM_PSP;
    options.format_id = ROMX_FORMAT_ISO;
    CHECK(romx_save_catalog_open_path(directory_root, &options, &catalog,
        &error) == ROMX_OK);
    CHECK(romx_save_catalog_get_candidate_count(catalog, &count, &error) ==
        ROMX_OK && count == 1U);
    CHECK(check_candidate(catalog, 0U, "ULUS12345", 2U, 1,
        ROMX_SAVE_SOURCE_PSP_SAVEDATA));
    {
        romx_reader_t *reader = NULL;
        romx_mutable_bundle_t *bundle = NULL;
        romx_mutable_save_slot_info_t slot = ROMX_MUTABLE_SAVE_SLOT_INFO_INIT;
        CHECK(join_path(path, sizeof(path), root, "psp-catalog.romx"));
        /* PBP selects the PSP profile even with an unknown platform. */
        CHECK(create_platform_romx(path, UINT16_C(0x007f), ROMX_FORMAT_PBP));
        CHECK(romx_save_catalog_write_candidate(catalog, 0U, path,
            "renamed-save", NULL, NULL, NULL, &error) == ROMX_OK);
        CHECK(romx_reader_open_path(path, NULL, &reader, &error) == ROMX_OK);
        CHECK(romx_mutable_bundle_open(reader, ROMX_MUTABLE_NAMESPACE_SAVE,
            "renamed-save", NULL, &bundle, &error) == ROMX_OK);
        CHECK(romx_mutable_bundle_get_save_slot_count(bundle, &count,
            &error) == ROMX_OK && count == 1U);
        CHECK(romx_mutable_bundle_get_save_slot(bundle, 0U, &slot, &error) ==
            ROMX_OK && slot.entry_count == 2U && slot.is_directory == 1U &&
            strcmp(slot.key, "ULUS12345") == 0);
        romx_mutable_bundle_close(bundle);
        romx_reader_close(reader);
    }
    romx_save_catalog_close(catalog);
    return 1;
}

static int test_write_and_3ds_slots(const char *root)
{
    char directory_root[1024];
    char save_directory[1024];
    char path[1024];
    char romx_path[1024];
    static const uint8_t first[] = { 0x40U, 0x41U };
    static const uint8_t second[] = { 0x42U, 0x43U, 0x44U };
    romx_save_scan_options_t scan_options = ROMX_SAVE_SCAN_OPTIONS_INIT;
    romx_mutable_bundle_options_t bundle_options =
        ROMX_MUTABLE_BUNDLE_OPTIONS_INIT;
    romx_mutable_write_options_t write_options = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
    romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
    romx_save_catalog_t *catalog = NULL;
    romx_reader_t *reader = NULL;
    romx_mutable_bundle_t *bundle = NULL;
    romx_error_t error;
    uint32_t count;

    CHECK(join_path(directory_root, sizeof(directory_root), root,
        "write-source"));
    CHECK(join_path(save_directory, sizeof(save_directory), directory_root,
        "profile-1"));
    CHECK(join_path(romx_path, sizeof(romx_path), root, "save-manager.romx"));
    CHECK(make_dir(directory_root));
    CHECK(make_dir(save_directory));
    CHECK(join_path(path, sizeof(path), save_directory, "save00.bin"));
    CHECK(write_bytes(path, first, sizeof(first)));
    CHECK(join_path(path, sizeof(path), save_directory, "system.dat"));
    CHECK(write_bytes(path, second, sizeof(second)));
    CHECK(join_path(path, sizeof(path), save_directory, "custom.txt"));
    CHECK(write_bytes(path, second, sizeof(second)));
    CHECK(join_path(path, sizeof(path), save_directory, "preview.png"));
    CHECK(write_bytes(path, first, sizeof(first)));
    CHECK(join_path(path, sizeof(path), save_directory, ".DS_Store"));
    CHECK(write_bytes(path, first, sizeof(first)));
    CHECK(create_test_romx(romx_path));
    scan_options.platform_id = ROMX_PLATFORM_NINTENDO_3DS;
    scan_options.format_id = ROMX_FORMAT_N3DS;
    CHECK(romx_save_catalog_open_path(directory_root, &scan_options, &catalog,
        &error) == ROMX_OK);
    CHECK(romx_save_catalog_get_candidate_count(catalog, &count, &error) ==
        ROMX_OK && count == 1U);
    CHECK(romx_save_catalog_write_candidate(catalog, 0U, romx_path,
        NULL, &bundle_options, &write_options, &object, &error) == ROMX_OK);
    romx_save_catalog_close(catalog);
    CHECK(romx_reader_open_path(romx_path, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_mutable_bundle_open(reader, ROMX_MUTABLE_NAMESPACE_SAVE,
        "profile-1", NULL, &bundle, &error) == ROMX_OK);
    CHECK(romx_mutable_bundle_get_entry_count(bundle, &count, &error) ==
        ROMX_OK && count == 4U);
    {
        uint32_t index;
        for (index = 0U; index < count; ++index) {
            romx_mutable_bundle_entry_info_t entry =
                ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
            uint8_t restored[8];
            uint64_t read = 0U;
            CHECK(romx_mutable_bundle_get_entry(bundle, index, &entry,
                &error) == ROMX_OK);
            CHECK(strcmp(entry.path, ".DS_Store") != 0);
            CHECK(romx_mutable_bundle_read_entry(bundle, index, 0U,
                restored, sizeof(restored), &read, &error) == ROMX_OK);
            CHECK(read == entry.data_size);
            CHECK((read == sizeof(first) &&
                memcmp(restored, first, sizeof(first)) == 0) ||
                (read == sizeof(second) &&
                memcmp(restored, second, sizeof(second)) == 0));
        }
    }
    romx_mutable_bundle_close(bundle);
    romx_reader_close(reader);
    return 1;
}

static void cleanup_fixture(const char *root)
{
    char path[1024];
    const char *files[] = {
        "3ds-directories/save-1/save00.bin",
        "3ds-directories/save-1/system.dat",
        "3ds-directories/save-2/Data0",
        "3ds-single-files/slot-1.sav",
        "3ds-single-files/slot-2.sav",
        "0004000000078B00.sav",
        "3ds-special/game-a/20150101010101/export.log",
        "3ds-special/game-a/20150101010101/000015d8.dat",
        "3ds-special/game-a/20150101010101/000015d8_.dat",
        "3ds-special/game-a/20150101010101/000015d8/Data0",
        "3ds-special/game-a/20150101010101/000015d8/Data1",
        "3ds-special/citra/title/0004000000030000/data/00000001/data.dat",
        "3ds-special/azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/00040000/00040001/data/00000001/savedata.dat",
        "3ds-special/gateway-game/0004000000078B00.sav",
        "3ds-special/readme.txt",
        "3ds-special/download.cia",
        "3ds-special/dlc/content.cia",
        "3ds-extdata/citra/extdata/00000000/000016e1/user/mhr_game0.sav",
        "3ds-extdata/citra/extdata/00000000/000016e1/user/mhr_sys.sav",
        "3ds-extdata/savedatafiler/20161015140648/export.log",
        "3ds-extdata/savedatafiler/20161015140648/000016e1.dat",
        "3ds-extdata/savedatafiler/20161015140648/000016e1_.dat",
        "3ds-extdata/savedatafiler/20161015140648/000016e1/mhr_game0.sav",
        "3ds-extdata/3ds-extdata.romx",
        "3ds-citra-flat/saveData.bin",
        "3ds-citra-flat.romx",
        "psp-saves/ULUS12345/PARAM.SFO",
        "psp-saves/ULUS12345/DATA.BIN",
        "write-source/profile-1/save00.bin",
        "write-source/profile-1/system.dat",
        "write-source/profile-1/custom.txt",
        "write-source/profile-1/preview.png",
        "write-source/profile-1/.DS_Store",
        "psp-catalog.romx",
        "save-manager.romx"
    };
    const char *directories[] = {
        "3ds-directories/save-1", "3ds-directories/save-2",
        "3ds-directories", "3ds-single-files",
        "3ds-special/game-a/20150101010101/000015d8",
        "3ds-special/game-a/20150101010101",
        "3ds-special/game-a",
        "3ds-special/citra/title/0004000000030000/data/00000001",
        "3ds-special/citra/title/0004000000030000/data",
        "3ds-special/citra/title/0004000000030000",
        "3ds-special/citra/title", "3ds-special/citra",
        "3ds-special/azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/00040000/00040001/data/00000001",
        "3ds-special/azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/00040000/00040001/data",
        "3ds-special/azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/00040000/00040001",
        "3ds-special/azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/00040000",
        "3ds-special/azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title",
        "3ds-special/azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000",
        "3ds-special/azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000",
        "3ds-special/azahar/sdmc/Nintendo 3DS",
        "3ds-special/azahar/sdmc", "3ds-special/azahar",
        "3ds-special/gateway-game", "3ds-special/dlc", "3ds-special",
        "3ds-citra-flat",
        "3ds-extdata/citra/extdata/00000000/000016e1/user",
        "3ds-extdata/citra/extdata/00000000/000016e1",
        "3ds-extdata/citra/extdata/00000000",
        "3ds-extdata/citra/extdata", "3ds-extdata/citra",
        "3ds-extdata/savedatafiler/20161015140648/000016e1",
        "3ds-extdata/savedatafiler/20161015140648",
        "3ds-extdata/savedatafiler", "3ds-extdata",
        "psp-saves/ULUS12345",
        "psp-saves", "write-source/profile-1", "write-source"
    };
    size_t index;
    for (index = 0U; index < sizeof(files) / sizeof(files[0]); ++index) {
        if (join_path(path, sizeof(path), root, files[index]))
            (void)remove_file(path);
    }
    for (index = 0U; index < sizeof(directories) / sizeof(directories[0]);
         ++index) {
        if (join_path(path, sizeof(path), root, directories[index]))
            (void)remove_directory(path);
    }
    (void)remove_directory(root);
}

int main(int argc, char **argv)
{
    char root[1024];
    int written;
    if (argc != 2) {
        fprintf(stderr, "usage: %s <temporary-directory>\n", argv[0]);
        return 2;
    }
    written = snprintf(root, sizeof(root), "%s/romx-save-manager-%ld",
        argv[1], (long)get_process_id());
    if (written < 0 || (size_t)written >= sizeof(root) || !make_dir(root)) {
        fprintf(stderr, "failed to create test directory\n");
        return 1;
    }
    if (!test_3ds_directory_grouping(root) ||
        !test_3ds_single_files(root) ||
        !test_gateway_identity(root) ||
        !test_3ds_special_directories(root) ||
        !test_3ds_flat_citra_directory(root) ||
        !test_3ds_extdata_scope_and_write(root) ||
        !test_psp_marker_grouping(root) ||
        !test_write_and_3ds_slots(root)) {
        cleanup_fixture(root);
        return 1;
    }
    cleanup_fixture(root);
    puts("save manager tests passed");
    return 0;
}
