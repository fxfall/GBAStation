#include "RomxSaveExport.hpp"

#include "RomxError.hpp"
#include "RomxPlatformIdentity.hpp"
#include "RomxSavePaths.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
bool openThreeDsSaveCatalog(const fs::path& source,
                            romx_save_catalog_t** catalog,
                            uint32_t* candidateCount,
                            std::string* error)
{
    if (catalog != nullptr)
        *catalog = nullptr;
    if (candidateCount != nullptr)
        *candidateCount = 0;
    if (source.empty())
    {
        beiklive::romx::assignError(error, "3DS save source path is empty");
        return false;
    }

    romx_save_scan_options_t scanOptions = ROMX_SAVE_SCAN_OPTIONS_INIT;
    scanOptions.platform_id = ROMX_PLATFORM_NINTENDO_3DS;
    scanOptions.format_id = ROMX_FORMAT_N3DS;
    scanOptions.launch_format_id = ROMX_LAUNCH_RAW_SINGLE_FILE;

    romx_error_t err{};
    const romx_result_t openResult = romx_save_catalog_open_path(
        source.string().c_str(), &scanOptions, catalog, &err);
    if (openResult != ROMX_OK)
    {
        beiklive::romx::assignError(
            error, beiklive::romx::errorText("romx_save_catalog_open_path",
                                             err, openResult));
        return false;
    }

    uint32_t count = 0;
    const romx_result_t countResult = romx_save_catalog_get_candidate_count(
        *catalog, &count, &err);
    if (countResult != ROMX_OK)
    {
        romx_save_catalog_close(*catalog);
        *catalog = nullptr;
        beiklive::romx::assignError(
            error, beiklive::romx::errorText(
                "romx_save_catalog_get_candidate_count", err, countResult));
        return false;
    }
    if (candidateCount != nullptr)
        *candidateCount = count;
    return true;
}

std::string copyThreeDsCandidateSourcePath(const romx_save_catalog_t* catalog,
                                            uint32_t candidateIndex,
                                            std::string* error)
{
    romx_error_t err{};
    uint64_t required = 0;
    romx_result_t result = romx_save_catalog_copy_candidate_source_path(
        catalog, candidateIndex, nullptr, 0, &required, &err);
    if (result != ROMX_E_BUFFER_TOO_SMALL && result != ROMX_OK)
    {
        beiklive::romx::assignError(
            error, beiklive::romx::errorText(
                "romx_save_catalog_copy_candidate_source_path", err, result));
        return {};
    }
    if (required == 0 || required > static_cast<uint64_t>(
        std::numeric_limits<std::size_t>::max()))
    {
        beiklive::romx::assignError(error, "3DS save source path size is invalid");
        return {};
    }
    std::vector<char> buffer(static_cast<std::size_t>(required));
    result = romx_save_catalog_copy_candidate_source_path(
        catalog, candidateIndex, buffer.data(), required, &required, &err);
    if (result != ROMX_OK)
    {
        beiklive::romx::assignError(
            error, beiklive::romx::errorText(
                "romx_save_catalog_copy_candidate_source_path", err, result));
        return {};
    }
    return std::string(buffer.data());
}

std::string candidateText(const char* value, std::size_t capacity,
                          uint32_t size)
{
    if (value == nullptr || capacity == 0)
        return {};
    const std::size_t bounded = std::min<std::size_t>(size, capacity - 1U);
    return std::string(value, bounded);
}

bool findMutableObject(const beiklive::GameEntry& entry,
                       romx_mutable_namespace_t objectNamespace,
                       const char* key,
                       romx_mutable_object_info_t* foundObject)
{
    if (entry.path.empty() || key == nullptr || *key == '\0')
        return false;

    romx_reader_t* reader = nullptr;
    romx_error_t error{};
    if (romx_reader_open_path(entry.path.c_str(), nullptr, &reader, &error) !=
        ROMX_OK)
        return false;

    uint32_t count = 0;
    bool found = false;
    if (romx_reader_get_mutable_object_count(reader, &count, &error) == ROMX_OK)
    {
        for (uint32_t index = 0; index < count; ++index)
        {
            romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
            if (romx_reader_get_mutable_object(reader, index, &object, &error) !=
                    ROMX_OK ||
                object.object_namespace != objectNamespace)
                continue;
            const std::size_t keySize = std::min<std::size_t>(
                object.key_size, sizeof(object.key) - 1U);
            if (keySize != std::strlen(key) ||
                std::memcmp(object.key, key, keySize) != 0)
                continue;
            if (foundObject != nullptr)
                *foundObject = object;
            found = true;
            break;
        }
    }
    romx_reader_close(reader);
    return found;
}
}

