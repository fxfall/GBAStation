#include "RomxGameEntryAdapter.hpp"

#include "RomxFrontend.hpp"
#include "RomxPlatformIdentity.hpp"
#include "RomxSavePaths.hpp"
#include "RomxSaveRestore.hpp"
#include "RomxStatsAdapter.hpp"
#include "core/ThreeDsTitlePaths.hpp"
#include "core/Tools.hpp"
#include "core/constexpr.h"

#include <romx/romx.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace beiklive::romx
{
namespace
{
std::string errorText(const char* operation, const romx_error_t& error, romx_result_t result)
{
    std::ostringstream stream;
    stream << operation << " failed (" << result << ")";
    if (error.message[0] != '\0')
        stream << ": " << error.message;
    if (error.system_code != 0)
    {
        stream << " [system_code=" << error.system_code;
        const char* systemMessage = std::strerror(error.system_code);
        if (systemMessage != nullptr && *systemMessage != '\0')
            stream << ": " << systemMessage;
        stream << "]";
    }
    return stream.str();
}

void assignError(std::string* output, const std::string& message)
{
    if (output)
        *output = message;
}

constexpr int kPspPlatform = static_cast<int>(enums::EmuPlatform::EmuPSP);
constexpr int kThreeDsPlatform = static_cast<int>(enums::EmuPlatform::Emu3DS);
constexpr uint64_t kPspSfoMaximumSize = 4U * 1024U * 1024U;
constexpr uint64_t kPspDirectoryMaximumSize = 32U * 1024U * 1024U;

std::string upperAlphanumeric(std::string value)
{
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char character : value)
    {
        if (std::isalnum(character) == 0)
            continue;
        normalized.push_back(static_cast<char>(std::toupper(character)));
    }
    return normalized;
}

bool isPlausiblePspDiscId(const std::string& value)
{
    if (value.size() < 4 || value.size() > 32)
        return false;
    bool hasDigit = false;
    for (const unsigned char character : value)
    {
        if (std::isdigit(character) != 0)
            hasDigit = true;
        if (std::isalnum(character) == 0)
            return false;
    }
    return hasDigit;
}

std::string normalizePspDiscId(const std::string& value)
{
    const std::string normalized = upperAlphanumeric(value);
    return isPlausiblePspDiscId(normalized) ? normalized : std::string();
}

uint32_t readU32Le(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) |
           (static_cast<uint32_t>(bytes[3]) << 24U);
}

uint32_t readU32Be(const uint8_t* bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24U) |
           (static_cast<uint32_t>(bytes[1]) << 16U) |
           (static_cast<uint32_t>(bytes[2]) << 8U) |
           static_cast<uint32_t>(bytes[3]);
}

uint32_t readIsoBothEndian(const uint8_t* bytes, uint64_t payloadSize,
                           bool sector)
{
    const uint32_t little = readU32Le(bytes);
    const uint32_t big = readU32Be(bytes);
    const uint64_t scale = sector ? 2048U : 1U;
    if (little != 0 && static_cast<uint64_t>(little) <= payloadSize / scale)
        return little;
    return big;
}

size_t boundedStringLength(const char* value, size_t maximum)
{
    size_t length = 0;
    while (length < maximum && value[length] != '\0')
        ++length;
    return length;
}

bool readPspSfoValue(const std::vector<uint8_t>& sfo, const char* wantedKey,
                     std::string& value)
{
    value.clear();
    if (sfo.size() < 20 || !wantedKey)
        return false;

    // Retail PSP SFO files start with 00 PSF and use a 20-byte header with
    // 16-byte index records.  A few tools emit the historical PSF/20-byte
    // record layout; accept it as a compatibility fallback for old bundles.
    const bool retailMagic = sfo.size() >= 4 && sfo[0] == 0x00 &&
                             sfo[1] == 'P' && sfo[2] == 'S' && sfo[3] == 'F';
    const bool shortMagic = sfo.size() >= 3 && sfo[0] == 'P' &&
                            sfo[1] == 'S' && sfo[2] == 'F';
    if (!retailMagic && !shortMagic)
        return false;

    const auto parseRetail = [&]() -> bool {
        if (sfo.size() < 20)
            return false;
        const uint32_t keyTable = readU32Le(sfo.data() + 8);
        const uint32_t dataTable = readU32Le(sfo.data() + 12);
        const uint32_t count = readU32Le(sfo.data() + 16);
        if (keyTable >= sfo.size() || dataTable > sfo.size() || count > 4096U)
            return false;
        const uint64_t indexEnd = 20U + static_cast<uint64_t>(count) * 16U;
        if (indexEnd > sfo.size())
            return false;
        for (uint32_t index = 0; index < count; ++index)
        {
            const uint8_t* record = sfo.data() + 20U + index * 16U;
            const uint16_t keyOffset = static_cast<uint16_t>(record[0]) |
                                       (static_cast<uint16_t>(record[1]) << 8U);
            const uint16_t format = static_cast<uint16_t>(record[2]) |
                                    (static_cast<uint16_t>(record[3]) << 8U);
            const uint32_t dataLength = readU32Le(record + 4);
            const uint32_t dataOffset = readU32Le(record + 12);
            const uint64_t keyPosition = static_cast<uint64_t>(keyTable) + keyOffset;
            const uint64_t dataPosition = static_cast<uint64_t>(dataTable) + dataOffset;
            if (keyPosition >= sfo.size() || dataPosition > sfo.size() ||
                dataLength > sfo.size() - dataPosition)
                continue;
            const char* key = reinterpret_cast<const char*>(sfo.data() + keyPosition);
            const size_t keyLength = boundedStringLength(
                key, sfo.size() - static_cast<size_t>(keyPosition));
            if (keyLength == sfo.size() - keyPosition ||
                std::string(key, keyLength) != wantedKey)
                continue;
            const uint8_t* data = sfo.data() + dataPosition;
            const size_t length = boundedStringLength(
                reinterpret_cast<const char*>(data), dataLength);
            if (format == 0x0406U)
                return false; // DISC_ID/SAVEDATA_DIRECTORY are UTF-8 in PSP SFO.
            value.assign(reinterpret_cast<const char*>(data), length);
            return true;
        }
        return false;
    };

    if (retailMagic && parseRetail())
        return true;
    if (!shortMagic)
        return false;

    // Compatibility layout used by the old frontend's synthetic SFO tests.
    if (sfo.size() < 24)
        return false;
    const uint32_t keyTable = readU32Le(sfo.data() + 12);
    const uint32_t dataTable = readU32Le(sfo.data() + 16);
    const uint32_t count = readU32Le(sfo.data() + 20);
    if (keyTable < 20U || keyTable >= sfo.size() || dataTable > sfo.size() ||
        count > 4096U || keyTable + static_cast<uint64_t>(count) * 20U > sfo.size())
        return false;
    for (uint32_t index = 0; index < count; ++index)
    {
        const uint8_t* record = sfo.data() + keyTable + index * 20U;
        const uint32_t keyOffset = readU32Le(record);
        const uint32_t format = readU32Le(record + 4);
        const uint32_t dataLength = readU32Le(record + 8);
        const uint32_t dataOffset = readU32Le(record + 16);
        const uint64_t keyPosition = static_cast<uint64_t>(keyTable) + keyOffset;
        const uint64_t dataPosition = static_cast<uint64_t>(dataTable) + dataOffset;
        if (keyPosition >= sfo.size() || dataPosition > sfo.size() ||
            dataLength > sfo.size() - dataPosition)
            continue;
        const char* key = reinterpret_cast<const char*>(sfo.data() + keyPosition);
        const size_t keyLength = boundedStringLength(
            key, sfo.size() - static_cast<size_t>(keyPosition));
        if (keyLength == sfo.size() - keyPosition ||
            std::string(key, keyLength) != wantedKey)
            continue;
        if (format == 0x0406U)
            return false;
        const uint8_t* data = sfo.data() + dataPosition;
        value.assign(reinterpret_cast<const char*>(data),
                     boundedStringLength(reinterpret_cast<const char*>(data), dataLength));
        return true;
    }
    return false;
}

struct IsoFileRecord
{
    uint32_t lba = 0;
    uint32_t size = 0;
};

std::string isoName(const uint8_t* record, uint8_t nameLength)
{
    std::string name(reinterpret_cast<const char*>(record + 33), nameLength);
    const std::size_t version = name.find(';');
    if (version != std::string::npos)
        name.resize(version);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return name;
}

bool findIsoFile(const std::vector<uint8_t>& directory, const std::string& wanted,
                IsoFileRecord& result)
{
    std::size_t position = 0;
    while (position < directory.size())
    {
        const uint8_t length = directory[position];
        if (length == 0)
        {
            position = ((position / 2048U) + 1U) * 2048U;
            continue;
        }
        if (length < 34U || position + length > directory.size())
            return false;
        const uint8_t nameLength = directory[position + 32U];
        if (33U + nameLength > length)
            return false;
        const uint8_t* record = directory.data() + position;
        if (isoName(record, nameLength) == wanted)
        {
            result.lba = readU32Le(record + 2);
            result.size = readU32Le(record + 10);
            if (result.lba == 0)
                result.lba = readU32Be(record + 2);
            if (result.size == 0)
                result.size = readU32Be(record + 10);
            return true;
        }
        position += length;
    }
    return false;
}

std::string readPspDiscIdFromRomx(const std::string& path)
{
    romx_reader_t* reader = nullptr;
    romx_error_t error{};
    if (romx_reader_open_path(path.c_str(), nullptr, &reader, &error) != ROMX_OK)
        return {};
    romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
    if (romx_reader_get_entrypoint(reader, &entry, &error) != ROMX_OK)
    {
        romx_reader_close(reader);
        return {};
    }
    const uint64_t payloadSize = entry.data_size;
    // PSP EBOOT.PBP stores PARAM.SFO as its first subfile.  Detect it before
    // applying the ISO minimum-size check because a real PBP is often larger
    // than one ISO directory sector.
    std::vector<uint8_t> pbpHeader;
    if (payloadSize >= 32U &&
        readRomxEntryBytes(reader, entry.index, 0, 32U, pbpHeader) &&
        pbpHeader.size() >= 16U && pbpHeader[0] == 0x00U &&
        pbpHeader[1] == 'P' && pbpHeader[2] == 'B' && pbpHeader[3] == 'P')
    {
        const uint32_t start = readU32Le(pbpHeader.data() + 8U);
        const uint32_t end = readU32Le(pbpHeader.data() + 12U);
        if (start < end && static_cast<uint64_t>(end) <= payloadSize &&
            end - start <= kPspSfoMaximumSize)
        {
            std::vector<uint8_t> sfo;
            if (readRomxEntryBytes(reader, entry.index, start, end - start, sfo))
            {
                std::string discId;
                if (readPspSfoValue(sfo, "DISC_ID", discId))
                {
                    romx_reader_close(reader);
                    return normalizePspDiscId(discId);
                }
            }
        }
        romx_reader_close(reader);
        return {};
    }
    if (payloadSize < 16U * 2048U + 2048U)
    {
        romx_reader_close(reader);
        return {};
    }

    std::vector<uint8_t> pvd;
    if (!readRomxEntryBytes(reader, entry.index, 16U * 2048U, 2048U, pvd) ||
        pvd.size() < 2048U || pvd[0] != 1U || pvd[1] != 'C' || pvd[2] != 'D' ||
        pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1')
    {
        romx_reader_close(reader);
        return {};
    }
    const uint8_t* rootRecord = pvd.data() + 156U;
    const uint32_t rootLba = readIsoBothEndian(rootRecord + 2, payloadSize, true);
    const uint32_t rootSize = readIsoBothEndian(rootRecord + 10, payloadSize, false);
    if (rootLba == 0 || rootSize == 0 || rootSize > kPspDirectoryMaximumSize ||
        static_cast<uint64_t>(rootLba) * 2048U > payloadSize ||
        static_cast<uint64_t>(rootSize) > payloadSize -
            static_cast<uint64_t>(rootLba) * 2048U)
    {
        romx_reader_close(reader);
        return {};
    }

    std::vector<uint8_t> root;
    if (!readRomxEntryBytes(reader, entry.index,
                            static_cast<uint64_t>(rootLba) * 2048U, rootSize, root))
    {
        romx_reader_close(reader);
        return {};
    }
    IsoFileRecord gameDirectory;
    if (!findIsoFile(root, "PSP_GAME", gameDirectory) || gameDirectory.size == 0 ||
        gameDirectory.size > kPspDirectoryMaximumSize)
    {
        romx_reader_close(reader);
        return {};
    }
    std::vector<uint8_t> game;
    if (!readRomxEntryBytes(reader, entry.index,
                            static_cast<uint64_t>(gameDirectory.lba) * 2048U,
                            gameDirectory.size, game))
    {
        romx_reader_close(reader);
        return {};
    }
    IsoFileRecord sfoRecord;
    if (!findIsoFile(game, "PARAM.SFO", sfoRecord) || sfoRecord.size == 0 ||
        sfoRecord.size > kPspSfoMaximumSize ||
        static_cast<uint64_t>(sfoRecord.lba) * 2048U > payloadSize ||
        static_cast<uint64_t>(sfoRecord.size) > payloadSize -
            static_cast<uint64_t>(sfoRecord.lba) * 2048U)
    {
        romx_reader_close(reader);
        return {};
    }
    std::vector<uint8_t> sfo;
    const bool read = readRomxEntryBytes(
        reader, entry.index, static_cast<uint64_t>(sfoRecord.lba) * 2048U,
        sfoRecord.size, sfo);
    romx_reader_close(reader);
    if (!read)
        return {};
    std::string discId;
    return readPspSfoValue(sfo, "DISC_ID", discId)
        ? normalizePspDiscId(discId) : std::string();
}

std::string pspDiscId(const GameEntry& entry)
{
    if (!isPsp(entry))
        return {};
    if (entry.romx.is_object())
    {
        const auto cached = entry.romx.find("psp_disc_id");
        if (cached != entry.romx.end() && cached->is_string())
        {
            const std::string value = normalizePspDiscId(cached->get<std::string>());
            if (!value.empty())
                return value;
        }
        const auto metadata = entry.romx.find("metadata");
        if (metadata != entry.romx.end() && metadata->is_object())
        {
            const auto serial = metadata->find("serial");
            if (serial != metadata->end() && serial->is_string())
            {
                const std::string value = normalizePspDiscId(serial->get<std::string>());
                if (!value.empty())
                    return value;
            }
        }
    }
    if (!isRomxPath(entry.path))
        return {};
    Info info;
    std::string ignored;
    if (readInfo(entry.path, info, &ignored))
    {
        const std::string metadataId = normalizePspDiscId(info.serial);
        if (!metadataId.empty())
            return metadataId;
    }
    return readPspDiscIdFromRomx(entry.path);
}

std::optional<fs::path> findPspSfoPath(const fs::path& directory)
{
    std::error_code ec;
    for (const fs::directory_entry& child : fs::directory_iterator(
             directory, fs::directory_options::skip_permission_denied, ec))
    {
        if (ec)
            break;
        if (!child.is_regular_file(ec))
        {
            ec.clear();
            continue;
        }
        std::string filename = child.path().filename().string();
        std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        if (filename == "PARAM.SFO")
            return child.path();
        ec.clear();
    }
    return std::nullopt;
}

bool isPspSfoFilename(const fs::path& path)
{
    std::string filename = path.filename().string();
    std::transform(filename.begin(), filename.end(), filename.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return filename == "PARAM.SFO";
}

std::optional<std::string> readSfoValueFromFile(const fs::path& directory,
                                                const char* key)
{
    const std::optional<fs::path> sfoPath = findPspSfoPath(directory);
    if (!sfoPath)
        return std::nullopt;
    std::error_code ec;
    const uintmax_t fileSize = fs::file_size(*sfoPath, ec);
    if (ec || fileSize == 0 || fileSize > kPspSfoMaximumSize)
        return std::nullopt;
    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
    std::ifstream file(*sfoPath, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size())))
        return std::nullopt;
    std::string value;
    return readPspSfoValue(bytes, key, value)
        ? std::optional<std::string>(std::move(value)) : std::nullopt;
}

