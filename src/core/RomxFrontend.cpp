#include "core/RomxFrontend.hpp"

#include "core/constexpr.h"
#include "core/enums.h"

#include <nlohmann/json.hpp>
#include <romx/romx.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace beiklive::romx
{
namespace
{
constexpr std::uint64_t MaxMetadataSize = 16U * 1024U * 1024U;

struct CacheEntry
{
    std::uintmax_t fileSize = 0;
    fs::file_time_type modified{};
    bool verified = false;
    Info info;
};

std::mutex cacheMutex;
std::unordered_map<std::string, CacheEntry> infoCache;

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

void setError(std::string* error, const std::string& message)
{
    if (error) *error = message;
}

std::string romxError(const romx_error_t& error, romx_result_t result)
{
    if (error.message[0] != '\0') return error.message;
    const char* text = romx_result_string(result);
    return text ? text : "ROMX operation failed";
}

std::string hex(const std::uint8_t* bytes, std::size_t size)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i)
        output << std::setw(2) << static_cast<unsigned>(bytes[i]);
    return output.str();
}

std::uint32_t parseHex32(const std::string& value)
{
    if (value.size() != 8) return 0;
    std::uint32_t result = 0;
    for (char c : value)
    {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        else return 0;
        result = (result << 4U) | digit;
    }
    return result;
}

std::string stringValue(const json& object, const char* key)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_string())
        return {};
    return object[key].get<std::string>();
}

std::string stringListValue(const json& object, const char* key)
{
    if (!object.is_object() || !object.contains(key)) return {};
    const auto& value = object[key];
    if (value.is_string()) return value.get<std::string>();
    if (!value.is_array()) return {};
    std::string result;
    for (const auto& item : value)
    {
        if (!item.is_string()) continue;
        if (!result.empty()) result += ", ";
        result += item.get<std::string>();
    }
    return result;
}

int platformFromString(std::string value)
{
    value = lower(std::move(value));
    if (value == "gba") return static_cast<int>(enums::EmuPlatform::EmuGBA);
    if (value == "gbc") return static_cast<int>(enums::EmuPlatform::EmuGBC);
    if (value == "gb") return static_cast<int>(enums::EmuPlatform::EmuGB);
    if (value == "nes" || value == "fds") return static_cast<int>(enums::EmuPlatform::EmuNES);
    if (value == "snes") return static_cast<int>(enums::EmuPlatform::EmuSNES);
    if (value == "nds") return static_cast<int>(enums::EmuPlatform::EmuNDS);
    if (value == "3ds") return static_cast<int>(enums::EmuPlatform::Emu3DS);
    if (value == "genesis" || value == "genesis32x")
        return static_cast<int>(enums::EmuPlatform::EmuGenesis);
    if (value == "dreamcast") return static_cast<int>(enums::EmuPlatform::EmuDreamcast);
    if (value == "psp") return static_cast<int>(enums::EmuPlatform::EmuPSP);
    return 0;
}

int platformFromExtension(std::string value)
{
    value = lower(std::move(value));
    if (!value.empty() && value.front() == '.') value.erase(value.begin());
    if (value == "gba") return static_cast<int>(enums::EmuPlatform::EmuGBA);
    if (value == "gbc") return static_cast<int>(enums::EmuPlatform::EmuGBC);
    if (value == "gb") return static_cast<int>(enums::EmuPlatform::EmuGB);
    if (value == "nes" || value == "unf" || value == "unif" || value == "fds")
        return static_cast<int>(enums::EmuPlatform::EmuNES);
    if (value == "sfc" || value == "smc") return static_cast<int>(enums::EmuPlatform::EmuSNES);
    if (value == "nds") return static_cast<int>(enums::EmuPlatform::EmuNDS);
    if (value == "3ds" || value == "cci" || value == "cxi" || value == "app" || value == "cia")
        return static_cast<int>(enums::EmuPlatform::Emu3DS);
    if (value == "md" || value == "gen" || value == "smd" || value == "32x" || value == "bin")
        return static_cast<int>(enums::EmuPlatform::EmuGenesis);
    if (value == "iso" || value == "cso" || value == "pbp" || value == "elf" || value == "prx")
        return static_cast<int>(enums::EmuPlatform::EmuPSP);
    if (value == "cdi") return static_cast<int>(enums::EmuPlatform::EmuDreamcast);
    return 0;
}