namespace beiklive::romx
{
bool isSaveStateArtifact(const fs::path& path)
{
    const std::string name = path.filename().string();
    const std::size_t marker = name.rfind(".ss");
    if (marker != std::string::npos && marker + 3U < name.size())
    {
        bool digits = true;
        for (std::size_t index = marker + 3U; index < name.size(); ++index)
            digits = digits && std::isdigit(
                static_cast<unsigned char>(name[index])) != 0;
        if (digits)
            return true;
    }
    const auto endsWith = [&name](const char* suffix) {
        const std::string value(suffix);
        return name.size() >= value.size() &&
               name.compare(name.size() - value.size(), value.size(), value) == 0;
    };
    return endsWith(".playtime") || endsWith(".playtime.tmp");
}

std::vector<GameEntryAdapter::LocalSaveCandidate> listThreeDsSaveCandidates(
    const GameEntry& entry, const fs::path& source, std::string* error)
{
    std::vector<GameEntryAdapter::LocalSaveCandidate> candidates;
    if (!isThreeDs(entry))
    {
        assignError(error, "SAVE catalog is only available for 3DS entries");
        return candidates;
    }

    romx_save_catalog_t* catalog = nullptr;
    uint32_t count = 0;
    if (!openThreeDsSaveCatalog(source, &catalog, &count, error))
        return candidates;
    if (count == 0U)
    {
        romx_save_catalog_close(catalog);
        assignError(error, "未找到可识别的3DS本地存档: " + source.string());
        return candidates;
    }

    candidates.reserve(count);
    for (uint32_t index = 0; index < count; ++index)
    {
        romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
        romx_error_t err{};
        const romx_result_t result = romx_save_catalog_get_candidate(
            catalog, index, &candidate, &err);
        if (result != ROMX_OK)
        {
            assignError(error, errorText("romx_save_catalog_get_candidate",
                                         err, result));
            candidates.clear();
            break;
        }

        GameEntryAdapter::LocalSaveCandidate item;
        item.key = candidateText(candidate.key, sizeof(candidate.key),
                                 candidate.key_size);
        item.displayName = candidateText(
            candidate.display_name, sizeof(candidate.display_name),
            candidate.display_name_size);
        if (item.displayName.empty())
            item.displayName = item.key;
        item.titleId = candidateText(candidate.title_id,
                                     sizeof(candidate.title_id),
                                     candidate.title_id_size);
        item.sourcePath = copyThreeDsCandidateSourcePath(catalog, index, error);
        if (item.sourcePath.empty())
        {
            candidates.clear();
            break;
        }
        item.sourceFormat = candidate.source_format;
        item.grouping = candidate.grouping;
        item.scope = candidate.scope;
        item.extdataId = candidateText(candidate.extdata_id,
                                       sizeof(candidate.extdata_id),
                                       candidate.extdata_id_size);
        item.fileCount = candidate.file_count;
        item.dataSize = candidate.data_size;
        item.isDirectory = (candidate.flags & ROMX_SAVE_CANDIDATE_IS_DIRECTORY) != 0;
        candidates.push_back(std::move(item));
    }
    romx_save_catalog_close(catalog);
    return candidates;
}

bool collectRegularBundleFiles(const fs::path& root,
                               std::vector<std::string>& relativePaths,
                               std::vector<std::string>& sourcePaths)
{
    relativePaths.clear();
    sourcePaths.clear();
    std::error_code ec;
    fs::recursive_directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        const fs::directory_entry& item = *iterator;
        if (item.is_symlink(ec))
        {
            iterator.disable_recursion_pending();
            ec.clear();
            continue;
        }
        if (!item.is_regular_file(ec))
        {
            ec.clear();
            continue;
        }
        if (isSaveStateArtifact(item.path()))
            continue;
        const fs::path relative = fs::relative(item.path(), root, ec);
        if (ec || relative.empty())
        {
            ec.clear();
            continue;
        }
        relativePaths.push_back(relative.generic_string());
        sourcePaths.push_back(item.path().string());
    }
    return !relativePaths.empty() && relativePaths.size() == sourcePaths.size();
}