bool isPspSaveDirectoryForDisc(const fs::path& directory,
                               const std::string& discId)
{
    std::error_code ec;
    if (!fs::is_directory(directory, ec) || ec)
        return false;
    const std::string directoryName = normalizePspDiscId(directory.filename().string());
    if (directoryName.empty() || directoryName.rfind(discId, 0) != 0)
        return false;

    // Every PPSSPP savedata directory contains PARAM.SFO.  Its
    // SAVEDATA_DIRECTORY must point back to this exact directory; if a title
    // also carries DISC_ID, require that field to match as well.
    const auto savedataDirectory = readSfoValueFromFile(directory, "SAVEDATA_DIRECTORY");
    const auto savedataDiscId = readSfoValueFromFile(directory, "DISC_ID");
    if (!savedataDirectory && !savedataDiscId)
        return false;
    if (savedataDirectory &&
        normalizePspDiscId(*savedataDirectory) != directoryName)
        return false;
    if (savedataDiscId && normalizePspDiscId(*savedataDiscId) != discId)
        return false;
    return true;
}

struct PspSaveDirectory
{
    fs::path path;
    std::string name;
};

std::vector<PspSaveDirectory> listPspSaveDirectories(const fs::path& root,
                                                      const std::string& discId)
{
    std::vector<PspSaveDirectory> directories;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec)
        return directories;

    for (const fs::directory_entry& directory : fs::directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec))
    {
        if (ec)
            break;
        if (!directory.is_directory(ec) || directory.is_symlink(ec))
        {
            ec.clear();
            continue;
        }
        if (isPspSaveDirectoryForDisc(directory.path(), discId))
        {
            directories.push_back({directory.path(), directory.path().filename().string()});
        }
        ec.clear();
    }

    std::stable_sort(directories.begin(), directories.end(), [&discId](
        const PspSaveDirectory& left, const PspSaveDirectory& right) {
        const bool leftExact = normalizePspDiscId(left.name) == discId;
        const bool rightExact = normalizePspDiscId(right.name) == discId;
        if (leftExact != rightExact)
            return leftExact;
        return left.name < right.name;
    });
    return directories;
}

std::optional<PspSaveDirectory> findPspSaveDirectory(const fs::path& root,
                                                     const std::string& discId)
{
    const std::vector<PspSaveDirectory> directories =
        listPspSaveDirectories(root, discId);
    if (directories.empty())
        return std::nullopt;
    return directories.front();
}

bool pspSaveBundlePathMatches(const fs::path& relative,
                              const fs::path& root,
                              const std::string& discId)
{
    auto component = relative.begin();
    if (component == relative.end())
        return false;
    const std::string directoryName = normalizePspDiscId(component->string());
    if (directoryName.empty() || directoryName.rfind(discId, 0) != 0)
        return false;
    const fs::path targetDirectory = root / *component;
    std::error_code ec;
    // Existing targets must pass the same PARAM.SFO check.  New directories
    // are accepted by prefix here; their PARAM.SFO is part of the bundle and
    // will be installed atomically with the rest of the savedata.
    return !fs::exists(targetDirectory, ec) ||
           isPspSaveDirectoryForDisc(targetDirectory, discId);
}

bool pspSaveBundlePathHasDiscPrefix(const fs::path& relative,
                                    const std::string& discId)
{
    auto component = relative.begin();
    if (component == relative.end())
        return false;
    const std::string directoryName = normalizePspDiscId(component->string());
    return !directoryName.empty() && directoryName.rfind(discId, 0) == 0;
}

bool pspCheatBundlePathMatches(const fs::path& relative,
                               const std::string& discId)
{
    if (relative.has_parent_path())
        return false;
    std::string filename = relative.filename().string();
    std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    const std::string expected = discId + ".INI";
    return filename == expected;
}

std::function<bool(const fs::path&)> bundlePathValidator(
    const GameEntry& entry, romx_mutable_namespace_t objectNamespace,
    const fs::path& root)
{
    if (!isPsp(entry))
        return {};
    const std::string discId = pspDiscId(entry);
    if (discId.empty())
        return [](const fs::path&) { return false; };
    if (objectNamespace == ROMX_MUTABLE_NAMESPACE_SAVE)
        return [root, discId](const fs::path& relative) {
            return pspSaveBundlePathMatches(relative, root, discId);
        };
    if (objectNamespace == ROMX_MUTABLE_NAMESPACE_CHEAT)
        return [discId](const fs::path& relative) {
            return pspCheatBundlePathMatches(relative, discId);
        };
    return {};
}

int mutableKeyPriority(const GameEntry& entry, const std::string& key)
{
    if (isPsp(entry))
    {
        if (key == "ppsspp")
            return 0;
        if (key == "default")
            return 1;
        if (key == "libretro")
            return 2;
        return 3;
    }
    if (key == "libretro")
        return 0;
    if (key == "default")
        return 1;
    return 2;
}

bool validUtf8(const std::string& value)
{
    const auto continuation = [](unsigned char byte) {
        return (byte & 0xc0U) == 0x80U;
    };
    for (std::size_t index = 0; index < value.size();)
    {
        const unsigned char first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7fU)
        {
            ++index;
            continue;
        }
        std::size_t length = 0;
        uint32_t codepoint = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2U && first <= 0xdfU)
        {
            length = 2;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        }
        else if (first >= 0xe0U && first <= 0xefU)
        {
            length = 3;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        }
        else if (first >= 0xf0U && first <= 0xf4U)
        {
            length = 4;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        }
        else
        {
            return false;
        }
        if (index + length > value.size())
            return false;
        for (std::size_t offset = 1; offset < length; ++offset)
        {
            const unsigned char byte = static_cast<unsigned char>(value[index + offset]);
            if (!continuation(byte))
                return false;
            codepoint = (codepoint << 6U) | (byte & 0x3fU);
        }
        // Reject overlong encodings, UTF-16 surrogate code points, and values
        // beyond Unicode's scalar range.  ROMX keys are UTF-8, not arbitrary
        // byte strings.
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU))
            return false;
        index += length;
    }
    return true;
}

bool validateSaveSlotKeyImpl(const std::string& key, std::string* error)
{
    const auto fail = [error](const char* message) {
        if (error)
            *error = message;
        return false;
    };
    if (key.empty())
        return fail("存档名称不能为空");
    if (key.size() > ROMX_MUTABLE_KEY_CAPACITY)
        return fail("存档名称超过 ROMX 的 448 字节限制");
    if (!validUtf8(key))
        return fail("存档名称不是有效的 UTF-8");
    if (key.find('\0') != std::string::npos ||
        key.find('/') != std::string::npos ||
        key.find('\\') != std::string::npos)
        return fail("存档名称不能包含路径分隔符");
    if (key == "." || key == "..")
        return fail("存档名称不能是 . 或 ..");
    return true;
}

bool validateBundleSlotKey(const std::string& key)
{
    if (key.empty() || key.size() > ROMX_MUTABLE_BUNDLE_PATH_CAPACITY ||
        !validUtf8(key) || key.find('\0') != std::string::npos ||
        key.front() == '/' || key.back() == '/' ||
        key.find('\\') != std::string::npos)
        return false;
    std::size_t component = 0;
    for (std::size_t index = 0; index <= key.size(); ++index)
    {
        if (index < key.size() && key[index] != '/')
            continue;
        const std::size_t componentSize = index - component;
        if (componentSize == 0 ||
            (componentSize == 1 && key[component] == '.') ||
            (componentSize == 2 && key[component] == '.' &&
             key[component + 1U] == '.'))
            return false;
        component = index + 1U;
    }
    return true;
}

const char* mutableBundleKey(const GameEntry& entry)
{
    return isPsp(entry) ? "ppsspp" : "libretro";
}

constexpr const char* kBundleSlotSelectorPrefix = "@romx-save-slot:";

std::string makeBundleSlotSelector(const std::string& objectKey,
                                   const std::string& slotKey)
{
    // This is an adapter-only token, not a ROMX key.  Length-prefixing the
    // object key keeps arbitrary valid UTF-8 labels unambiguous.
    return std::string(kBundleSlotSelectorPrefix) +
           std::to_string(objectKey.size()) + ":" + objectKey + slotKey;
}

bool parseBundleSlotSelector(const std::string& selector,
                             std::string& objectKey,
                             std::string& slotKey)
{
    const std::string prefix(kBundleSlotSelectorPrefix);
    if (selector.rfind(prefix, 0) != 0)
        return false;

    objectKey.clear();
    slotKey.clear();

    const std::size_t lengthStart = prefix.size();
    const std::size_t separator = selector.find(':', lengthStart);
    if (separator == std::string::npos || separator == lengthStart)
        return false;
    std::size_t objectLength = 0;
    try
    {
        objectLength = std::stoull(selector.substr(lengthStart,
                                                   separator - lengthStart));
    }
    catch (...)
    {
        return false;
    }
    const std::size_t objectStart = separator + 1U;
    if (objectLength == 0 || objectStart > selector.size() ||
        objectLength > selector.size() - objectStart)
        return false;
    objectKey = selector.substr(objectStart, objectLength);
    slotKey = selector.substr(objectStart + objectLength);
    return validateSaveSlotKeyImpl(objectKey, nullptr) &&
           validateBundleSlotKey(slotKey);
}

fs::path resolvedCheatPath(const GameEntry& entry)
{
    if (isPsp(entry))
    {
        const std::string discId = pspDiscId(entry);
        if (!discId.empty())
            return pspCheatRoot() / (discId + ".ini");
    }
    if (!entry.cheatPath.empty())
        return fs::path(entry.cheatPath);
    const std::string filename =
        entry.platform == static_cast<int>(enums::EmuPlatform::EmuNDS)
            ? std::string("usrcheat.dat")
            : fs::path(entry.path).stem().string() + ".cht";
    return fs::path(beiklive::path::cheatPath()) / filename;
}