std::string extensionForPlatform(int platform, const std::string& format)
{
    const std::string normalized = lower(format);
    if (platformFromExtension(normalized) == platform && !normalized.empty())
        return "." + normalized;
    return {};
}

std::string aliasExtension(const std::string& path)
{
    std::string extension = lower(fs::path(path).extension().string());
    if (extension.size() > 2 && extension.back() == 'x') extension.pop_back();
    return extension;
}

std::string trimTitle(const std::uint8_t* data, std::size_t size)
{
    std::string title(reinterpret_cast<const char*>(data), size);
    const std::size_t nul = title.find('\0');
    if (nul != std::string::npos) title.resize(nul);
    while (!title.empty() && (title.back() == ' ' ||
           static_cast<unsigned char>(title.back()) == 0xffU)) title.pop_back();
    return title;
}

bool readPayload(const romx_reader_t* reader, const romx_info_t& info,
                 std::uint64_t offset, void* data, std::size_t size)
{
    if (!reader || offset > info.rom.size || size > info.rom.size - offset)
        return false;
    std::uint64_t read = 0;
    romx_error_t error{};
    return romx_reader_read_region(reader, ROMX_REGION_ROM, offset, data,
        static_cast<std::uint64_t>(size), &read, &error) == ROMX_OK &&
        read == size;
}

int detectHeader(const romx_reader_t* reader, const romx_info_t& info,
                 const std::string& payloadFormat, std::string* title,
                 std::string* extension)
{
    std::array<std::uint8_t, 0x200> header{};
    const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(header.size(), info.rom.size));
    if (wanted == 0 || !readPayload(reader, info, 0, header.data(), wanted)) return 0;
    const auto set = [&](int platform, const char* ext, const std::string& name = std::string{}) {
        if (title && title->empty()) *title = name;
        if (extension) *extension = ext;
        return platform;
    };
    if (wanted >= 4 && std::memcmp(header.data(), "NES\x1a", 4) == 0)
        return set(static_cast<int>(enums::EmuPlatform::EmuNES), ".nes");
    if (wanted >= 4 && std::memcmp(header.data(), "FDS\x1a", 4) == 0)
        return set(static_cast<int>(enums::EmuPlatform::EmuNES), ".fds");
    static constexpr std::array<std::uint8_t, 8> NintendoLogo{0x24,0xff,0xae,0x51,0x69,0x9a,0xa2,0x21};
    if (wanted >= 0x160 && std::equal(NintendoLogo.begin(), NintendoLogo.end(), header.begin() + 0xc0))
        return set(static_cast<int>(enums::EmuPlatform::EmuNDS), ".nds", trimTitle(header.data(), 12));
    if (wanted >= 0x104 && std::memcmp(header.data() + 0x100, "NCSD", 4) == 0)
        return set(static_cast<int>(enums::EmuPlatform::Emu3DS), ".cci");
    if (wanted >= 0x154 && std::memcmp(header.data() + 0x100, "SEGA", 4) == 0)
    {
        std::string name = trimTitle(header.data() + 0x150, 48);
        if (name.empty()) name = trimTitle(header.data() + 0x120, 48);
        return set(static_cast<int>(enums::EmuPlatform::EmuGenesis), ".md", name);
    }
    static constexpr std::array<std::uint8_t, 8> GbaLogo{0x24,0xff,0xae,0x51,0x69,0x9a,0xa2,0x21};
    if (wanted > 0xb2 && header[0xb2] == 0x96 &&
        std::equal(GbaLogo.begin(), GbaLogo.end(), header.begin() + 0x04))
        return set(static_cast<int>(enums::EmuPlatform::EmuGBA), ".gba",
            wanted >= 0xac ? trimTitle(header.data() + 0xa0, 12) : std::string{});
    static constexpr std::array<std::uint8_t, 8> GbLogo{0xce,0xed,0x66,0x66,0xcc,0x0d,0x00,0x0b};
    if (wanted >= 0x144 && std::equal(GbLogo.begin(), GbLogo.end(), header.begin() + 0x104))
    {
        const bool color = header[0x143] == 0xc0 ||
            (header[0x143] == 0x80 && lower(payloadFormat) != "gb");
        return set(color ? static_cast<int>(enums::EmuPlatform::EmuGBC)
                         : static_cast<int>(enums::EmuPlatform::EmuGB),
            color ? ".gbc" : ".gb",
            trimTitle(header.data() + 0x134, color ? 15 : 16));
    }

    const std::array<std::uint64_t, 6> candidates{0x7fc0,0xffc0,0x40ffc0,0x81c0,0x101c0,0x4101c0};
    int bestScore = 0;
    std::string bestTitle;
    for (const auto offset : candidates)
    {
        std::array<std::uint8_t, 64> snes{};
        if (offset + snes.size() > info.rom.size || !readPayload(reader, info, offset, snes.data(), snes.size())) continue;
        int score = 0;
        const std::uint8_t mode = snes[0x15] & 0x3fU;
        if (mode == 0x20 || mode == 0x21 || mode == 0x22 || mode == 0x23 || mode == 0x25 || mode == 0x30 || mode == 0x31 || mode == 0x32 || mode == 0x35) score += 2;
        const std::uint16_t complement = static_cast<std::uint16_t>(snes[0x1c] | (snes[0x1d] << 8));
        const std::uint16_t checksum = static_cast<std::uint16_t>(snes[0x1e] | (snes[0x1f] << 8));
        if (checksum != 0 && static_cast<std::uint16_t>(checksum + complement) == 0xffffU) score += 3;
        int printable = 0;
        for (std::size_t i = 0; i < 21; ++i) if (snes[i] == ' ' || (snes[i] >= 0x21 && snes[i] <= 0x7e)) ++printable;
        if (printable >= 18) score += 2;
        if (score > bestScore) { bestScore = score; bestTitle = trimTitle(snes.data(), 21); }
    }
    if (bestScore >= 4) return set(static_cast<int>(enums::EmuPlatform::EmuSNES), ".sfc", bestTitle);
    return 0;
}