SyncResult writeBundlePathEntries(
    const GameEntry& entry, romx_mutable_namespace_t objectNamespace,
    const char* key, const std::vector<std::string>& relativePaths,
    const std::vector<std::string>& sourcePaths, std::string* error)
{
    if (key == nullptr || *key == '\0' || relativePaths.empty() ||
        relativePaths.size() != sourcePaths.size() ||
        relativePaths.size() > UINT32_MAX)
    {
        assignError(error, "ROMX mutable export file list is invalid");
        return SyncResult::Failed;
    }

    std::vector<romx_mutable_bundle_path_entry_t> files;
    files.reserve(relativePaths.size());
    for (std::size_t index = 0; index < relativePaths.size(); ++index)
    {
        romx_mutable_bundle_path_entry_t file = ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT;
        file.relative_path = relativePaths[index].c_str();
        file.source_path = sourcePaths[index].c_str();
        files.push_back(file);
    }

    uint64_t measuredSize = 0;
    romx_error_t err{};
    const romx_result_t measureResult = romx_mutable_bundle_measure_path_entries(
        objectNamespace, files.data(), static_cast<uint32_t>(files.size()),
        nullptr, &measuredSize, &err);
    if (measureResult != ROMX_OK)
    {
        assignError(error, errorText(
            "romx_mutable_bundle_measure_path_entries", err, measureResult));
        return SyncResult::Failed;
    }

    romx_mutable_write_options_t options = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
    romx_mutable_object_info_t existingObject = ROMX_MUTABLE_OBJECT_INFO_INIT;
    const bool existing = findMutableObject(entry, objectNamespace, key,
                                             &existingObject);
    const bool allowGrowth = !existing && isPsp(entry) &&
        objectNamespace == ROMX_MUTABLE_NAMESPACE_SAVE &&
        relativePaths.size() > 1U;
    options.data_capacity = existing ? 0U :
        expandedMutableBundleCapacity(measuredSize, allowGrowth);
    romx_mutable_object_info_t written = ROMX_MUTABLE_OBJECT_INFO_INIT;
    romx_result_t result = romx_mutable_bundle_write_path_entries(
        entry.path.c_str(), objectNamespace, key, files.data(),
        static_cast<uint32_t>(files.size()), nullptr, &options, &written, &err);
    if (result == ROMX_E_MUTABLE_NO_SPACE && !existing &&
        options.data_capacity != measuredSize)
    {
        options.data_capacity = measuredSize;
        err = romx_error_t{};
        result = romx_mutable_bundle_write_path_entries(
            entry.path.c_str(), objectNamespace, key, files.data(),
            static_cast<uint32_t>(files.size()), nullptr, &options, &written,
            &err);
    }
    if (result != ROMX_OK)
    {
        std::string message = errorText(
            "romx_mutable_bundle_write_path_entries", err, result);
        if (result == ROMX_E_MUTABLE_NO_SPACE)
        {
            message += " (required=" + std::to_string(measuredSize);
            if (existing)
                message += ", existing_capacity=" +
                    std::to_string(existingObject.data_capacity);
            message += ")";
        }
        assignError(error, message);
        return SyncResult::Failed;
    }
    return SyncResult::Success;
}