fs::path threeDsNativeSaveDirectory(const GameEntry& entry)
{
    const fs::path root = entry.savePath.empty()
        ? fs::path(beiklive::tools::defaultGameSavePath(entry.platform, entry.path))
        : fs::path(entry.savePath);
    if (root.empty())
        return {};

    const std::string titleId = GameEntryAdapter::resolveThreeDsTitleId(entry);
    if (titleId.size() != 16U)
        return root;

#ifdef __SWITCH__
    // The built-in core stores 3DS save data in the same canonical SDMC tree
    // exposed by ThreeDsTitlePaths.  Use the payload-derived Title ID for a
    // ROMX entry instead of falling back to the generic per-ROM directory.
    const std::string native = beiklive::three_ds::saveDataPath(titleId);
    return native.empty() ? root : fs::path(native);
#else
    std::string lowerTitleId = titleId;
    std::transform(lowerTitleId.begin(), lowerTitleId.end(), lowerTitleId.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });

    // Azahar receives `savePath` as its libretro root and appends Azahar/
    // before deriving SDMCDir. Its save-data archive is then split into the
    // high/low 32-bit Title ID directories below data/00000001.
    constexpr const char* kZeroId =
        "00000000000000000000000000000000";
    return root / "Azahar" / "sdmc" / "Nintendo 3DS" / kZeroId / kZeroId /
           "title" / lowerTitleId.substr(0, 8U) / lowerTitleId.substr(8U, 8U) /
           "data" / "00000001";
#endif
}

fs::path threeDsNativeSdRoot(const GameEntry& entry)
{
#ifdef __SWITCH__
    // The mapper below returns paths relative to the actual SD root.  Keep
    // this root independent from the selected Title Save directory so an
    // ExtData object cannot be redirected into a Title Save by accident.
    return fs::path(beiklive::three_ds::sdRootPath());
#else
    const fs::path root = entry.savePath.empty()
        ? fs::path(beiklive::tools::defaultGameSavePath(entry.platform, entry.path))
        : fs::path(entry.savePath);
    if (root.empty())
        return root;
    constexpr const char* kZeroId = "00000000000000000000000000000000";
    return root / "Azahar" / "sdmc" / "Nintendo 3DS" / kZeroId / kZeroId;
#endif
}

std::string namespaceDirectory(const GameEntry& entry, romx_mutable_namespace_t ns)
{
    if (ns == ROMX_MUTABLE_NAMESPACE_CHEAT)
    {
        if (isPsp(entry))
            return pspCheatRoot().string();
        const fs::path cheatPath = resolvedCheatPath(entry);
        std::error_code ec;
        if (fs::is_directory(cheatPath, ec))
            return cheatPath.string();
        if (!cheatPath.parent_path().empty())
            return cheatPath.parent_path().string();
        return beiklive::path::cheatPath();
    }

    if (isPsp(entry))
        return pspSaveRoot().string();
    if (isThreeDs(entry))
        return threeDsNativeSaveDirectory(entry).string();
    return entry.savePath.empty()
        ? beiklive::tools::defaultGameSavePath(entry.platform, entry.path)
        : entry.savePath;
}

bool safeRelativePath(const char* input, fs::path& result)
{
    if (!input || !*input)
        return false;
    const std::string raw(input);
    // Backslashes are not valid ROMX bundle separators.  More importantly,
    // reject dot components before lexical normalization so a traversal such
    // as "a/../b" cannot be normalized into an apparently safe path.
    if (raw.find('\\') != std::string::npos || raw.find('\0') != std::string::npos)
        return false;
    const fs::path rawPath(raw);
    for (const auto& component : rawPath)
        if (component == ".." || component == ".")
            return false;
    fs::path candidate = rawPath.lexically_normal();
    if (candidate.empty() || candidate.is_absolute())
        return false;
    result = candidate;
    return true;
}

bool isWithinDirectory(const fs::path& directory, const fs::path& candidate)
{
    std::error_code ec;
    const fs::path canonicalDirectory = fs::weakly_canonical(directory, ec);
    if (ec)
        return false;
    const fs::path canonicalCandidate = fs::weakly_canonical(candidate, ec);
    if (ec)
        return false;
    const fs::path relative = canonicalCandidate.lexically_relative(canonicalDirectory);
    if (relative.empty() || relative.is_absolute())
        return false;
    for (const auto& component : relative)
        if (component == "..")
            return false;
    return true;
}

// std::filesystem::copy_file(..., overwrite_existing) is not implemented
// consistently by the Switch SDMC filesystem: it can still return EEXIST
// when the destination already exists.  Publish the completed temporary file
// with rename first (atomic on normal POSIX filesystems), then use a guarded
// remove-and-rename fallback for platforms that reject replacing a file.
bool installMutableFile(const fs::path& temporary, const fs::path& output,
                        bool overwrite, std::error_code& error)
{
    error.clear();
    fs::rename(temporary, output, error);
    if (!error || !overwrite)
        return !error;

    std::error_code existsError;
    if (!fs::exists(output, existsError))
    {
        if (existsError)
            error = existsError;
        return false;
    }
    if (existsError)
    {
        error = existsError;
        return false;
    }

    std::error_code typeError;
    if (fs::is_symlink(output, typeError) || typeError)
    {
        if (typeError)
            error = typeError;
        return false;
    }
    if (!fs::is_regular_file(output, typeError) || typeError)
    {
        if (typeError)
            error = typeError;
        return false;
    }

    error.clear();
    fs::remove(output, error);
    if (error)
        return false;
    fs::rename(temporary, output, error);
    return !error;
}

// ROMX stores a bundle path for interoperability, but the frontend's
// battery-save filename is derived from GameEntry.path.  Keep that mapping
// separate from the generic path traversal code below so PSP's native
// savedata tree can continue to use its own validator unchanged.
using BundleOutputMapper = SavePathMapper;

fs::path gameBatterySavePath(const GameEntry& entry);
BundleOutputMapper batterySaveOutputMapper(const GameEntry& entry);
std::string pspTemporarySuffix();

BundleOutputMapper threeDsSaveOutputMapper(
    const GameEntry& entry,
    const romx_mutable_save_layout_info_t* saveLayout = nullptr)
{
    const std::string titleId = GameEntryAdapter::resolveThreeDsTitleId(entry);
    romx_mutable_save_layout_info_t layout =
        ROMX_MUTABLE_SAVE_LAYOUT_INFO_INIT;
    if (saveLayout != nullptr)
        layout = *saveLayout;
    return makeThreeDsSaveOutputMapper(titleId, layout);
}

SyncResult extractBundle(const romx_reader_t* reader, romx_mutable_namespace_t ns,
                         const char* key, const fs::path& destination,
                         bool overwrite, std::string* error,
                         std::vector<fs::path>* restoredPaths = nullptr,
                         const std::function<bool(const fs::path&)>& validator = {},
                         const BundleOutputMapper& outputMapper = {},
                         const std::string* requestedSaveSlot = nullptr,
                         const fs::path* transactionDirectory = nullptr)
{
    romx_error_t err{};
    romx_mutable_bundle_t* bundle = nullptr;
    const romx_result_t openResult = romx_mutable_bundle_open(
        reader, ns, key, nullptr, &bundle, &err);
    if (openResult != ROMX_OK)
    {
        // A namespace can legally contain an opaque object.  It is not a file
        // bundle and therefore is not actionable by the frontend.
        if (openResult == ROMX_E_MUTABLE_BUNDLE ||
            openResult == ROMX_E_MUTABLE_ENTRY ||
            openResult == ROMX_E_MUTABLE_ABSENT)
            return SyncResult::Skipped;
        assignError(error, errorText("romx_mutable_bundle_open", err, openResult));
        return SyncResult::Failed;
    }

    uint32_t count = 0;
    const romx_result_t countResult =
        romx_mutable_bundle_get_entry_count(bundle, &count, &err);
    if (countResult != ROMX_OK)
    {
        romx_mutable_bundle_close(bundle);
        assignError(error, errorText("romx_mutable_bundle_get_entry_count", err,
                                     countResult));
        return SyncResult::Failed;
    }
    std::vector<uint32_t> selectedEntries;
    if (requestedSaveSlot != nullptr) {
        if (ns != ROMX_MUTABLE_NAMESPACE_SAVE) {
            romx_mutable_bundle_close(bundle);
            assignError(error, "SAVE slot selection used for a non-SAVE bundle");
            return SyncResult::Failed;
        }
        uint32_t slotCount = 0;
        const romx_result_t slotCountResult =
            romx_mutable_bundle_get_save_slot_count(bundle, &slotCount, &err);
        if (slotCountResult != ROMX_OK) {
            romx_mutable_bundle_close(bundle);
            assignError(error, errorText("romx_mutable_bundle_get_save_slot_count",
                                         err, slotCountResult));
            return SyncResult::Failed;
        }
        for (uint32_t slotIndex = 0; slotIndex < slotCount; ++slotIndex) {
            romx_mutable_save_slot_info_t slot = ROMX_MUTABLE_SAVE_SLOT_INFO_INIT;
            if (romx_mutable_bundle_get_save_slot(bundle, slotIndex, &slot,
                                                   &err) != ROMX_OK)
                continue;
            if (std::string(slot.key, slot.key_size) != *requestedSaveSlot)
                continue;
            selectedEntries.reserve(slot.entry_count);
            for (uint32_t entryIndex = 0; entryIndex < slot.entry_count;
                 ++entryIndex) {
                romx_mutable_bundle_entry_info_t selected =
                    ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
                if (romx_mutable_bundle_get_save_slot_entry(
                        bundle, slotIndex, entryIndex, &selected, &err) != ROMX_OK) {
                    romx_mutable_bundle_close(bundle);
                    assignError(error, errorText(
                        "romx_mutable_bundle_get_save_slot_entry", err,
                        ROMX_E_MUTABLE_BUNDLE));
                    return SyncResult::Failed;
                }
                selectedEntries.push_back(selected.index);
            }
            break;
        }
        if (selectedEntries.empty()) {
            romx_mutable_bundle_close(bundle);
            return SyncResult::Skipped;
        }
        count = static_cast<uint32_t>(selectedEntries.size());
    }
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec)
    {
        romx_mutable_bundle_close(bundle);
        assignError(error, "cannot create ROMX mutable directory: " + ec.message());
        return SyncResult::Failed;
    }
    DirectoryRestoreTransaction restoreTransaction;
    bool hasRestoreTransaction = false;
    auto abortRestore = [&]() {
        if (hasRestoreTransaction)
            abortDirectoryRestore(restoreTransaction);
    };
    if (transactionDirectory != nullptr)
    {
        if (!isWithinDirectory(destination, *transactionDirectory))
        {
            romx_mutable_bundle_close(bundle);
            assignError(error, "ROMX restore directory escapes the native SD root");
            return SyncResult::Failed;
        }
        const DirectoryRestoreResult prepared = prepareDirectoryRestore(
            *transactionDirectory, overwrite, pspTemporarySuffix(),
            restoreTransaction, error);
        if (prepared != DirectoryRestoreResult::Ready)
        {
            romx_mutable_bundle_close(bundle);
            return prepared == DirectoryRestoreResult::Skipped
                ? SyncResult::Skipped : SyncResult::Failed;
        }
        hasRestoreTransaction = true;
    }
    struct PendingEntry
    {
        uint32_t index = 0;
        uint64_t size = 0;
        fs::path relative;
        fs::path finalRelative;
    };
    std::vector<PendingEntry> pending;
    pending.reserve(count);
    for (uint32_t index = 0; index < count; ++index)
    {
        romx_mutable_bundle_entry_info_t info = ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
        const uint32_t bundleIndex = requestedSaveSlot == nullptr
            ? index : selectedEntries[index];
        const romx_result_t entryResult =
            romx_mutable_bundle_get_entry(bundle, bundleIndex, &info, &err);
        if (entryResult != ROMX_OK)
        {
            abortRestore();
            romx_mutable_bundle_close(bundle);
            assignError(error, errorText("romx_mutable_bundle_get_entry", err,
                                         entryResult));
            return SyncResult::Failed;
        }
        fs::path relative;
        if (!safeRelativePath(info.path, relative))
        {
            abortRestore();
            romx_mutable_bundle_close(bundle);
            assignError(error, "ROMX mutable bundle contains an unsafe path");
            return SyncResult::Failed;
        }
        if (validator && !validator(relative))
        {
            abortRestore();
            romx_mutable_bundle_close(bundle);
            return SyncResult::Skipped;
        }
        fs::path outputRelative = relative;
        if (outputMapper)
        {
            const std::optional<fs::path> mapped =
                outputMapper(relative, index, count);
            if (!mapped)
                continue;
            outputRelative = *mapped;
        }
        const fs::path output = destination / outputRelative;
        if (!isWithinDirectory(destination, output))
        {
            abortRestore();
            romx_mutable_bundle_close(bundle);
            assignError(error, "ROMX mutable destination escapes the local directory");
            return SyncResult::Failed;
        }
        fs::path transactionRelative = outputRelative;
        if (hasRestoreTransaction)
        {
            if (!isWithinDirectory(restoreTransaction.finalDirectory, output))
            {
                abortRestore();
                romx_mutable_bundle_close(bundle);
                assignError(error, "ROMX mutable destination escapes the restore directory");
                return SyncResult::Failed;
            }
            transactionRelative = output.lexically_relative(
                restoreTransaction.finalDirectory);
            if (transactionRelative.empty() || transactionRelative.is_absolute())
            {
                abortRestore();
                romx_mutable_bundle_close(bundle);
                assignError(error, "ROMX mutable restore path is invalid");
                return SyncResult::Failed;
            }
            for (const auto& component : transactionRelative)
            {
                if (component == ".." || component == ".")
                {
                    abortRestore();
                    romx_mutable_bundle_close(bundle);
                    assignError(error, "ROMX mutable restore path is unsafe");
                    return SyncResult::Failed;
                }
            }
        }
        // Automatic import is first-use only; explicit ROMX management passes
        // overwrite=true after the user confirms the destructive operation.
        if (!overwrite && (fs::exists(output, ec) || ec))
        {
            abortRestore();
            romx_mutable_bundle_close(bundle);
            return SyncResult::Skipped;
        }
        pending.push_back({bundleIndex, info.data_size,
                           std::move(transactionRelative),
                           std::move(outputRelative)});
    }

    if (pending.empty())
    {
        abortRestore();
        romx_mutable_bundle_close(bundle);
        return SyncResult::Skipped;
    }

    std::vector<uint8_t> buffer(64 * 1024);
    for (const PendingEntry& pendingEntry : pending)
    {
        const fs::path output = hasRestoreTransaction
            ? restoreTransaction.stagingDirectory / pendingEntry.relative
            : destination / pendingEntry.relative;
        fs::create_directories(output.parent_path(), ec);
        if (ec)
        {
            abortRestore();
            romx_mutable_bundle_close(bundle);
            assignError(error, "cannot create local mutable directory: " + ec.message());
            return SyncResult::Failed;
        }
        fs::path temporary = output;
        temporary += ".romx-import.tmp";
        ec.clear();
        fs::remove(temporary, ec);
        ec.clear();
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            abortRestore();
            romx_mutable_bundle_close(bundle);
            assignError(error, "cannot open local mutable file for writing");
            return SyncResult::Failed;
        }
        uint64_t offset = 0;
        while (offset < pendingEntry.size)
        {
            const uint64_t want = std::min<uint64_t>(buffer.size(), pendingEntry.size - offset);
            uint64_t read = 0;
            const romx_result_t readResult = romx_mutable_bundle_read_entry(
                bundle, pendingEntry.index, offset, buffer.data(), want, &read, &err);
            if (readResult != ROMX_OK || read == 0)
            {
                file.close();
                fs::remove(temporary, ec);
                abortRestore();
                romx_mutable_bundle_close(bundle);
                assignError(error, errorText("romx_mutable_bundle_read_entry", err,
                                             readResult == ROMX_OK
                                                 ? ROMX_E_TRUNCATED : readResult));
                return SyncResult::Failed;
            }
            file.write(reinterpret_cast<const char*>(buffer.data()),
                       static_cast<std::streamsize>(read));
            if (!file)
            {
                file.close();
                fs::remove(temporary, ec);
                abortRestore();
                romx_mutable_bundle_close(bundle);
                assignError(error, "cannot write local mutable file");
                return SyncResult::Failed;
            }
            offset += read;
        }
        file.close();
        if (!file)
        {
            fs::remove(temporary, ec);
            abortRestore();
            romx_mutable_bundle_close(bundle);
            assignError(error, "cannot finish writing local mutable file");
            return SyncResult::Failed;
        }

        installMutableFile(temporary, output, overwrite, ec);
        if (ec)
        {
            std::error_code cleanupError;
            fs::remove(temporary, cleanupError);
            abortRestore();
            romx_mutable_bundle_close(bundle);
            assignError(error, "cannot install local mutable file: " + ec.message());
            return SyncResult::Failed;
        }
        if (restoredPaths && !hasRestoreTransaction)
            restoredPaths->push_back(destination / pendingEntry.finalRelative);
    }

    if (hasRestoreTransaction)
    {
        if (!commitDirectoryRestore(restoreTransaction, error))
        {
            abortRestore();
            romx_mutable_bundle_close(bundle);
            return SyncResult::Failed;
        }
        if (restoredPaths)
        {
            for (const PendingEntry& pendingEntry : pending)
                restoredPaths->push_back(restoreTransaction.finalDirectory /
                                         pendingEntry.finalRelative);
        }
    }
    romx_mutable_bundle_close(bundle);
    return SyncResult::Success;
}

