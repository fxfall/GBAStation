#include "RomxSavePaths.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <string_view>
#include <utility>

namespace
{
namespace fs = std::filesystem;

void setSafePathError(std::string* error, const std::string& reason,
                      const std::error_code& filesystemError = {})
{
    if (error == nullptr)
        return;
    *error = "reason=" + reason +
        "\nfilesystem_code=" + std::to_string(filesystemError.value()) +
        "\nfilesystem_message=" +
        (filesystemError ? filesystemError.message() : "none");
}

bool pathComponentIs(const fs::path& component, const char* value)
{
    return component == fs::path(value);
}

bool hasUnsafeRelativeSyntax(const fs::path& relative, std::string* error)
{
    if (relative.empty())
    {
        setSafePathError(error, "relative path is empty");
        return false;
    }

    const std::string native = relative.string();
    const std::string generic = relative.generic_string();
    if (native.find('\0') != std::string::npos)
    {
        setSafePathError(error, "relative path contains NUL");
        return false;
    }
#ifndef _WIN32
    if (native.find('\\') != std::string::npos)
    {
        setSafePathError(error, "relative path contains backslash");
        return false;
    }
#endif
    if (generic.empty() || generic.front() == '/' || generic.back() == '/' ||
        generic.find("//") != std::string::npos)
    {
        setSafePathError(error, "relative path contains an empty component");
        return false;
    }
    if (relative.has_root_name() || relative.has_root_directory() ||
        relative.is_absolute())
    {
        setSafePathError(error, "relative path has a root or is absolute");
        return false;
    }

    for (const auto& component : relative)
    {
        const std::string value = component.string();
        if (value.empty())
        {
            setSafePathError(error, "relative path contains an empty component");
            return false;
        }
        if (pathComponentIs(component, ".") || pathComponentIs(component, ".."))
        {
            setSafePathError(error, "relative path contains dot traversal");
            return false;
        }
        if (value.find(':') != std::string::npos)
        {
            setSafePathError(error, "relative path component contains ':'");
            return false;
        }
    }
    return true;
}

bool existingStatus(const fs::path& path, fs::file_status& status,
                    std::error_code& error)
{
    error.clear();
    status = fs::symlink_status(path, error);
    if (error == std::make_error_code(std::errc::no_such_file_or_directory))
    {
        error.clear();
        return false;
    }
    if (error)
        return false;
    return fs::exists(status);
}

bool inspectPathComponents(const fs::path& trustedRoot,
                           const fs::path& normalizedRelative,
                           std::string* error)
{
    fs::path current = trustedRoot;
    std::error_code filesystemError;
    bool parentMissing = false;

    fs::file_status rootStatus;
    if (!existingStatus(current, rootStatus, filesystemError))
    {
        if (filesystemError)
        {
            setSafePathError(error, "cannot inspect trusted root",
                             filesystemError);
            return false;
        }
        parentMissing = true;
    }
    else if (fs::is_symlink(rootStatus))
    {
        setSafePathError(error, "trusted root is a symbolic link");
        return false;
    }
    else if (!normalizedRelative.empty() && !fs::is_directory(rootStatus))
    {
        setSafePathError(error, "trusted root is not a directory");
        return false;
    }

    auto inspect = [&](const fs::path& component, bool hasFollowing) {
        if (parentMissing)
            return true;
        current /= component;

        fs::file_status status;
        if (!existingStatus(current, status, filesystemError))
        {
            if (filesystemError)
            {
                setSafePathError(error, "cannot inspect path component",
                                 filesystemError);
                return false;
            }
            // A missing component and everything below it will be created by
            // the caller. There is no existing symlink left to inspect.
            parentMissing = true;
            return true;
        }
        if (fs::is_symlink(status))
        {
            setSafePathError(error, "path component is a symbolic link");
            return false;
        }
        if (hasFollowing && !fs::is_directory(status))
        {
            setSafePathError(error, "path component is not a directory");
            return false;
        }
        return true;
    };

    auto relativeComponent = normalizedRelative.begin();
    while (relativeComponent != normalizedRelative.end())
    {
        auto next = relativeComponent;
        ++next;
        if (!inspect(*relativeComponent, next != normalizedRelative.end()))
            return false;
        relativeComponent = next;
    }
    return true;
}

std::string normalizeHexId(std::string value)
{
    if (value.size() != 16U)
        return {};
    for (char& character : value)
    {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (std::isxdigit(byte) == 0)
            return {};
        character = static_cast<char>(std::tolower(byte));
    }
    return value;
}

void setMappingError(std::string* error, const std::string& reason)
{
    if (error != nullptr && error->empty())
        setSafePathError(error, reason);
}

bool componentEquals(const std::filesystem::path& component,
                     std::string_view expected)
{
    const std::string value = component.string();
    if (value.size() != expected.size())
        return false;
    return std::equal(value.begin(), value.end(), expected.begin(), expected.end(),
                      [](char left, char right) {
                          return std::tolower(static_cast<unsigned char>(left)) ==
                              std::tolower(static_cast<unsigned char>(right));
                      });
}

std::filesystem::path appendPathComponents(
    std::filesystem::path output,
    std::filesystem::path::const_iterator first,
    std::filesystem::path::const_iterator last)
{
    for (; first != last; ++first)
        output /= *first;
    return output;
}

std::filesystem::path threeDsSaveTransactionDirectoryForIds(
    const std::string& normalizedTitleId,
    const std::string& extdataId,
    bool extdata)
{
    if (extdata)
    {
        if (extdataId.size() != 16U)
            return {};
        return std::filesystem::path("extdata") /
            extdataId.substr(0U, 8U) /
            extdataId.substr(8U, 8U) / "user";
    }
    if (normalizedTitleId.empty())
        return {};
    return std::filesystem::path("title") /
        normalizedTitleId.substr(0U, 8U) /
        normalizedTitleId.substr(8U, 8U) /
        "data" / "00000001";
}
}