SyncResult writeThreeDsSaveCatalog(const fs::path& source,
                                   const fs::path& destination,
                                   const std::string& preferredKey,
                                   uint32_t* writtenCount,
                                   std::string* error)
{
    if (writtenCount != nullptr)
        *writtenCount = 0;
    if (source.empty() || destination.empty())
    {
        assignError(error, "3DS SAVE source or ROMX destination is empty");
        return SyncResult::Failed;
    }
    if (!preferredKey.empty() &&
        !GameEntryAdapter::validateSaveSlotKey(preferredKey, error))
        return SyncResult::Failed;

    romx_save_catalog_t* catalog = nullptr;
    uint32_t count = 0;
    if (!openThreeDsSaveCatalog(source, &catalog, &count, error))
        return SyncResult::Failed;
    if (count == 0U)
    {
        romx_save_catalog_close(catalog);
        assignError(error, "未找到可识别的3DS本地存档: " + source.string());
        return SyncResult::Skipped;
    }

    uint32_t succeeded = 0;
    std::string firstError;
    GameEntry destinationEntry;
    destinationEntry.path = destination.string();
    for (uint32_t index = 0; index < count; ++index)
    {
        romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
        romx_error_t err{};
        const romx_result_t candidateResult = romx_save_catalog_get_candidate(
            catalog, index, &candidate, &err);
        if (candidateResult != ROMX_OK)
        {
            if (firstError.empty())
                firstError = errorText("romx_save_catalog_get_candidate", err,
                                       candidateResult);
            continue;
        }

        const std::string candidateKey = candidateText(
            candidate.key, sizeof(candidate.key), candidate.key_size);
        // Keep the historical `libretro` key when it already exists, but use
        // the catalog's real folder/file key for a new single-save ROMX.
        const bool reusePreferred = preferredKey.empty() ||
            preferredKey != "libretro" ||
            findMutableObject(destinationEntry,
                              ROMX_MUTABLE_NAMESPACE_SAVE,
                              preferredKey.c_str(), nullptr);
        const std::string objectKey = count == 1U && reusePreferred &&
                                      !preferredKey.empty()
            ? preferredKey : candidateKey;
        if (!GameEntryAdapter::validateSaveSlotKey(objectKey, nullptr))
        {
            if (firstError.empty())
                firstError = "3DS SAVE candidate has an invalid object key";
            continue;
        }

        uint64_t measuredSize = 0;
        const romx_result_t measureResult = romx_save_catalog_measure_candidate(
            catalog, index, nullptr, &measuredSize, &err);
        if (measureResult != ROMX_OK)
        {
            if (firstError.empty())
                firstError = errorText("romx_save_catalog_measure_candidate",
                                       err, measureResult);
            continue;
        }

        romx_mutable_write_options_t writeOptions =
            ROMX_MUTABLE_WRITE_OPTIONS_INIT;
        romx_mutable_object_info_t existingObject = ROMX_MUTABLE_OBJECT_INFO_INIT;
        const bool existing = findMutableObject(
            destinationEntry, ROMX_MUTABLE_NAMESPACE_SAVE, objectKey.c_str(),
            &existingObject);
        const bool allowGrowth = !existing && candidate.file_count > 1U;
        writeOptions.data_capacity = existing ? 0U :
            expandedMutableBundleCapacity(measuredSize, allowGrowth);
        romx_mutable_object_info_t written = ROMX_MUTABLE_OBJECT_INFO_INIT;
        romx_result_t result = romx_save_catalog_write_candidate(
            catalog, index, destination.string().c_str(), objectKey.c_str(),
            nullptr, &writeOptions, &written, &err);
        if (result == ROMX_E_MUTABLE_NO_SPACE && !existing &&
            writeOptions.data_capacity != measuredSize)
        {
            writeOptions.data_capacity = measuredSize;
            err = romx_error_t{};
            result = romx_save_catalog_write_candidate(
                catalog, index, destination.string().c_str(), objectKey.c_str(),
                nullptr, &writeOptions, &written, &err);
        }
        if (result != ROMX_OK)
        {
            if (firstError.empty())
            {
                firstError = errorText("romx_save_catalog_write_candidate", err,
                                       result);
                if (result == ROMX_E_MUTABLE_NO_SPACE)
                    firstError += " (required=" + std::to_string(measuredSize) +
                        (existing ? ", existing_capacity=" +
                            std::to_string(existingObject.data_capacity) :
                            std::string()) + ")";
            }
            continue;
        }
        ++succeeded;
    }
    romx_save_catalog_close(catalog);
    if (writtenCount != nullptr)
        *writtenCount = succeeded;
    if (succeeded == count)
        return SyncResult::Success;
    if (succeeded != 0U)
    {
        assignError(error, "仅写入 " + std::to_string(succeeded) + "/" +
                            std::to_string(count) + " 个3DS存档" +
                            (firstError.empty() ? std::string() :
                                "：" + firstError));
        return SyncResult::Failed;
    }
    assignError(error, firstError.empty() ? "没有3DS存档被写入 ROMX" : firstError);
    return SyncResult::Failed;
}
}