BundleOutputMapper pspSaveOutputMapper(const fs::path& localRoot,
                                       const std::string& discId,
                                       const std::string& requestedDirectory,
                                       std::string& targetDirectory)
{
    const std::optional<PspSaveDirectory> localDirectory =
        findPspSaveDirectory(localRoot, discId);
    if (localDirectory)
        targetDirectory = localDirectory->name;
    else if (!requestedDirectory.empty())
        targetDirectory = requestedDirectory;

    return makePspSaveOutputMapper(targetDirectory, requestedDirectory);
}

std::string pspTemporarySuffix()
{
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::to_string(ticks);
}

bool openThreeDsSaveCatalog(const fs::path& source,
                            romx_save_catalog_t** catalog,
                            uint32_t* candidateCount,
                            std::string* error)
{
    if (catalog)
        *catalog = nullptr;
    if (candidateCount)
        *candidateCount = 0;
    if (source.empty()) {
        assignError(error, "3DS save source path is empty");
        return false;
    }

    romx_save_scan_options_t scanOptions = ROMX_SAVE_SCAN_OPTIONS_INIT;
    scanOptions.platform_id = ROMX_PLATFORM_NINTENDO_3DS;
    scanOptions.format_id = ROMX_FORMAT_N3DS;
    scanOptions.launch_format_id = ROMX_LAUNCH_RAW_SINGLE_FILE;

    romx_error_t err{};
    const romx_result_t openResult = romx_save_catalog_open_path(
        source.string().c_str(), &scanOptions, catalog, &err);
    if (openResult != ROMX_OK) {
        assignError(error, errorText("romx_save_catalog_open_path", err,
                                     openResult));
        return false;
    }

    uint32_t count = 0;
    const romx_result_t countResult = romx_save_catalog_get_candidate_count(
        *catalog, &count, &err);
    if (countResult != ROMX_OK) {
        romx_save_catalog_close(*catalog);
        *catalog = nullptr;
        assignError(error, errorText("romx_save_catalog_get_candidate_count",
                                     err, countResult));
        return false;
    }
    if (candidateCount)
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
    if (result != ROMX_E_BUFFER_TOO_SMALL && result != ROMX_OK) {
        assignError(error, errorText(
            "romx_save_catalog_copy_candidate_source_path", err, result));
        return {};
    }
    if (required == 0 || required > static_cast<uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        assignError(error, "3DS save source path size is invalid");
        return {};
    }
    std::vector<char> buffer(static_cast<std::size_t>(required));
    result = romx_save_catalog_copy_candidate_source_path(
        catalog, candidateIndex, buffer.data(), required, &required, &err);
    if (result != ROMX_OK) {
        assignError(error, errorText(
            "romx_save_catalog_copy_candidate_source_path", err, result));
        return {};
    }
    return std::string(buffer.data());
}

std::string threeDsCandidateText(const char* value, std::size_t capacity,
                                 uint32_t size)
{
    if (value == nullptr || capacity == 0)
        return {};
    const std::size_t bounded = std::min<std::size_t>(size, capacity - 1U);
    return std::string(value, bounded);
}