bool readMetadataJson(const romx_reader_t* reader, const romx_info_t& info,
                      json* metadata, std::string* serialized)
{
    if (metadata) *metadata = json::object();
    if (serialized) serialized->clear();
    if (info.metadata.size == 0 || info.metadata.size > MaxMetadataSize || !reader) return false;
    std::string bytes(static_cast<std::size_t>(info.metadata.size), '\0');
    std::uint64_t read = 0;
    romx_error_t error{};
    if (romx_reader_read_region(reader, ROMX_REGION_METADATA, 0, bytes.data(), info.metadata.size, &read, &error) != ROMX_OK || read != info.metadata.size) return false;
    try
    {
        json parsed = json::parse(bytes);
        if (!parsed.is_object()) return false;
        if (metadata) *metadata = parsed;
        if (serialized) *serialized = parsed.dump();
        return true;
    }
    catch (const json::exception&) { return false; }
}
}

bool hasSupportedExtension(const std::string& path)
{
    static constexpr std::array<const char*, 27> extensions{
        ".gbx", ".gbcx", ".gbax", ".nesx", ".unfx", ".unifx", ".fdsx",
        ".sfcx", ".smcx", ".ndsx", ".3dsx", ".ccix", ".cxix", ".appx",
        ".ciax", ".mdx", ".genx", ".smdx", ".32xx", ".binx", ".isox",
        ".csox", ".pbpx", ".elfx", ".prxx", ".chdx", ".cdix"};
    const std::string extension = lower(fs::path(path).extension().string());
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

std::string logicalExtension(const std::string& path, const Info* info)
{
    if (info && !info->payloadFormat.empty()) return lower(info->payloadFormat);
    return aliasExtension(path).substr(1);
}

std::optional<Info> readInfo(const std::string& path, std::string* error, bool verifyPayload)
{
    if (error) error->clear();
    if (!hasSupportedExtension(path)) return std::nullopt;

    std::error_code statusError;
    const auto fileSize = fs::file_size(path, statusError);
    const auto modified = fs::last_write_time(path, statusError);
    if (!statusError)
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto cached = infoCache.find(path);
        if (cached != infoCache.end() && cached->second.fileSize == fileSize &&
            cached->second.modified == modified && (!verifyPayload || cached->second.verified))
            return cached->second.info;
    }

    romx_reader_t* reader = nullptr;
    romx_error_t romxErrorValue{};
    romx_result_t result = romx_reader_open_path(path.c_str(), nullptr, &reader, &romxErrorValue);
    if (result != ROMX_OK) { setError(error, romxError(romxErrorValue, result)); return std::nullopt; }
    romx_info_t raw = ROMX_INFO_INIT;
    result = romx_reader_get_info(reader, &raw, &romxErrorValue);
    if (result != ROMX_OK) { setError(error, romxError(romxErrorValue, result)); romx_reader_close(reader); return std::nullopt; }

    Info info;
    std::string computedCrc32;
    info.version = raw.version; info.fileSize = raw.file_size; info.bodySize = raw.body_size;
    info.flags = raw.flags; info.romOffset = raw.rom.offset; info.romSize = raw.rom.size;
    info.metadataOffset = raw.metadata.offset; info.metadataSize = raw.metadata.size;
    info.coverOffset = raw.cover.offset; info.coverSize = raw.cover.size;
    if ((raw.flags & ROMX_FLAG_HAS_BODY_SHA256) != 0U) info.bodySha256 = hex(raw.body_sha256, 32);

    if (verifyPayload)
    {
        romx_validation_report_t report = ROMX_VALIDATION_REPORT_INIT;
        result = romx_reader_validate(reader, ROMX_VALIDATE_BODY_SHA256 | ROMX_VALIDATE_PAYLOAD_HASHES, &report, &romxErrorValue);
        if (result != ROMX_OK) { setError(error, romxError(romxErrorValue, result)); romx_reader_close(reader); return std::nullopt; }
        info.payloadSha256 = hex(report.computed_payload_sha256, 32);
        if (report.computed_payload_crc32 != 0U) {
            std::ostringstream crc; crc << std::hex << std::setfill('0') << std::setw(8) << report.computed_payload_crc32;
            computedCrc32 = crc.str();
        }
    }

    json metadata;
    readMetadataJson(reader, raw, &metadata, &info.metadataJson);
    info.title = stringValue(metadata, "name");
    info.developer = stringValue(metadata, "developer"); info.publisher = stringValue(metadata, "publisher");
    info.origin = stringValue(metadata, "origin"); info.franchise = stringValue(metadata, "franchise");
    info.releaseDate = stringValue(metadata, "release_date"); info.region = stringListValue(metadata, "region");
    info.crc32 = lower(stringValue(metadata, "crc32")); info.lookupCrc32 = parseHex32(info.crc32);
    if (info.crc32.empty()) { info.crc32 = computedCrc32; info.lookupCrc32 = parseHex32(info.crc32); }
    info.originCrc32 = lower(stringValue(metadata, "origin_crc32")); info.dumpStatus = stringValue(metadata, "dump_status");
    if (metadata.contains("genre") && metadata["genre"].is_array())
        for (const auto& entry : metadata["genre"]) if (entry.is_string()) info.genre.push_back(entry.get<std::string>());
    info.payloadFormat = lower(stringValue(metadata, "payload_format"));

    std::string headerTitle, headerExtension;
    const int detected = detectHeader(reader, raw, info.payloadFormat, &headerTitle, &headerExtension);
    const int declared = platformFromString(stringValue(metadata, "platform"));
    const std::string alias = aliasExtension(path);
    const int aliased = platformFromExtension(alias);
    info.platform = detected != 0 ? detected : declared != 0 ? declared : aliased;
    if (!headerExtension.empty() && detected != 0) info.romExtension = headerExtension;
    if (info.payloadFormat.empty()) info.payloadFormat = alias.substr(1);
    if (info.romExtension.empty()) info.romExtension = extensionForPlatform(info.platform, info.payloadFormat);
    if (info.romExtension.empty()) info.romExtension = alias;
    if (info.title.empty()) info.title = headerTitle;

    romx_reader_close(reader);
    if (!statusError)
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (infoCache.size() >= 1024U && infoCache.find(path) == infoCache.end()) infoCache.clear();
        infoCache[path] = CacheEntry{fileSize, modified, verifyPayload, info};
    }
    return info;
}

