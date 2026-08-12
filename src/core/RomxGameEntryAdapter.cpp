#include "core/RomxGameEntryAdapter.hpp"

#include "core/RomxFrontend.hpp"
#include "core/ThreeDsTitlePaths.hpp"
#include "core/Tools.hpp"
#include "core/common.h"

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace beiklive::romx
{
namespace
{
template <typename T>
void assignIfChanged(T& target, const T& value, bool& changed)
{
    if (target == value)
        return;
    target = value;
    changed = true;
}

std::string defaultTitleForPath(const std::string& path)
{
    const std::string title = beiklive::tools::getFileNameWithoutExtension(
        fs::path(path).filename().string());
    return title.empty() ? "game" : title;
}

bool isNonePlatform(int platform)
{
    return platform == static_cast<int>(beiklive::enums::EmuPlatform::NONE);
}
}

int RomxGameEntryAdapter::detectPlatform(const std::string& path,
                                         int fallbackPlatform,
                                         std::string* error)
{
    if (error)
        error->clear();
    if (!hasSupportedExtension(path))
        return fallbackPlatform;

    const auto info = readInfo(path, error, false);
    if (info && !isNonePlatform(info->platform))
        return info->platform;
    return fallbackPlatform;
}

RomxGameEntryResult RomxGameEntryAdapter::apply(
    GameEntry& entry, const std::string& path,
    const RomxGameEntryOptions& options)
{
    RomxGameEntryResult result;
    result.romxCandidate = hasSupportedExtension(path);

    std::optional<Info> info;
    if (result.romxCandidate)
    {
        info = readInfo(path, &result.error, options.verifyPayload);
        result.romxValid = info.has_value();
    }

    const int fallbackPlatform = options.fallbackPlatform;
    const int detectedPlatform = info && !isNonePlatform(info->platform)
        ? info->platform : fallbackPlatform;
    const std::string fallbackTitle = options.fallbackTitle.empty()
        ? defaultTitleForPath(path) : options.fallbackTitle;
    const bool firstPackedImport = info.has_value() &&
        entry.romxBodySha256.empty() && entry.romxMetadataJson.empty();

    assignIfChanged(entry.path, path, result.changed);
    if (isNonePlatform(entry.platform) ||
        (firstPackedImport && entry.platform != detectedPlatform))
    {
        assignIfChanged(entry.platform, detectedPlatform, result.changed);
    }

    if (entry.platform != static_cast<int>(beiklive::enums::EmuPlatform::NONE))
    {
        if (entry.core.empty())
            assignIfChanged(entry.core, beiklive::GetDefaultCoreId(entry.platform), result.changed);
        assignIfChanged(entry.core,
                        beiklive::NormalizeCoreId(entry.platform, entry.core),
                        result.changed);
    }

    const std::string metadataTitle = info ? info->title : std::string{};
    if (entry.title.empty() || (firstPackedImport && entry.title == fallbackTitle))
    {
        assignIfChanged(entry.title,
                        metadataTitle.empty() ? fallbackTitle : metadataTitle,
                        result.changed);
    }

    if (entry.savePath.empty() &&
        entry.platform != static_cast<int>(beiklive::enums::EmuPlatform::NONE))
    {
        assignIfChanged(entry.savePath,
                        beiklive::tools::defaultGameSavePath(entry.platform, path),
                        result.changed);
    }
    if (options.createSaveDirectory && !entry.savePath.empty())
    {
        std::error_code error;
        fs::create_directories(entry.savePath, error);
        if (error && result.error.empty())
            result.error = error.message();
    }

    if (entry.platform != static_cast<int>(beiklive::enums::EmuPlatform::NONE))
    {
        const auto platform = static_cast<beiklive::enums::EmuPlatform>(entry.platform);
        const std::string defaultLogo = beiklive::tools::getDefaultLogoPath(platform, path);
        const bool defaultCover = entry.logoPath.empty() ||
            (firstPackedImport && entry.logoPath == defaultLogo) ||
            options.forceCover;
        if (entry.logoPath.empty())
            assignIfChanged(entry.logoPath, defaultLogo, result.changed);

        if (info && options.extractCover && defaultCover && info->coverSize > 0)
        {
            const std::string destination = options.coverDirectory.empty()
                ? entry.savePath : options.coverDirectory;
            const std::string cover = extractCover(path, *info, destination, &result.error);
            if (!cover.empty())
                assignIfChanged(entry.logoPath, cover, result.changed);
        }

        if (platform == beiklive::enums::EmuPlatform::EmuNDS &&
            beiklive::tools::tryUseNdsInternalIconCover(entry))
        {
            result.changed = true;
        }
    }

    if (entry.screenShotPath.empty())
    {
        assignIfChanged(entry.screenShotPath,
                        beiklive::path::screenshotPath(), result.changed);
    }

    if (info)
    {
        if (!info->crc32.empty())
            assignIfChanged(entry.crc32, static_cast<int>(info->lookupCrc32), result.changed);
        assignIfChanged(entry.developer, info->developer, result.changed);
        assignIfChanged(entry.releaseDate, info->releaseDate, result.changed);
        assignIfChanged(entry.genre, info->genre, result.changed);
        assignIfChanged(entry.region, info->region, result.changed);
        assignIfChanged(entry.romxBodySha256, info->bodySha256, result.changed);
        assignIfChanged(entry.romxMetadataJson, info->metadataJson, result.changed);
    }

    if (entry.platform == static_cast<int>(beiklive::enums::EmuPlatform::Emu3DS))
    {
        const std::string titleId = beiklive::three_ds::resolveTitleId(
            entry.threeDsTitleId, path);
        if (!titleId.empty())
            assignIfChanged(entry.threeDsTitleId, titleId, result.changed);
    }

    return result;
}

} // 命名空间 beiklive::romx