std::vector<GameEntryAdapter::LocalSaveCandidate> listThreeDsSaveCandidates(
    const GameEntry& entry, const fs::path& source, std::string* error)
{
    std::vector<GameEntryAdapter::LocalSaveCandidate> candidates;
    if (!isThreeDs(entry)) {
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
    for (uint32_t index = 0; index < count; ++index) {
        romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
        romx_error_t err{};
        const romx_result_t result = romx_save_catalog_get_candidate(
            catalog, index, &candidate, &err);
        if (result != ROMX_OK) {
            assignError(error, errorText("romx_save_catalog_get_candidate",
                                         err, result));
            candidates.clear();
            break;
        }

        GameEntryAdapter::LocalSaveCandidate item;
        item.key = threeDsCandidateText(candidate.key, sizeof(candidate.key),
                                        candidate.key_size);
        item.displayName = threeDsCandidateText(
            candidate.display_name, sizeof(candidate.display_name),
            candidate.display_name_size);
        if (item.displayName.empty())
            item.displayName = item.key;
        item.titleId = threeDsCandidateText(candidate.title_id,
                                            sizeof(candidate.title_id),
                                            candidate.title_id_size);
        item.sourcePath = copyThreeDsCandidateSourcePath(catalog, index, error);
        if (item.sourcePath.empty()) {
            candidates.clear();
            break;
        }
        item.sourceFormat = candidate.source_format;
        item.grouping = candidate.grouping;
        item.scope = candidate.scope;
        item.extdataId = threeDsCandidateText(
            candidate.extdata_id, sizeof(candidate.extdata_id),
            candidate.extdata_id_size);
        item.fileCount = candidate.file_count;
        item.dataSize = candidate.data_size;
        item.isDirectory = (candidate.flags & ROMX_SAVE_CANDIDATE_IS_DIRECTORY) != 0;
        candidates.push_back(std::move(item));
    }
    romx_save_catalog_close(catalog);
    return candidates;
}

bool hasThreeDsSaveObject(const fs::path& destination,
                          const std::string& key)
{
    if (destination.empty() || key.empty())
        return false;
    romx_reader_t* reader = nullptr;
    romx_error_t err{};
    if (romx_reader_open_path(destination.string().c_str(), nullptr, &reader,
                              &err) != ROMX_OK)
        return false;
    uint32_t count = 0;
    const romx_result_t countResult =
        romx_reader_get_mutable_object_count(reader, &count, &err);
    bool found = false;
    if (countResult == ROMX_OK) {
        for (uint32_t index = 0; index < count; ++index) {
            romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
            if (romx_reader_get_mutable_object(reader, index, &object, &err) !=
                    ROMX_OK ||
                object.object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE)
                continue;
            if (std::string(object.key,
                            std::min<std::size_t>(object.key_size,
                                                  sizeof(object.key) - 1U)) == key) {
                found = true;
                break;
            }
        }
    }
    romx_reader_close(reader);
    return found;
}

SyncResult writeThreeDsSaveCatalog(const fs::path& source,
                                   const fs::path& destination,
                                   const std::string& preferredKey,
                                   uint32_t* writtenCount,
                                   std::string* error)
{
    if (writtenCount)
        *writtenCount = 0;
    if (source.empty() || destination.empty()) {
        assignError(error, "3DS SAVE source or ROMX destination is empty");
        return SyncResult::Failed;
    }
    if (!preferredKey.empty() && !validateSaveSlotKeyImpl(preferredKey, error))
        return SyncResult::Failed;

    romx_save_catalog_t* catalog = nullptr;
    uint32_t count = 0;
    if (!openThreeDsSaveCatalog(source, &catalog, &count, error))
        return SyncResult::Failed;
    if (count == 0) {
        romx_save_catalog_close(catalog);
        assignError(error, "未找到可识别的3DS本地存档: " + source.string());
        return SyncResult::Skipped;
    }

    uint32_t succeeded = 0;
    std::string firstError;
    for (uint32_t index = 0; index < count; ++index) {
        romx_save_candidate_info_t candidate = ROMX_SAVE_CANDIDATE_INFO_INIT;
        romx_error_t err{};
        const romx_result_t candidateResult = romx_save_catalog_get_candidate(
            catalog, index, &candidate, &err);
        if (candidateResult != ROMX_OK) {
            if (firstError.empty())
                firstError = errorText("romx_save_catalog_get_candidate", err,
                                       candidateResult);
            continue;
        }

        const std::string candidateKey = threeDsCandidateText(
            candidate.key, sizeof(candidate.key), candidate.key_size);
        // Keep the historical `libretro` key when it already exists, but use
        // the catalog's real folder/file key for a new single-save ROMX. This
        // preserves old containers while making a newly imported folder
        // visible as its own save name in the frontend.
        const bool reusePreferred = preferredKey.empty() ||
            preferredKey != "libretro" ||
            hasThreeDsSaveObject(destination, preferredKey);
        const std::string objectKey = count == 1 && reusePreferred &&
                                      !preferredKey.empty()
            ? preferredKey : candidateKey;
        if (!validateSaveSlotKeyImpl(objectKey, nullptr)) {
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
        const bool existing = hasThreeDsSaveObject(destination, objectKey);
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
        if (result != ROMX_OK) {
            if (firstError.empty())
                firstError = errorText("romx_save_catalog_write_candidate", err,
                                       result);
            continue;
        }
        ++succeeded;
    }
    romx_save_catalog_close(catalog);
    if (writtenCount)
        *writtenCount = succeeded;
    if (succeeded == count)
        return SyncResult::Success;
    if (succeeded != 0) {
        assignError(error, "仅写入 " + std::to_string(succeeded) + "/" +
                            std::to_string(count) + " 个3DS存档" +
                            (firstError.empty() ? std::string() :
                                "：" + firstError));
        return SyncResult::Failed;
    }
    assignError(error, firstError.empty() ? "没有3DS存档被写入 ROMX" : firstError);
    return SyncResult::Failed;
}

SyncResult extractPspSaveBundle(const romx_reader_t* reader,
                                const GameEntry& entry,
                                const char* key,
                                const std::string& requestedDirectory,
                                bool overwrite,
                                std::string* error)
{
    const std::string discId = pspDiscId(entry);
    if (discId.empty())
    {
        assignError(error, "PSP ROMX payload does not contain a valid DISC_ID");
        return SyncResult::Failed;
    }

    const fs::path root = pspSaveRoot();
    const std::optional<PspSaveDirectory> localDirectory =
        findPspSaveDirectory(root, discId);
    // First-use import must not replace an already existing native savedata
    // directory.  Explicit slot restore passes overwrite=true.
    if (!overwrite && localDirectory)
        return SyncResult::Skipped;

    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec)
    {
        assignError(error, "cannot create PSP savedata root: " + ec.message());
        return SyncResult::Failed;
    }

    const std::string suffix = pspTemporarySuffix();
    const fs::path staging = root / (".romx-psp-restore-" + suffix);
    fs::remove_all(staging, ec);
    ec.clear();
    if (!fs::create_directory(staging, ec) || ec)
    {
        assignError(error, "cannot create PSP restore staging directory: " +
                            ec.message());
        return SyncResult::Failed;
    }

    std::string targetDirectory;
    const BundleOutputMapper mapper = pspSaveOutputMapper(
        root, discId, requestedDirectory, targetDirectory);
    const auto validator = [discId](const fs::path& relative) {
        // The bundle is extracted into an empty staging tree.  Do not ask
        // for PARAM.SFO from a directory while that same bundle is still
        // being populated; entry order is not a validity condition.
        return pspSaveBundlePathHasDiscPrefix(relative, discId);
    };
    std::vector<fs::path> restoredPaths;
    const SyncResult result = extractBundle(
        reader, ROMX_MUTABLE_NAMESPACE_SAVE, key, staging, true, error,
        &restoredPaths, validator, mapper);
    if (result != SyncResult::Success || targetDirectory.empty())
    {
        fs::remove_all(staging, ec);
        return result == SyncResult::Success ? SyncResult::Skipped : result;
    }

    const fs::path stagedDirectory = staging / targetDirectory;
    const fs::path finalDirectory = root / targetDirectory;
    if (!fs::is_directory(stagedDirectory, ec) || ec ||
        !isWithinDirectory(root, finalDirectory))
    {
        fs::remove_all(staging, ec);
        assignError(error, "PSP ROMX savedata directory is invalid");
        return SyncResult::Failed;
    }

    // When a ROMX slot came from a different PSP savedata suffix, keep the
    // local PARAM.SFO identity while replacing the slot's actual save files;
    // PPSSPP uses SAVEDATA_DIRECTORY to validate the on-device folder name.
    if (localDirectory)
    {
        const std::optional<fs::path> localSfo = findPspSfoPath(localDirectory->path);
        if (localSfo)
        {
            const std::optional<fs::path> stagedSfo = findPspSfoPath(stagedDirectory);
            if (stagedSfo)
            {
                fs::remove(*stagedSfo, ec);
                if (ec)
                {
                    fs::remove_all(staging, ec);
                    assignError(error, "cannot prepare PSP savedata metadata: " +
                                        ec.message());
                    return SyncResult::Failed;
                }
            }
            fs::copy_file(*localSfo, stagedDirectory / localSfo->filename(),
                          fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                fs::remove_all(staging, ec);
                assignError(error, "cannot preserve PSP savedata metadata: " +
                                    ec.message());
                return SyncResult::Failed;
            }
        }
    }
    if (!isPspSaveDirectoryForDisc(stagedDirectory, discId))
    {
        fs::remove_all(staging, ec);
        assignError(error, "PSP ROMX slot does not contain valid savedata metadata");
        return SyncResult::Failed;
    }

    // PSP-specific staging and PARAM.SFO handling are complete. Reuse the
    // shared directory transaction for backup, atomic publish, and rollback.
    DirectoryRestoreTransaction restoreTransaction;
    const DirectoryRestoreResult prepared = prepareDirectoryRestoreWithStaging(
        finalDirectory, stagedDirectory, overwrite, suffix, restoreTransaction,
        error);
    if (prepared != DirectoryRestoreResult::Ready)
    {
        fs::remove_all(staging, ec);
        return prepared == DirectoryRestoreResult::Skipped
            ? SyncResult::Skipped : SyncResult::Failed;
    }

    if (!commitDirectoryRestore(restoreTransaction, error))
    {
        abortDirectoryRestore(restoreTransaction);
        fs::remove_all(staging, ec);
        return SyncResult::Failed;
    }
    fs::remove_all(staging, ec);
    return SyncResult::Success;
}

void updateCheatPathFromRestored(GameEntry& entry,
                                 const std::vector<fs::path>& restoredPaths)
{
    if (restoredPaths.empty())
        return;
    if (isPsp(entry))
    {
        const std::string discId = pspDiscId(entry);
        if (!discId.empty())
            entry.cheatPath = (pspCheatRoot() / (discId + ".ini")).string();
        return;
    }
    const auto preferred = std::find_if(
        restoredPaths.begin(), restoredPaths.end(),
        [&entry](const fs::path& restored) {
            if (entry.platform == static_cast<int>(enums::EmuPlatform::EmuNDS))
                return restored.filename() == "usrcheat.dat";
            return restored.extension() == ".cht";
        });
    entry.cheatPath = (preferred == restoredPaths.end()
        ? restoredPaths.front() : *preferred).string();
}

void importMutableObjects(const std::string& path, GameEntry& entry)
{
    romx_reader_t* reader = nullptr;
    romx_error_t err{};
    if (romx_reader_open_path(path.c_str(), nullptr, &reader, &err) != ROMX_OK)
        return;
    uint32_t count = 0;
    if (romx_reader_get_mutable_object_count(reader, &count, &err) != ROMX_OK)
    {
        romx_reader_close(reader);
        return;
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
        if (romx_reader_get_mutable_object(reader, index, &object, &err) != ROMX_OK)
            continue;
        if (object.object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE &&
            object.object_namespace != ROMX_MUTABLE_NAMESPACE_CHEAT)
            continue;
        // PSP and 3DS SAVE objects are restored only after an explicit user
        // action.  Their native layouts can contain multiple selectable
        // saves, so opening a ROMX must never overwrite the active local save.
        if ((isPsp(entry) || isThreeDs(entry)) &&
            object.object_namespace == ROMX_MUTABLE_NAMESPACE_SAVE)
            continue;
        const fs::path root(namespaceDirectory(entry, object.object_namespace));
        std::vector<fs::path> restoredPaths;
        const auto validator = bundlePathValidator(entry, object.object_namespace, root);
        const BundleOutputMapper outputMapper =
            object.object_namespace == ROMX_MUTABLE_NAMESPACE_SAVE
                ? batterySaveOutputMapper(entry)
                : BundleOutputMapper{};
        const SyncResult result = extractBundle(
            reader, object.object_namespace, object.key, root, false, nullptr,
            &restoredPaths, validator, outputMapper);
        if (result == SyncResult::Success &&
            object.object_namespace == ROMX_MUTABLE_NAMESPACE_CHEAT &&
            !restoredPaths.empty())
            updateCheatPathFromRestored(entry, restoredPaths);
    }
    romx_reader_close(reader);
}

SyncResult importBundleNamespace(GameEntry& entry,
                                 romx_mutable_namespace_t objectNamespace,
                                 bool overwrite, std::string* error,
                                 const std::string* requestedKey = nullptr)
{
    if (isPsp(entry) && pspDiscId(entry).empty())
    {
        assignError(error, "PSP ROMX payload does not contain a valid DISC_ID");
        return SyncResult::Failed;
    }
    romx_reader_t* reader = nullptr;
    romx_error_t err{};
    const romx_result_t openResult =
        romx_reader_open_path(entry.path.c_str(), nullptr, &reader, &err);
    if (openResult != ROMX_OK)
    {
        assignError(error, errorText("romx_reader_open_path", err, openResult));
        return SyncResult::Failed;
    }
    uint32_t count = 0;
    const romx_result_t countResult =
        romx_reader_get_mutable_object_count(reader, &count, &err);
    if (countResult != ROMX_OK)
    {
        romx_reader_close(reader);
        if (countResult == ROMX_E_MUTABLE_ABSENT)
            return SyncResult::Skipped;
        assignError(error, errorText("romx_reader_get_mutable_object_count", err,
                                     countResult));
        return SyncResult::Failed;
    }

    std::vector<std::string> keys;
    if (requestedKey && !requestedKey->empty())
    {
        // Keep the selected UTF-8 label verbatim.  libromx performs its
        // portable key comparison when opening the object, including the
        // documented ASCII case-folding behavior.
        keys.push_back(*requestedKey);
    }
    else
    {
        keys.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
            const romx_result_t objectResult =
                romx_reader_get_mutable_object(reader, index, &object, &err);
            if (objectResult != ROMX_OK)
                continue;
            if (object.object_namespace == objectNamespace)
                keys.emplace_back(object.key, object.key_size);
        }
        std::stable_sort(keys.begin(), keys.end(),
                         [&entry](const std::string& left, const std::string& right) {
                             const int leftPriority = mutableKeyPriority(entry, left);
                             const int rightPriority = mutableKeyPriority(entry, right);
                             if (leftPriority != rightPriority)
                                 return leftPriority < rightPriority;
                             return left < right;
                         });
    }

    SyncResult result = SyncResult::Skipped;
    const bool threeDsSave = isThreeDs(entry) &&
        objectNamespace == ROMX_MUTABLE_NAMESPACE_SAVE;
    const fs::path root = threeDsSave
        ? threeDsNativeSdRoot(entry)
        : fs::path(namespaceDirectory(entry, objectNamespace));
    const auto validator = bundlePathValidator(entry, objectNamespace, root);
    for (const std::string& key : keys)
    {
        std::string objectKey = key;
        std::string requestedSlot;
        const bool hasBundleSelector =
            parseBundleSlotSelector(key, objectKey, requestedSlot);
        std::string bundleError;
        std::vector<fs::path> restoredPaths;
        romx_mutable_save_layout_info_t saveLayout =
            ROMX_MUTABLE_SAVE_LAYOUT_INFO_INIT;
        bool hasSaveLayout = false;
        if (threeDsSave)
        {
            // libromx owns the 3DS layout decision.  The adapter only turns
            // the resulting semantic scope into the native Azahar path.
            romx_mutable_bundle_t* layoutBundle = nullptr;
            romx_error_t layoutError{};
            if (romx_mutable_bundle_open(reader, ROMX_MUTABLE_NAMESPACE_SAVE,
                                         objectKey.c_str(), nullptr,
                                         &layoutBundle, &layoutError) == ROMX_OK)
            {
                hasSaveLayout = romx_mutable_bundle_get_save_layout(
                    layoutBundle, &saveLayout, &layoutError) == ROMX_OK;
                romx_mutable_bundle_close(layoutBundle);
            }
        }
        const BundleOutputMapper outputMapper =
            threeDsSave
                ? threeDsSaveOutputMapper(entry,
                    hasSaveLayout ? &saveLayout : nullptr)
                : objectNamespace == ROMX_MUTABLE_NAMESPACE_SAVE
                    ? batterySaveOutputMapper(entry)
                    : BundleOutputMapper{};
        const fs::path transactionDirectory = threeDsSave
            ? threeDsSaveTransactionDirectory(
                GameEntryAdapter::resolveThreeDsTitleId(entry), saveLayout)
            : fs::path{};
        if (isPsp(entry) && objectNamespace == ROMX_MUTABLE_NAMESPACE_SAVE)
        {
            result = extractPspSaveBundle(reader, entry, objectKey.c_str(),
                                          hasBundleSelector ? requestedSlot
                                                         : std::string{},
                                          overwrite, &bundleError);
        }
        else
        {
            result = extractBundle(reader, objectNamespace, objectKey.c_str(), root,
                                   overwrite, &bundleError, &restoredPaths,
                                   validator, outputMapper,
                                   hasBundleSelector ? &requestedSlot : nullptr,
                                   transactionDirectory.empty()
                                       ? nullptr : &transactionDirectory);
        }
        if (result == SyncResult::Success)
        {
            if (objectNamespace == ROMX_MUTABLE_NAMESPACE_CHEAT &&
                !restoredPaths.empty())
                updateCheatPathFromRestored(entry, restoredPaths);
            break;
        }
        if (result == SyncResult::Failed)
        {
            assignError(error, bundleError);
            break;
        }
    }
    romx_reader_close(reader);
    return result;
}

std::string titleFallback(const std::string& path)
{
    std::string stem = fs::path(path).stem().string();
    return stem.empty() ? std::string("ROMX") : stem;
}