namespace beiklive::romx
{
bool normalizeSafeMutableRelativePath(const std::filesystem::path& relative,
                                      std::filesystem::path& normalized,
                                      std::string* error)
{
    normalized.clear();
    if (!hasUnsafeRelativeSyntax(relative, error))
        return false;

    const std::filesystem::path candidate = relative.lexically_normal();
    if (!hasUnsafeRelativeSyntax(candidate, error))
        return false;
    normalized = candidate;
    return true;
}

bool resolveSafeMutablePath(const std::filesystem::path& trustedRoot,
                            const std::filesystem::path& relative,
                            std::filesystem::path& output,
                            std::string* error)
{
    output.clear();
    if (trustedRoot.empty())
    {
        setSafePathError(error, "trusted root is empty");
        return false;
    }

    std::filesystem::path normalized;
    if (!normalizeSafeMutableRelativePath(relative, normalized, error))
        return false;
    if (!inspectPathComponents(trustedRoot, normalized, error))
        return false;

    // Do not canonicalize here. In particular, sdmc:/... is a Switch mount
    // prefix rather than a desktop filesystem root.
    output = trustedRoot / normalized;
    return true;
}

uint64_t expandedMutableBundleCapacity(uint64_t measuredSize,
                                       bool allowGrowth)
{
    if (!allowGrowth || measuredSize == 0U)
        return measuredSize;

    constexpr uint64_t oneMiB = UINT64_C(1024) * UINT64_C(1024);
    const uint64_t withReserve = measuredSize > UINT64_MAX - oneMiB
        ? UINT64_MAX : measuredSize + oneMiB;
    const uint64_t doubled = measuredSize > UINT64_MAX / 2U
        ? UINT64_MAX : measuredSize * 2U;
    return std::max(withReserve, doubled);
}

SavePathMapper makePspSaveOutputMapper(std::string& targetDirectory,
                                       std::string requestedDirectory)
{
    return [&targetDirectory,
            selectedSourceDirectory = std::move(requestedDirectory)](
               const std::filesystem::path& relative, uint32_t /*index*/,
               uint32_t /*count*/) mutable -> std::optional<std::filesystem::path> {
        std::filesystem::path normalizedRelative;
        if (!normalizeSafeMutableRelativePath(relative, normalizedRelative))
            return std::nullopt;
        auto component = normalizedRelative.begin();
        if (component == normalizedRelative.end())
            return std::nullopt;

        const std::string sourceDirectory = component->string();
        if (selectedSourceDirectory.empty())
            selectedSourceDirectory = sourceDirectory;
        if (sourceDirectory != selectedSourceDirectory)
            return std::nullopt;
        if (targetDirectory.empty())
            targetDirectory = sourceDirectory;

        std::filesystem::path mapped = targetDirectory;
        ++component;
        for (; component != normalizedRelative.end(); ++component)
            mapped /= *component;
        return mapped;
    };
}

SavePathMapper makeThreeDsSaveOutputMapper(
    std::string titleId,
    romx_mutable_save_layout_info_t saveLayout,
    std::string* mappingError)
{
    const std::string normalizedTitleId = normalizeHexId(std::move(titleId));
    const bool titleLayout = saveLayout.scope == ROMX_SAVE_SCOPE_3DS_TITLE &&
        !normalizedTitleId.empty();
    const bool strictExtdata = saveLayout.scope == ROMX_SAVE_SCOPE_3DS_EXTDATA &&
        (saveLayout.flags & ROMX_MUTABLE_SAVE_LAYOUT_HAS_EXTDATA_ID) != 0U &&
        (saveLayout.flags & ROMX_MUTABLE_SAVE_LAYOUT_STRICT_EXTDATA) != 0U &&
        saveLayout.extdata_id_size == 16U;
    const bool canonicalExtdata = saveLayout.scope == ROMX_SAVE_SCOPE_3DS_EXTDATA &&
        (saveLayout.flags & ROMX_MUTABLE_SAVE_LAYOUT_HAS_EXTDATA_ID) != 0U &&
        (saveLayout.flags & ROMX_MUTABLE_SAVE_LAYOUT_STRICT_EXTDATA) == 0U &&
        saveLayout.extdata_id_size == 16U;

    std::string extdataId;
    if (strictExtdata || canonicalExtdata)
    {
        extdataId.assign(saveLayout.extdata_id,
                         std::min<std::size_t>(saveLayout.extdata_id_size, 16U));
        extdataId = normalizeHexId(std::move(extdataId));
    }

    const std::filesystem::path titlePrefix = !titleLayout
        ? std::filesystem::path{}
        : std::filesystem::path("title") /
            normalizedTitleId.substr(0U, 8U) /
            normalizedTitleId.substr(8U, 8U) /
            "data" / "00000001";
    const std::string extdataLow = extdataId.size() == 16U
        ? extdataId.substr(8U, 8U) : std::string{};
    const std::filesystem::path extdataPrefix = extdataId.empty()
        ? std::filesystem::path{}
        : std::filesystem::path("extdata") /
            extdataId.substr(0U, 8U) /
            extdataId.substr(8U, 8U) / "user";

    return [titlePrefix, extdataPrefix, extdataId, extdataLow, strictExtdata,
            canonicalExtdata, titleLayout, mappingError](
               const std::filesystem::path& relative, uint32_t /*index*/,
               uint32_t /*count*/) -> std::optional<std::filesystem::path> {
        std::filesystem::path normalizedRelative;
        if (!normalizeSafeMutableRelativePath(relative, normalizedRelative))
            return std::nullopt;

        auto first = normalizedRelative.begin();
        if (strictExtdata)
        {
            // SaveDataFiler metadata is deliberately retained in ROMX but is
            // not released into the core's user directory.
            if (!normalizedRelative.has_parent_path() ||
                extdataLow.empty() || !componentEquals(*first, extdataLow))
                return std::nullopt;
            auto component = first;
            ++component;
            return appendPathComponents(extdataPrefix, component,
                                        normalizedRelative.end());
        }

        if (canonicalExtdata)
        {
            auto component = first;
            if (!componentEquals(*component, "extdata"))
            {
                setMappingError(mappingError,
                                "canonical ExtData path does not start with extdata");
                return std::nullopt;
            }
            ++component;
            if (component == normalizedRelative.end() ||
                !componentEquals(*component, extdataId.substr(0U, 8U)))
            {
                setMappingError(mappingError,
                                "canonical ExtData high ID does not match layout extdata_id");
                return std::nullopt;
            }
            ++component;
            if (component == normalizedRelative.end() ||
                !componentEquals(*component, extdataId.substr(8U, 8U)))
            {
                setMappingError(mappingError,
                                "canonical ExtData low ID does not match layout extdata_id");
                return std::nullopt;
            }
            ++component;
            if (component == normalizedRelative.end())
            {
                setMappingError(mappingError,
                                "canonical ExtData path does not name a file");
                return std::nullopt;
            }
            // Citra/Azahar keeps ExtData metadata such as `icon` and
            // `metadata` beside `user`.  They are not core save files and
            // must not escape the user-only restore transaction.  Ignore
            // those auxiliary entries after validating the exact ExtData
            // identity; malformed paths outside that identity still fail.
            if (!componentEquals(*component, "user"))
            {
                if (componentEquals(*component, "icon") ||
                    componentEquals(*component, "metadata"))
                    return std::nullopt;
                setMappingError(mappingError,
                                "canonical ExtData path is missing the user directory");
                return std::nullopt;
            }
            ++component;
            if (component == normalizedRelative.end())
            {
                setMappingError(mappingError,
                                "canonical ExtData path does not name a file");
                return std::nullopt;
            }
            return appendPathComponents(extdataPrefix, component,
                                        normalizedRelative.end());
        }

        if (!titleLayout)
        {
            setMappingError(mappingError, "unknown 3DS SAVE layout");
            return std::nullopt;
        }
        if (componentEquals(*first, "00000001"))
            ++first;
        if (first == normalizedRelative.end())
        {
            setMappingError(mappingError, "3DS Title SAVE path does not name a file");
            return std::nullopt;
        }
        return appendPathComponents(titlePrefix, first, normalizedRelative.end());
    };
}

std::filesystem::path threeDsSaveTransactionDirectory(
    std::string titleId,
    romx_mutable_save_layout_info_t saveLayout)
{
    const std::string normalizedTitleId = normalizeHexId(std::move(titleId));
    const bool title = saveLayout.scope == ROMX_SAVE_SCOPE_3DS_TITLE;
    const bool extdata = saveLayout.scope == ROMX_SAVE_SCOPE_3DS_EXTDATA;
    if (!title && !extdata)
        return {};
    std::string extdataId;
    if (extdata &&
        (saveLayout.flags & ROMX_MUTABLE_SAVE_LAYOUT_HAS_EXTDATA_ID) != 0U &&
        saveLayout.extdata_id_size == 16U)
    {
        extdataId.assign(saveLayout.extdata_id, 16U);
        extdataId = normalizeHexId(std::move(extdataId));
    }
    return threeDsSaveTransactionDirectoryForIds(
        normalizedTitleId, extdataId, extdata);
}
}