std::string extractCover(const std::string& packedPath, const Info& info,
                         const std::string& destinationDir, std::string* error)
{
    if (error) error->clear();
    if (info.coverSize == 0 || destinationDir.empty()) return {};
    std::error_code ec; fs::create_directories(destinationDir, ec);
    if (ec) { setError(error, ec.message()); return {}; }
    std::string key = !info.crc32.empty() ? info.crc32 : (!info.payloadSha256.empty() ? info.payloadSha256.substr(0, 16) : std::to_string(std::hash<std::string>{}(packedPath)));
    const fs::path output = fs::path(destinationDir) / ("packed_cover_" + key + ".png");
    if (fs::exists(output, ec) && fs::file_size(output, ec) == info.coverSize) return output.string();
    romx_reader_t* reader = nullptr; romx_error_t errorValue{};
    if (romx_reader_open_path(packedPath.c_str(), nullptr, &reader, &errorValue) != ROMX_OK) { setError(error, romxError(errorValue, errorValue.code)); return {}; }
    romx_extract_options_t options = ROMX_EXTRACT_OPTIONS_INIT; options.flags = ROMX_EXTRACT_REPLACE_EXISTING;
    const romx_result_t result = romx_extract_cover_path(reader, output.string().c_str(), &options, &errorValue);
    romx_reader_close(reader);
    if (result != ROMX_OK) { setError(error, romxError(errorValue, result)); return {}; }
    return output.string();
}