bool isSaveStateArtifact(const fs::path& path)
{
    const std::string name = path.filename().string();
    const std::size_t marker = name.rfind(".ss");
    if (marker != std::string::npos && marker + 3U < name.size())
    {
        bool digits = true;
        for (std::size_t i = marker + 3U; i < name.size(); ++i)
            digits = digits && std::isdigit(static_cast<unsigned char>(name[i])) != 0;
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

// The generic cores persist their battery-backed RAM under the ROM stem.  A
// save directory may also contain savestates and their PNG thumbnails, so a
// ROMX SAVE bundle must not recursively copy the whole directory.  Genesis
// Plus GX is the one bundled core using the libretro `.srm` convention; the
// other built-in cores write `<stem>.sav` (including mGBA and melonDS).
fs::path gameBatterySavePath(const GameEntry& entry)
{
    const fs::path root(namespaceDirectory(entry, ROMX_MUTABLE_NAMESPACE_SAVE));
    const std::string stem = beiklive::tools::getFileNameWithoutExtension(entry.path);
    if (root.empty() || stem.empty())
        return {};
    const bool genesis =
        entry.platform == static_cast<int>(enums::EmuPlatform::EmuGenesis);
    return root / (stem + (genesis ? ".srm" : ".sav"));
}

BundleOutputMapper batterySaveOutputMapper(const GameEntry& entry)
{
    // These are the cores whose save path is fully controlled by GBAStation:
    // all of them use savePath/<ROM stem>/{stem}.sav, except Genesis Plus GX
    // which uses .srm.  PSP, 3DS, Arcade and the other external cores have
    // core-specific identities (DISC_ID, Title ID or driver name), so their
    // bundle paths must not be guessed from the ROMX filename here.
    switch (static_cast<enums::EmuPlatform>(entry.platform))
    {
    case enums::EmuPlatform::EmuGBA:
    case enums::EmuPlatform::EmuGBC:
    case enums::EmuPlatform::EmuGB:
    case enums::EmuPlatform::EmuNES:
    case enums::EmuPlatform::EmuSNES:
    case enums::EmuPlatform::EmuNDS:
    case enums::EmuPlatform::EmuGenesis:
        break;
    default:
        return {};
    }

    const fs::path target = gameBatterySavePath(entry);
    if (target.empty())
        return {};

    std::string expectedExtension = target.extension().string();
    std::transform(expectedExtension.begin(), expectedExtension.end(),
                   expectedExtension.begin(), [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });

    // The current writer emits one stem-matched battery file.  For older
    // bundles that contain auxiliary files, select the battery-looking entry
    // by extension and discard savestates/thumbnails instead of allowing the
    // mutable path to choose an arbitrary local filename.
    bool selected = false;
    return [target, expectedExtension, selected](const fs::path& relative,
                                                  uint32_t /*index*/,
                                                  uint32_t count) mutable
        -> std::optional<fs::path> {
        if (selected)
            return std::nullopt;

        std::string extension = relative.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        // A legacy SAVE bundle may have been built from the whole frontend
        // save directory.  Never reinterpret a savestate, playtime marker or
        // screenshot as battery RAM just because it is the only entry.
        if (isSaveStateArtifact(relative) || extension == ".png" ||
            extension == ".jpg" || extension == ".jpeg" || extension == ".state")
            return std::nullopt;
        if (count != 1U && extension != expectedExtension)
            return std::nullopt;

        selected = true;
        return target.filename();
    };
}

bool collectPspDirectoryFiles(const fs::path& root,
                              const PspSaveDirectory& directory,
                              std::vector<std::string>& relativePaths,
                              std::vector<std::string>& sourcePaths)
{
    std::error_code ec;
    fs::recursive_directory_iterator iterator(
        directory.path, fs::directory_options::skip_permission_denied, ec);
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

bool collectBundleFiles(const GameEntry& entry, romx_mutable_namespace_t ns,
                        std::vector<std::string>& relativePaths,
                        std::vector<std::string>& sourcePaths)
{
    relativePaths.clear();
    sourcePaths.clear();
    std::error_code ec;
    if (isPsp(entry))
    {
        const std::string discId = pspDiscId(entry);
        if (discId.empty())
            return false;
        if (ns == ROMX_MUTABLE_NAMESPACE_CHEAT)
        {
            const fs::path source = pspCheatRoot() / (discId + ".ini");
            if (!fs::is_regular_file(source, ec) || fs::is_symlink(source, ec))
                return false;
            relativePaths.push_back(source.filename().generic_string());
            sourcePaths.push_back(source.string());
            return true;
        }
        if (ns == ROMX_MUTABLE_NAMESPACE_SAVE)
        {
            const fs::path root = pspSaveRoot();
            const std::optional<PspSaveDirectory> directory =
                findPspSaveDirectory(root, discId);
            if (!directory)
                return false;
            // A local PPSSPP installation has one active savedata directory.
            // ROMX slots are separated at the mutable-object/slot layer; do
            // not merge every local DISC_ID-prefixed directory into one slot.
            return collectPspDirectoryFiles(root, *directory,
                                            relativePaths, sourcePaths);
        }
    }
    if (ns == ROMX_MUTABLE_NAMESPACE_SAVE)
    {
        // SAVE is deliberately a single, stem-matched battery file for the
        // generic cores.  In particular, do not include `.ss*` savestates,
        // their `.png` thumbnails, or another game's files in the directory.
        const fs::path source = gameBatterySavePath(entry);
        if (!fs::is_regular_file(source, ec) || fs::is_symlink(source, ec))
            return false;
        relativePaths.push_back(source.filename().generic_string());
        sourcePaths.push_back(source.string());
        return true;
    }
    if (ns == ROMX_MUTABLE_NAMESPACE_CHEAT)
    {
        const fs::path source = resolvedCheatPath(entry);
        if (!fs::is_directory(source, ec))
        {
            if (!fs::is_regular_file(source, ec) || fs::is_symlink(source, ec))
                return false;
            relativePaths.push_back(source.filename().generic_string());
            sourcePaths.push_back(source.string());
            return true;
        }
    }

    const fs::path root(namespaceDirectory(entry, ns));
    if (!fs::is_directory(root, ec))
        return false;
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
        if (ns == ROMX_MUTABLE_NAMESPACE_SAVE && isSaveStateArtifact(item.path()))
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

bool mutableObjectExists(const GameEntry& entry,
                         romx_mutable_namespace_t objectNamespace,
                         const char* key)
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
            if (std::strlen(key) == keySize &&
                std::memcmp(object.key, key, keySize) == 0)
            {
                found = true;
                break;
            }
        }
    }
    romx_reader_close(reader);
    return found;
}

SyncResult writeBundlePathEntries(const GameEntry& entry,
                                  romx_mutable_namespace_t ns,
                                  const char* key,
                                  const std::vector<std::string>& relativePaths,
                                  const std::vector<std::string>& sourcePaths,
                                  std::string* error)
{
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
        ns, files.data(), static_cast<uint32_t>(files.size()), nullptr,
        &measuredSize, &err);
    if (measureResult != ROMX_OK)
    {
        assignError(error, errorText(
            "romx_mutable_bundle_measure_path_entries", err, measureResult));
        return SyncResult::Failed;
    }

    romx_mutable_write_options_t options = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
    const bool existing = mutableObjectExists(entry, ns, key);
    const bool allowGrowth = !existing && isPsp(entry) &&
        ns == ROMX_MUTABLE_NAMESPACE_SAVE && relativePaths.size() > 1U;
    // Existing objects keep their original extent. New fixed-size objects use
    // the measured RMBL size; PSP multi-file objects get a bounded growth
    // reserve and fall back to the exact size if that reserve does not fit.
    options.data_capacity = existing ? 0U :
        expandedMutableBundleCapacity(measuredSize, allowGrowth);
    romx_mutable_object_info_t written = ROMX_MUTABLE_OBJECT_INFO_INIT;
    romx_result_t result = romx_mutable_bundle_write_path_entries(
        entry.path.c_str(), ns, key, files.data(),
        static_cast<uint32_t>(files.size()), nullptr, &options, &written, &err);
    if (result == ROMX_E_MUTABLE_NO_SPACE && !existing &&
        options.data_capacity != measuredSize)
    {
        options.data_capacity = measuredSize;
        err = romx_error_t{};
        result = romx_mutable_bundle_write_path_entries(
            entry.path.c_str(), ns, key, files.data(),
            static_cast<uint32_t>(files.size()), nullptr, &options, &written,
            &err);
    }
    if (result != ROMX_OK)
    {
        assignError(error, errorText("romx_mutable_bundle_write_path_entries", err, result));
        return SyncResult::Failed;
    }
    return SyncResult::Success;
}

SyncResult writeBundle(const GameEntry& entry, romx_mutable_namespace_t ns,
                       const char* key, std::string* error)
{
    if (isThreeDs(entry) && ns == ROMX_MUTABLE_NAMESPACE_SAVE)
        return writeThreeDsSaveCatalog(
            namespaceDirectory(entry, ROMX_MUTABLE_NAMESPACE_SAVE),
            entry.path, key ? std::string(key) : std::string(), nullptr, error);
    if (isPsp(entry) && pspDiscId(entry).empty())
    {
        assignError(error, "PSP ROMX payload does not contain a valid DISC_ID");
        return SyncResult::Failed;
    }
    std::vector<std::string> relativePaths;
    std::vector<std::string> sourcePaths;
    if (!collectBundleFiles(entry, ns, relativePaths, sourcePaths))
        return SyncResult::Skipped;
    return writeBundlePathEntries(entry, ns, key, relativePaths, sourcePaths, error);
}

