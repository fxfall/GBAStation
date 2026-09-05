#include "RomxPlatformIdentity.hpp"

#include "RomxFrontend.hpp"
#include "core/ThreeDsTitlePaths.hpp"
#include "core/constexpr.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace fs = std::filesystem;

namespace beiklive::romx
{
namespace
{
constexpr uint64_t kMaximumIdentityRead = UINT64_C(32) * 1024U * 1024U;
}

bool isPsp(const GameEntry& entry)
{
    return entry.platform == static_cast<int>(enums::EmuPlatform::EmuPSP);
}

bool isThreeDs(const GameEntry& entry)
{
    return entry.platform == static_cast<int>(enums::EmuPlatform::Emu3DS);
}

fs::path pspSaveRoot()
{
    // PPSSPP is configured to use GBAStation/saves/PSP for native savedata.
    // Keep this core-specific root out of the generic GameEntry save mapping.
    return fs::path(beiklive::path::savePath()) / "PSP";
}

fs::path pspCheatRoot()
{
    const fs::path applicationRoot = fs::path(beiklive::path::savePath()).parent_path();
    return applicationRoot / "PSP" / "Cheats";
}

bool readRomxEntryBytes(const romx_reader_t* reader, uint32_t index,
                        uint64_t offset, uint64_t size,
                        std::vector<uint8_t>& output)
{
    if (reader == nullptr || size == 0 || size > kMaximumIdentityRead ||
        size > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        offset > UINT64_MAX - size)
        return false;
    output.resize(static_cast<std::size_t>(size));
    uint64_t position = 0;
    while (position < size)
    {
        const uint64_t requested = std::min<uint64_t>(256U * 1024U, size - position);
        uint64_t received = 0;
        romx_error_t error{};
        const romx_result_t result = romx_reader_read_entry(
            reader, index, offset + position,
            output.data() + static_cast<std::size_t>(position),
            requested, &received, &error);
        if (result != ROMX_OK || received == 0 || received > requested)
            return false;
        position += received;
    }
    return true;
}

// A 3DS ROMX entry is normally a CCI/NCSD image. The NCSD header is inside
// the entrypoint payload, not at the beginning of the ROMX container.
std::string readThreeDsTitleIdFromRomx(const std::string& path)
{
    if (!isRomxPath(path))
        return {};

    romx_reader_t* reader = nullptr;
    romx_error_t error{};
    if (romx_reader_open_path(path.c_str(), nullptr, &reader, &error) != ROMX_OK)
        return {};

    romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
    if (romx_reader_get_entrypoint(reader, &entry, &error) != ROMX_OK ||
        entry.data_size < 0x110U)
    {
        romx_reader_close(reader);
        return {};
    }

    std::vector<uint8_t> header;
    const bool read = readRomxEntryBytes(reader, entry.index, 0, 0x110U, header);
    romx_reader_close(reader);
    if (!read || header.size() < 0x110U || header[0x100] != 'N' ||
        header[0x101] != 'C' || header[0x102] != 'S' || header[0x103] != 'D')
        return {};

    uint64_t titleId = 0;
    for (int index = 0; index < 8; ++index)
        titleId |= static_cast<uint64_t>(header[0x108U + index]) << (index * 8);
    if (titleId == 0)
        return {};

    char titleIdText[17] = {};
    const int written = std::snprintf(
        titleIdText, sizeof(titleIdText), "%016llX",
        static_cast<unsigned long long>(titleId));
    return written == 16 ? beiklive::three_ds::normalizeTitleId(titleIdText)
                         : std::string();
}
}