std::string prepareRomForLaunch(const std::string& path, std::string* error)
{
    if (!hasSupportedExtension(path)) return path;
    auto info = readInfo(path, error, false);
    if (!info) return {};
    std::error_code ec;
    const fs::path directory = fs::path(beiklive::path::cachePath()) / "packed_roms";
    fs::create_directories(directory, ec);
    if (ec) { setError(error, ec.message()); return {}; }
    std::string key = !info->payloadSha256.empty() ? info->payloadSha256 : info->crc32;
    if (key.empty()) key = std::to_string(std::hash<std::string>{}(path));
    const fs::path output = directory / (key + (info->romExtension.empty() ? ".rom" : info->romExtension));
    if (fs::exists(output, ec) && fs::file_size(output, ec) == info->romSize) return output.string();
    romx_reader_t* reader = nullptr; romx_error_t errorValue{};
    if (romx_reader_open_path(path.c_str(), nullptr, &reader, &errorValue) != ROMX_OK) { setError(error, romxError(errorValue, errorValue.code)); return {}; }
    romx_extract_options_t options = ROMX_EXTRACT_OPTIONS_INIT; options.flags = ROMX_EXTRACT_REPLACE_EXISTING;
    const romx_result_t result = romx_extract_payload_path(reader, output.string().c_str(), &options, &errorValue);
    romx_reader_close(reader);
    if (result != ROMX_OK) { setError(error, romxError(errorValue, result)); return {}; }
    return output.string();
}

bool loadPayloadToMemory(const std::string& path, std::vector<std::uint8_t>& output,
                         std::string* error)
{
    output.clear();
    if (error) error->clear();

    if (!hasSupportedExtension(path))
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            setError(error, "failed to open ROM file");
            return false;
        }
        const std::streampos end = file.tellg();
        if (end <= 0 || static_cast<std::uintmax_t>(end) >
                             static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
        {
            setError(error, "ROM file is empty or too large");
            return false;
        }
        output.resize(static_cast<std::size_t>(end));
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size())))
        {
            output.clear();
            setError(error, "failed to read ROM file");
            return false;
        }
        return true;
    }

    romx_reader_t* reader = nullptr;
    romx_error_t errorValue{};
    romx_result_t result = romx_reader_open_path(path.c_str(), nullptr, &reader, &errorValue);
    if (result != ROMX_OK)
    {
        setError(error, romxError(errorValue, result));
        return false;
    }

    romx_info_t info = ROMX_INFO_INIT;
    result = romx_reader_get_info(reader, &info, &errorValue);
    if (result != ROMX_OK || info.rom.size == 0 || info.rom.size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        setError(error, result != ROMX_OK ? romxError(errorValue, result) :
            (info.rom.size == 0 ? "ROMX payload is empty" : "ROMX payload is too large"));
        romx_reader_close(reader);
        return false;
    }

    output.resize(static_cast<std::size_t>(info.rom.size));
    std::uint64_t offset = 0;
    constexpr std::uint64_t ChunkSize = 1024U * 1024U;
    while (offset < info.rom.size)
    {
        const std::uint64_t chunk = std::min<std::uint64_t>(ChunkSize, info.rom.size - offset);
        std::uint64_t bytesRead = 0;
        result = romx_reader_read_region(reader, ROMX_REGION_ROM, offset,
            output.data() + static_cast<std::size_t>(offset), chunk, &bytesRead, &errorValue);
        if (result != ROMX_OK || bytesRead != chunk)
        {
            output.clear();
            setError(error, result != ROMX_OK ? romxError(errorValue, result) :
                "ROMX payload read was truncated");
            romx_reader_close(reader);
            return false;
        }
        offset += bytesRead;
    }

    romx_reader_close(reader);
    return true;
}

} // 命名空间 beiklive::romx