bool collectRegularBundleFiles(const fs::path& root,
                               std::vector<std::string>& relativePaths,
                               std::vector<std::string>& sourcePaths)
{
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

SyncResult mergeSaveBundleSlot(const GameEntry& entry,
                               const std::string& objectKey,
                               const std::string& selectedSlot,
                               std::string* error)
{
    const fs::path source = gameBatterySavePath(entry);
    std::error_code ec;
    if (!fs::is_regular_file(source, ec) || fs::is_symlink(source, ec))
        return SyncResult::Skipped;

    const fs::path root = namespaceDirectory(entry, ROMX_MUTABLE_NAMESPACE_SAVE);
    const fs::path staging = root / (".romx-save-merge-" + pspTemporarySuffix());
    fs::remove_all(staging, ec);
    ec.clear();
    if (!fs::create_directories(staging, ec) || ec)
    {
        assignError(error, "cannot create SAVE merge staging directory: " +
                            ec.message());
        return SyncResult::Failed;
    }

    romx_reader_t* reader = nullptr;
    romx_error_t err{};
    if (romx_reader_open_path(entry.path.c_str(), nullptr, &reader, &err) != ROMX_OK)
    {
        fs::remove_all(staging, ec);
        assignError(error, errorText("romx_reader_open_path", err, ROMX_E_IO));
        return SyncResult::Failed;
    }

    romx_mutable_bundle_t* bundle = nullptr;
    const romx_result_t bundleResult = romx_mutable_bundle_open(
        reader, ROMX_MUTABLE_NAMESPACE_SAVE, objectKey.c_str(), nullptr,
        &bundle, &err);
    if (bundleResult != ROMX_OK)
    {
        romx_reader_close(reader);
        fs::remove_all(staging, ec);
        return bundleResult == ROMX_E_MUTABLE_ENTRY ||
                   bundleResult == ROMX_E_MUTABLE_BUNDLE
            ? SyncResult::Skipped : SyncResult::Failed;
    }

    std::vector<std::string> selectedPaths;
    uint32_t slotCount = 0;
    if (romx_mutable_bundle_get_save_slot_count(bundle, &slotCount, &err) != ROMX_OK)
    {
        romx_mutable_bundle_close(bundle);
        romx_reader_close(reader);
        fs::remove_all(staging, ec);
        assignError(error, errorText("romx_mutable_bundle_get_save_slot_count",
                                     err, ROMX_E_MUTABLE_BUNDLE));
        return SyncResult::Failed;
    }
    for (uint32_t slotIndex = 0; slotIndex < slotCount; ++slotIndex)
    {
        romx_mutable_save_slot_info_t info = ROMX_MUTABLE_SAVE_SLOT_INFO_INIT;
        if (romx_mutable_bundle_get_save_slot(bundle, slotIndex, &info, &err) != ROMX_OK)
            continue;
        if (std::string(info.key, info.key_size) != selectedSlot)
            continue;
        for (uint32_t entryIndex = 0; entryIndex < info.entry_count; ++entryIndex)
        {
            romx_mutable_bundle_entry_info_t entryInfo =
                ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
            if (romx_mutable_bundle_get_save_slot_entry(bundle, slotIndex,
                                                        entryIndex, &entryInfo,
                                                        &err) != ROMX_OK)
            {
                romx_mutable_bundle_close(bundle);
                romx_reader_close(reader);
                fs::remove_all(staging, ec);
                assignError(error, errorText(
                    "romx_mutable_bundle_get_save_slot_entry", err,
                    ROMX_E_MUTABLE_BUNDLE));
                return SyncResult::Failed;
            }
            selectedPaths.emplace_back(entryInfo.path,
                                       entryInfo.path_size);
        }
        break;
    }
    if (selectedPaths.empty())
    {
        romx_mutable_bundle_close(bundle);
        romx_reader_close(reader);
        fs::remove_all(staging, ec);
        return SyncResult::Skipped;
    }

    const SyncResult extracted = extractBundle(
        reader, ROMX_MUTABLE_NAMESPACE_SAVE, objectKey.c_str(), staging, true,
        error);
    romx_mutable_bundle_close(bundle);
    romx_reader_close(reader);
    if (extracted != SyncResult::Success)
    {
        fs::remove_all(staging, ec);
        return extracted;
    }

    const std::string expectedExtension = [&source]() {
        std::string value = source.extension().string();
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        return value;
    }();
    std::string replacement = selectedPaths.front();
    for (const std::string& selectedPath : selectedPaths)
    {
        std::string extension = fs::path(selectedPath).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (extension == expectedExtension)
        {
            replacement = selectedPath;
            break;
        }
    }
    const fs::path stagedReplacement = staging / fs::path(replacement);
    fs::create_directories(stagedReplacement.parent_path(), ec);
    if (ec || !fs::copy_file(source, stagedReplacement,
                             fs::copy_options::overwrite_existing, ec) || ec)
    {
        fs::remove_all(staging, ec);
        assignError(error, "cannot update selected SAVE slot: " + ec.message());
        return SyncResult::Failed;
    }

    std::vector<std::string> relativePaths;
    std::vector<std::string> sourcePaths;
    if (!collectRegularBundleFiles(staging, relativePaths, sourcePaths))
    {
        fs::remove_all(staging, ec);
        return SyncResult::Skipped;
    }
    const SyncResult written = writeBundlePathEntries(
        entry, ROMX_MUTABLE_NAMESPACE_SAVE, objectKey.c_str(), relativePaths,
        sourcePaths, error);
    fs::remove_all(staging, ec);
    return written;
}

bool pspMutableBundleExists(const std::string& path, const std::string& key)
{
    romx_reader_t* reader = nullptr;
    romx_error_t err{};
    if (romx_reader_open_path(path.c_str(), nullptr, &reader, &err) != ROMX_OK)
        return false;
    romx_mutable_bundle_t* bundle = nullptr;
    const romx_result_t result = romx_mutable_bundle_open(
        reader, ROMX_MUTABLE_NAMESPACE_SAVE, key.c_str(), nullptr, &bundle, &err);
    if (result == ROMX_OK)
        romx_mutable_bundle_close(bundle);
    romx_reader_close(reader);
    return result == ROMX_OK;
}

SyncResult mergePspSaveBundle(const GameEntry& entry,
                              const std::string& objectKey,
                              const std::string& selectedDirectory,
                              std::string* error)
{
    const std::string discId = pspDiscId(entry);
    const fs::path root = pspSaveRoot();
    const std::optional<PspSaveDirectory> localDirectory =
        findPspSaveDirectory(root, discId);
    if (!localDirectory)
        return SyncResult::Skipped;

    std::error_code ec;
    const fs::path staging = root /
        (".romx-psp-merge-" + pspTemporarySuffix());
    fs::remove_all(staging, ec);
    ec.clear();
    if (!fs::create_directory(staging, ec) || ec)
    {
        assignError(error, "cannot create PSP merge staging directory: " +
                            ec.message());
        return SyncResult::Failed;
    }

    romx_reader_t* reader = nullptr;
    romx_error_t err{};
    if (romx_reader_open_path(entry.path.c_str(), nullptr, &reader, &err) != ROMX_OK)
    {
        fs::remove_all(staging, ec);
        assignError(error, errorText("romx_reader_open_path", err, ROMX_E_IO));
        return SyncResult::Failed;
    }
    const auto validator = [discId](const fs::path& relative) {
        return pspSaveBundlePathHasDiscPrefix(relative, discId);
    };
    const SyncResult extracted = extractBundle(
        reader, ROMX_MUTABLE_NAMESPACE_SAVE, objectKey.c_str(), staging, true,
        error, nullptr, validator);
    romx_reader_close(reader);
    if (extracted != SyncResult::Success)
    {
        fs::remove_all(staging, ec);
        return extracted;
    }

    const fs::path selectedPath = staging / selectedDirectory;
    if (!isWithinDirectory(staging, selectedPath))
    {
        fs::remove_all(staging, ec);
        assignError(error, "PSP ROMX slot directory escapes staging root");
        return SyncResult::Failed;
    }
    const std::optional<fs::path> selectedSfo = findPspSfoPath(selectedPath);
    const fs::path preservedSfo = staging / ".romx-selected-param.sfo";
    if (selectedSfo)
    {
        fs::copy_file(*selectedSfo, preservedSfo,
                      fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            fs::remove_all(staging, ec);
            assignError(error, "cannot preserve ROMX savedata metadata: " +
                                ec.message());
            return SyncResult::Failed;
        }
    }
    fs::remove_all(selectedPath, ec);
    if (ec)
    {
        fs::remove_all(staging, ec);
        assignError(error, "cannot replace PSP ROMX slot directory: " + ec.message());
        return SyncResult::Failed;
    }
    if (selectedSfo)
    {
        fs::create_directories(selectedPath, ec);
        if (!ec)
            fs::copy_file(preservedSfo,
                          selectedPath / selectedSfo->filename(),
                          fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            fs::remove_all(staging, ec);
            assignError(error, "cannot restore ROMX savedata metadata: " +
                                ec.message());
            return SyncResult::Failed;
        }
        fs::remove(preservedSfo, ec);
        ec.clear();
    }

    std::vector<std::string> relativePaths;
    std::vector<std::string> sourcePaths;
    (void)collectRegularBundleFiles(staging, relativePaths, sourcePaths);

    std::vector<std::string> localRelativePaths;
    std::vector<std::string> localSourcePaths;
    if (!collectPspDirectoryFiles(root, *localDirectory,
                                  localRelativePaths, localSourcePaths))
    {
        fs::remove_all(staging, ec);
        return SyncResult::Skipped;
    }
    for (std::size_t index = 0; index < localRelativePaths.size(); ++index)
    {
        fs::path relative(localRelativePaths[index]);
        auto component = relative.begin();
        if (component == relative.end())
            continue;
        if (selectedSfo && localDirectory->name != selectedDirectory &&
            isPspSfoFilename(relative))
            continue;
        fs::path mapped = selectedDirectory;
        ++component;
        for (; component != relative.end(); ++component)
            mapped /= *component;
        relativePaths.push_back(mapped.generic_string());
        sourcePaths.push_back(localSourcePaths[index]);
    }

    const SyncResult written = relativePaths.empty()
        ? SyncResult::Skipped
        : writeBundlePathEntries(entry, ROMX_MUTABLE_NAMESPACE_SAVE,
                                 objectKey.c_str(), relativePaths,
                                 sourcePaths, error);
    fs::remove_all(staging, ec);
    return written;
}

SyncResult writePspSaveBundle(const GameEntry& entry,
                              const std::string& objectKey,
                              const std::string& selectedDirectory,
                              std::string* error)
{
    if (pspMutableBundleExists(entry.path, objectKey))
    {
        const std::optional<PspSaveDirectory> localDirectory =
            findPspSaveDirectory(pspSaveRoot(), pspDiscId(entry));
        if (!localDirectory)
            return SyncResult::Skipped;
        // A named selector identifies one directory in an existing object.
        // For the implicit/default export, replace the local directory of an
        // existing aggregate as well so old multi-directory ROMX files keep
        // their other logical slots.
        const std::string directory = selectedDirectory.empty()
            ? localDirectory->name : selectedDirectory;
        return mergePspSaveBundle(entry, objectKey, directory, error);
    }
    return writeBundle(entry, ROMX_MUTABLE_NAMESPACE_SAVE,
                       objectKey.c_str(), error);
}

std::vector<GameEntryAdapter::SaveSlot> enumerateSaveSlots(
    const GameEntry& entry, std::string* error)
{
    std::vector<GameEntryAdapter::SaveSlot> slots;
    if (!isRomxPath(entry.path))
        return slots;

    romx_reader_t* reader = nullptr;
    romx_error_t err{};
    const romx_result_t openResult =
        romx_reader_open_path(entry.path.c_str(), nullptr, &reader, &err);
    if (openResult != ROMX_OK)
    {
        assignError(error, errorText("romx_reader_open_path", err, openResult));
        return slots;
    }

    uint32_t objectCount = 0;
    const romx_result_t countResult =
        romx_reader_get_mutable_object_count(reader, &objectCount, &err);
    if (countResult != ROMX_OK)
    {
        romx_reader_close(reader);
        if (countResult != ROMX_E_MUTABLE_ABSENT)
            assignError(error, errorText("romx_reader_get_mutable_object_count",
                                         err, countResult));
        return slots;
    }

    for (uint32_t index = 0; index < objectCount; ++index)
    {
        romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
        if (romx_reader_get_mutable_object(reader, index, &object, &err) != ROMX_OK ||
            object.object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE ||
            object.key_size == 0)
            continue;

        GameEntryAdapter::SaveSlot slot;
        slot.key.assign(object.key,
                        std::min<std::size_t>(object.key_size, sizeof(object.key) - 1U));
        if (!validateSaveSlotKeyImpl(slot.key, nullptr))
            continue;
        slot.displayName = slot.key;
        slot.dataSize = object.data_size;
        slot.generation = object.generation;

        romx_mutable_bundle_t* bundle = nullptr;
        const romx_result_t bundleResult = romx_mutable_bundle_open(
            reader, ROMX_MUTABLE_NAMESPACE_SAVE, slot.key.c_str(), nullptr,
            &bundle, &err);
        if (bundleResult != ROMX_OK)
            continue; // Opaque SAVE objects are not selectable by this UI.

        uint32_t saveSlotCount = 0;
        if (romx_mutable_bundle_get_save_slot_count(bundle, &saveSlotCount,
                                                     &err) != ROMX_OK)
        {
            romx_mutable_bundle_close(bundle);
            continue;
        }
        const std::string discId = isPsp(entry) ? pspDiscId(entry) : std::string{};
        for (uint32_t slotIndex = 0; slotIndex < saveSlotCount; ++slotIndex)
        {
            romx_mutable_save_slot_info_t saveInfo =
                ROMX_MUTABLE_SAVE_SLOT_INFO_INIT;
            if (romx_mutable_bundle_get_save_slot(bundle, slotIndex, &saveInfo,
                                                  &err) != ROMX_OK)
                continue;
            if (isPsp(entry) &&
                normalizePspDiscId(std::string(saveInfo.key, saveInfo.key_size))
                    .rfind(discId, 0) != 0)
                continue;
            GameEntryAdapter::SaveSlot save;
            save.key = slot.key;
            save.selectionKey = saveSlotCount > 1U || isPsp(entry)
                ? makeBundleSlotSelector(slot.key,
                    std::string(saveInfo.key, saveInfo.key_size)) : std::string{};
            save.displayName = isPsp(entry)
                ? std::string(saveInfo.key, saveInfo.key_size)
                : (saveSlotCount > 1U
                    ? slot.key + " / " + std::string(saveInfo.key,
                                                       saveInfo.key_size)
                    : slot.key);
            save.dataSize = saveInfo.data_size;
            save.generation = slot.generation;
            save.entryCount = saveInfo.entry_count;
            if (saveInfo.entry_count != 0U)
            {
                romx_mutable_bundle_entry_info_t bundleEntry =
                    ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
                if (romx_mutable_bundle_get_save_slot_entry(bundle, slotIndex, 0U,
                                                            &bundleEntry, &err) == ROMX_OK)
                {
                    save.entryPath = bundleEntry.path;
                }
            }
            if (save.entryCount != 0U)
                slots.push_back(std::move(save));
        }
        romx_mutable_bundle_close(bundle);
    }
    romx_reader_close(reader);

    std::stable_sort(slots.begin(), slots.end(), [&entry](const auto& left,
                                                           const auto& right) {
        const int leftPriority = mutableKeyPriority(entry, left.key);
        const int rightPriority = mutableKeyPriority(entry, right.key);
        if (leftPriority != rightPriority)
            return leftPriority < rightPriority;
        return left.key < right.key;
    });
    return slots;
}
} // namespace

std::string GameEntryAdapter::payloadCacheDirectory()
{
    return (fs::path(beiklive::path::cachePath()) / "romx" / "payloads").string();
}

std::string GameEntryAdapter::nativeSaveDirectory(const GameEntry& entry)
{
    if (!isThreeDs(entry))
        return {};
    return threeDsNativeSaveDirectory(entry).string();
}

std::string GameEntryAdapter::resolveThreeDsTitleId(const GameEntry& entry)
{
    if (!isThreeDs(entry))
        return {};

    std::string titleId = beiklive::three_ds::resolveTitleId(
        entry.threeDsTitleId, entry.path);
    if (titleId.empty())
        titleId = readThreeDsTitleIdFromRomx(entry.path);
    return titleId;
}

