#include "RomxSavePaths.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace
{
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
        auto component = relative.begin();
        if (component == relative.end())
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
        for (; component != relative.end(); ++component)
            mapped /= *component;
        return mapped;
    };
}

SavePathMapper makeThreeDsSaveOutputMapper(
    std::string titleId,
    romx_mutable_save_layout_info_t saveLayout)
{
    const std::string normalizedTitleId = normalizeHexId(std::move(titleId));
    const bool strictExtdata = saveLayout.scope == ROMX_SAVE_SCOPE_3DS_EXTDATA &&
        (saveLayout.flags & ROMX_MUTABLE_SAVE_LAYOUT_HAS_EXTDATA_ID) != 0U &&
        (saveLayout.flags & ROMX_MUTABLE_SAVE_LAYOUT_STRICT_EXTDATA) != 0U &&
        saveLayout.extdata_id_size == 16U;
    const bool canonicalExtdata = saveLayout.scope == ROMX_SAVE_SCOPE_3DS_EXTDATA &&
        (saveLayout.flags & ROMX_MUTABLE_SAVE_LAYOUT_STRICT_EXTDATA) == 0U;

    std::string extdataId;
    if (strictExtdata || canonicalExtdata)
    {
        extdataId.assign(saveLayout.extdata_id,
                         std::min<std::size_t>(saveLayout.extdata_id_size, 16U));
        extdataId = normalizeHexId(std::move(extdataId));
    }

    const std::filesystem::path titlePrefix = normalizedTitleId.empty()
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

    return [titlePrefix, extdataPrefix, extdataLow, strictExtdata, canonicalExtdata](
               const std::filesystem::path& relative, uint32_t /*index*/,
               uint32_t /*count*/) -> std::optional<std::filesystem::path> {
        if (relative.empty())
            return std::nullopt;

        auto first = relative.begin();
        if (strictExtdata)
        {
            // SaveDataFiler metadata is deliberately retained in ROMX but is
            // not released into the core's user directory.
            if (!relative.has_parent_path() ||
                extdataLow.empty() || !componentEquals(*first, extdataLow))
                return std::nullopt;
            auto component = first;
            ++component;
            return appendPathComponents(extdataPrefix, component, relative.end());
        }

        if (canonicalExtdata)
        {
            if (!componentEquals(*first, "extdata"))
                return std::nullopt;
            return relative;
        }

        if (titlePrefix.empty())
            return std::nullopt;
        if (componentEquals(*first, "00000001"))
            ++first;
        return appendPathComponents(titlePrefix, first, relative.end());
    };
}

std::filesystem::path threeDsSaveTransactionDirectory(
    std::string titleId,
    romx_mutable_save_layout_info_t saveLayout)
{
    const std::string normalizedTitleId = normalizeHexId(std::move(titleId));
    const bool extdata = saveLayout.scope == ROMX_SAVE_SCOPE_3DS_EXTDATA;
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