bool GameEntryAdapter::apply(const std::string& path, GameEntry& entry,
                             const Options& options, std::string* error)
{
    if (!isRomxPath(path))
        return false;
    Info info;
    if (!readInfo(path, info, error))
        return false;

    const std::string oldTitle = entry.title;
    entry.path = path;
    if (info.platform != static_cast<int>(enums::EmuPlatform::NONE))
        entry.platform = info.platform;
    if (entry.platform == static_cast<int>(enums::EmuPlatform::NONE))
        entry.platform = static_cast<int>(enums::EmuPlatform::NONE);
    entry.core = NormalizeCoreId(entry.platform, entry.core.empty()
        ? GetDefaultCoreId(entry.platform) : entry.core);
    if (!options.preserveUserTitle || oldTitle.empty() || oldTitle == titleFallback(path))
        entry.title = info.title.empty() ? titleFallback(path) : info.title;
    if (entry.title.empty())
        entry.title = info.title.empty() ? titleFallback(path) : info.title;
    if (entry.savePath.empty())
        entry.savePath = beiklive::tools::defaultGameSavePath(entry.platform, path);
    if (entry.screenShotPath.empty())
        entry.screenShotPath = beiklive::path::screenshotPath();
    if (entry.logoPath.empty())
        entry.logoPath = beiklive::tools::getDefaultLogoPath(
            static_cast<enums::EmuPlatform>(entry.platform), path);
    if (options.extractCover && info.hasCover)
    {
        std::string coverPath;
        if (extractCover(path, coverPath))
            entry.logoPath = coverPath;
    }
    if (info.crc32 != 0)
        entry.crc32 = static_cast<int32_t>(info.crc32);
    nlohmann::json metadata = nlohmann::json::object();
    if (!info.metadataJson.empty())
    {
        const nlohmann::json parsed = nlohmann::json::parse(info.metadataJson, nullptr, false);
        if (parsed.is_object())
            metadata = parsed;
    }
    entry.romx = nlohmann::json{
        {"format", "0.2.0"},
        {"platform_id", info.platformId},
        {"launch_format_id", info.launchFormatId},
        {"entrypoint_format_id", info.entrypointFormatId},
        {"entrypoint", info.entrypointPath},
        {"entrypoint_size", info.entrypointSize},
        {"entry_count", info.entryCount},
        {"multi_file", info.multiFile},
        {"metadata", std::move(metadata)},
        {"cover_embedded", info.hasCover},
        {"cover_size", info.coverSize},
        {"payload_cache", payloadCacheDirectory()}
    };

    if (isPsp(entry))
    {
        std::string discId = normalizePspDiscId(info.serial);
        if (discId.empty())
            discId = readPspDiscIdFromRomx(path);
        if (!discId.empty())
        {
            // Keep the payload-derived identity with the database record so
            // batch operations do not need to inspect the ISO repeatedly.
            entry.romx["psp_disc_id"] = discId;
            entry.cheatPath = (pspCheatRoot() / (discId + ".ini")).string();
        }
    }
    else if (isThreeDs(entry))
    {
        std::string titleId = beiklive::three_ds::normalizeTitleId(info.serial);
        if (titleId.empty())
            titleId = readThreeDsTitleIdFromRomx(path);
        if (!titleId.empty())
            entry.threeDsTitleId = titleId;
    }

    std::error_code ec;
    fs::create_directories(isPsp(entry) ? pspSaveRoot() : fs::path(entry.savePath), ec);
    if (options.importMutable)
        importMutableObjects(path, entry);
    (void)readStats(path, entry);
    return true;
}

bool GameEntryAdapter::apply(const std::string& path, GameEntry& entry,
                             std::string* error)
{
    return apply(path, entry, Options{}, error);
}

SyncResult GameEntryAdapter::restoreSave(GameEntry& entry, std::string* error)
{
    if (!isRomxPath(entry.path))
        return SyncResult::Skipped;
    return importBundleNamespace(entry, ROMX_MUTABLE_NAMESPACE_SAVE, true, error);
}

SyncResult GameEntryAdapter::restoreSave(GameEntry& entry, const std::string& key,
                                         std::string* error)
{
    if (!isRomxPath(entry.path))
        return SyncResult::Skipped;
    std::string ignoredObjectKey;
    std::string ignoredSlotKey;
    const bool internalBundleSelector =
        parseBundleSlotSelector(key, ignoredObjectKey, ignoredSlotKey);
    if (!internalBundleSelector && !validateSaveSlotKeyImpl(key, error))
        return SyncResult::Failed;
    return importBundleNamespace(entry, ROMX_MUTABLE_NAMESPACE_SAVE, true, error,
                                 &key);
}

SyncResult GameEntryAdapter::exportSave(const GameEntry& entry, std::string* error)
{
    if (!isRomxPath(entry.path))
        return SyncResult::Skipped;
    if (isPsp(entry))
        return writePspSaveBundle(entry, mutableBundleKey(entry), {}, error);
    return writeBundle(entry, ROMX_MUTABLE_NAMESPACE_SAVE,
                       mutableBundleKey(entry), error);
}

SyncResult GameEntryAdapter::exportSave(const GameEntry& entry, const std::string& key,
                                        std::string* error)
{
    if (!isRomxPath(entry.path))
        return SyncResult::Skipped;
    std::string objectKey;
    std::string slotKey;
    if (parseBundleSlotSelector(key, objectKey, slotKey))
    {
        if (isPsp(entry))
            return writePspSaveBundle(entry, objectKey, slotKey, error);
        if (isThreeDs(entry))
            return writeThreeDsSaveCatalog(
                namespaceDirectory(entry, ROMX_MUTABLE_NAMESPACE_SAVE),
                entry.path, objectKey, nullptr, error);
        return mergeSaveBundleSlot(entry, objectKey, slotKey, error);
    }
    if (!validateSaveSlotKeyImpl(key, error))
        return SyncResult::Failed;
    if (isPsp(entry))
    {
        // A plain key is a user-facing mutable object label and creates or
        // replaces one logical PSP slot.
        return writePspSaveBundle(entry, key, {}, error);
    }
    if (isThreeDs(entry))
        return writeThreeDsSaveCatalog(
            namespaceDirectory(entry, ROMX_MUTABLE_NAMESPACE_SAVE),
            entry.path, key, nullptr, error);
    return writeBundle(entry, ROMX_MUTABLE_NAMESPACE_SAVE, key.c_str(), error);
}

SyncResult GameEntryAdapter::restoreCheat(GameEntry& entry, std::string* error)
{
    if (!isRomxPath(entry.path))
        return SyncResult::Skipped;
    return importBundleNamespace(entry, ROMX_MUTABLE_NAMESPACE_CHEAT, true, error);
}

SyncResult GameEntryAdapter::exportCheat(const GameEntry& entry, std::string* error)
{
    if (!isRomxPath(entry.path))
        return SyncResult::Skipped;
    return writeBundle(entry, ROMX_MUTABLE_NAMESPACE_CHEAT,
                       mutableBundleKey(entry), error);
}

SyncResult GameEntryAdapter::restoreStats(GameEntry& entry, std::string* error)
{
    if (!isRomxPath(entry.path))
        return SyncResult::Skipped;
    return readStats(entry.path, entry, error);
}

SyncResult GameEntryAdapter::exportStats(const GameEntry& entry, std::string* error)
{
    if (!isRomxPath(entry.path))
        return SyncResult::Skipped;
    romx_mutable_stats_t stats = ROMX_MUTABLE_STATS_INIT;
    romx_reader_t* reader = nullptr;
    romx_error_t readError{};
    if (romx_reader_open_path(entry.path.c_str(), nullptr, &reader, &readError) == ROMX_OK)
    {
        // Preserve fields owned by other clients (completion, achievements,
        // and timestamps) while replacing only the frontend counters.
        if (romx_mutable_stats_read(reader, "default", &stats, &readError) != ROMX_OK)
            (void)romx_mutable_stats_read(reader, "libretro", &stats, &readError);
        romx_reader_close(reader);
    }
    const bool hadLastPlayed =
        (stats.flags & ROMX_MUTABLE_STATS_HAS_LAST_PLAYED) != 0;
    stats.flags |= ROMX_MUTABLE_STATS_HAS_PLAY_TIME |
                   ROMX_MUTABLE_STATS_HAS_LAUNCH_COUNT |
                   ROMX_MUTABLE_STATS_HAS_FAVORITE |
                   ROMX_MUTABLE_STATS_HAS_LAST_PLAYED;
    stats.play_time_seconds = entry.playTime < 0 ? 0 : static_cast<uint64_t>(entry.playTime);
    stats.launch_count = entry.playCount < 0 ? 0 : static_cast<uint64_t>(entry.playCount);
    stats.favorite = entry.favourite ? 1U : 0U;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const uint64_t localLastPlayed = parseTimestamp(entry.lastPlayed);
    if ((stats.flags & ROMX_MUTABLE_STATS_HAS_FIRST_PLAYED) == 0)
    {
        stats.flags |= ROMX_MUTABLE_STATS_HAS_FIRST_PLAYED;
        stats.first_played_unix_seconds = localLastPlayed == 0 ? now : localLastPlayed;
    }
    if (localLastPlayed != 0 || !hadLastPlayed)
        stats.last_played_unix_seconds = localLastPlayed == 0 ? now : localLastPlayed;
    romx_mutable_write_options_t options = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
    options.data_capacity = 0;
    romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
    romx_error_t err{};
    const romx_result_t result = romx_mutable_stats_write_path(
        entry.path.c_str(), "default", &stats, &options, &object, &err);
    if (result != ROMX_OK)
    {
        assignError(error, errorText("romx_mutable_stats_write_path", err, result));
        return SyncResult::Failed;
    }
    return SyncResult::Success;
}

bool GameEntryAdapter::writeStats(const GameEntry& entry, std::string* error)
{
    return exportStats(entry, error) == SyncResult::Success;
}

bool GameEntryAdapter::writeMutable(const GameEntry& entry, std::string* error)
{
    if (!isRomxPath(entry.path))
        return false;
    std::string saveError;
    std::string cheatError;
    const SyncResult saveResult = exportSave(entry, &saveError);
    const SyncResult cheatResult = exportCheat(entry, &cheatError);
    if (saveResult != SyncResult::Success && cheatResult != SyncResult::Success && error)
        *error = !cheatError.empty() ? cheatError : saveError;
    return saveResult == SyncResult::Success || cheatResult == SyncResult::Success;
}

std::vector<GameEntryAdapter::SaveSlot> GameEntryAdapter::listSaveSlots(
    const GameEntry& entry, std::string* error)
{
    return enumerateSaveSlots(entry, error);
}

std::vector<GameEntryAdapter::LocalSaveCandidate>
GameEntryAdapter::listLocalSaveCandidates(const GameEntry& entry,
                                           const std::string& sourcePath,
                                           std::string* error)
{
    return listThreeDsSaveCandidates(entry, fs::path(sourcePath), error);
}

SyncResult GameEntryAdapter::writeLocalSavesToRomx(
    const GameEntry& entry, const std::string& sourcePath,
    std::string* outputPath, uint32_t* writtenCount, std::string* error)
{
    if (outputPath)
        outputPath->clear();
    if (writtenCount)
        *writtenCount = 0;
    if (!isThreeDs(entry)) {
        assignError(error, "只有3DS游戏支持从目录写入 ROMX");
        return SyncResult::Failed;
    }
    if (!isRomxPath(entry.path)) {
        assignError(error, "只能向现有3DS ROMX容器写入存档，普通ROM不会自动生成 ROMX");
        return SyncResult::Failed;
    }

    const fs::path source(sourcePath);
    std::string scanError;
    const auto candidates = listThreeDsSaveCandidates(entry, source, &scanError);
    if (candidates.empty()) {
        if (!scanError.empty())
            assignError(error, scanError);
        return SyncResult::Skipped;
    }

    const fs::path destination(entry.path);
    if (destination.empty()) {
        assignError(error, "无法确定3DS ROMX容器路径");
        return SyncResult::Failed;
    }

    std::error_code ec;
    if (!fs::is_regular_file(destination, ec) || ec) {
        assignError(error, "现有 ROMX 容器不存在或不是普通文件");
        return SyncResult::Failed;
    }
    Info info;
    std::string infoError;
    if (!readInfo(destination.string(), info, &infoError) ||
        info.platform != kThreeDsPlatform) {
        assignError(error, infoError.empty()
            ? "现有 ROMX 不是3DS容器" : infoError);
        return SyncResult::Failed;
    }

    const SyncResult result = writeThreeDsSaveCatalog(
        source, destination, "libretro", writtenCount, error);
    if (outputPath && (result == SyncResult::Success ||
                       (writtenCount && *writtenCount != 0)))
        *outputPath = destination.string();
    return result;
}

uint32_t GameEntryAdapter::saveSlotKeyCapacity()
{
    return ROMX_MUTABLE_KEY_CAPACITY;
}

bool GameEntryAdapter::validateSaveSlotKey(const std::string& key,
                                           std::string* error)
{
    return validateSaveSlotKeyImpl(key, error);
}

} // namespace beiklive::romx
