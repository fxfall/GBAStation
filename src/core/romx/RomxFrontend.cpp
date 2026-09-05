#include "RomxFrontend.hpp"

#include "core/constexpr.h"

#include <romx/romx.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace beiklive::romx
{
namespace
{
std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string extension(const std::string& path)
{
    const std::string ext = fs::path(path).extension().string();
    if (ext.empty())
        return {};
    return lower(ext.substr(1));
}

bool isAlias(const std::string& ext)
{
    return ext == "romx";
}

void setError(std::string* output, const std::string& message)
{
    if (output)
        *output = message;
}

std::string errorText(const char* operation, const romx_error_t& error, romx_result_t result)
{
    std::ostringstream stream;
    stream << operation << " failed (" << result << ")";
    if (error.message[0] != '\0')
        stream << ": " << error.message;
    return stream.str();
}

template <typename CopyFn>
bool copyBytes(CopyFn&& copy, std::string& out, std::string* error)
{
    uint64_t required = 0;
    romx_error_t err{};
    romx_result_t result = copy(nullptr, 0, &required, &err);
    if (result != ROMX_E_BUFFER_TOO_SMALL && result != ROMX_OK)
    {
        setError(error, errorText("ROMX JSON copy", err, result));
        return false;
    }
    if (required == 0)
    {
        out.clear();
        return true;
    }
    out.resize(static_cast<size_t>(required));
    result = copy(out.data(), required, &required, &err);
    if (result != ROMX_OK)
    {
        setError(error, errorText("ROMX JSON copy", err, result));
        out.clear();
        return false;
    }
    return true;
}

std::string jsonString(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string cacheKey(const Info& info, const char* purpose)
{
    // FNV-1a is stable across platforms.  The identity is deliberately built
    // from immutable ROMX data only: mutable SAVE/CHEAT/STATS commits must not
    // invalidate a launch or cover cache.
    uint64_t hash = UINT64_C(1469598103934665603);
    const std::string value = std::string(purpose) + "\n" + info.cacheIdentity;
    for (unsigned char c : value)
    {
        hash ^= c;
        hash *= UINT64_C(1099511628211);
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

std::string ephemeralCacheKey(const char* purpose)
{
    // A container without a trustworthy immutable identity must not reuse a
    // permanent cache entry.  Keep the result in the normal cache directory
    // for the current launch, but give every materialization a fresh name.
    static std::atomic<uint64_t> sequence{0};
    const uint64_t serial = ++sequence;
    const uint64_t clock = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::string(purpose) + "-uncacheable-" + std::to_string(clock) +
           "-" + std::to_string(serial);
}

std::string cachePathKey(const Info& info, const char* purpose)
{
    return info.cacheable && !info.cacheIdentity.empty()
        ? std::string(purpose) + "-" + cacheKey(info, purpose)
        : ephemeralCacheKey(purpose);
}

bool cacheFileSizeMatches(const fs::path& path, uint64_t expectedSize)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
        return false;
    return fs::file_size(path, ec) == expectedSize && !ec;
}

uint32_t crc32Update(uint32_t crc, const uint8_t* data, std::size_t size)
{
    for (std::size_t index = 0; index < size; ++index)
    {
        crc ^= static_cast<uint32_t>(data[index]);
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            const uint32_t mask = static_cast<uint32_t>(-
                static_cast<int32_t>(crc & UINT32_C(1)));
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc;
}

bool cacheFileMatches(const fs::path& path, const romx_entry_info_t& entry)
{
    if (!cacheFileSizeMatches(path, entry.data_size))
        return false;
    if ((entry.flags & ROMX_RIDX_HAS_CRC32) == 0)
        return true;

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    std::vector<uint8_t> buffer(256 * 1024);
    uint32_t crc = UINT32_C(0xffffffff);
    while (input)
    {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read > 0)
            crc = crc32Update(crc, buffer.data(), static_cast<std::size_t>(read));
        if (input.bad())
            return false;
    }
    return (crc ^ UINT32_C(0xffffffff)) == entry.crc32;
}

fs::path temporaryCachePath(const fs::path& destination)
{
    static std::atomic<uint64_t> sequence{0};
    return destination.string() + ".tmp-" + std::to_string(++sequence);
}

bool publishEntryCache(const fs::path& temporary, const fs::path& destination,
                       const romx_entry_info_t& entry, std::string* error)
{
    std::error_code ec;
    fs::rename(temporary, destination, ec);
    if (!ec)
        return true;
    // POSIX rename replaces atomically; another frontend may have won the
    // race.  Accept that result only after validating the complete file.
    if (cacheFileMatches(destination, entry))
    {
        fs::remove(temporary, ec);
        return true;
    }
    const std::string message = ec.message();
    fs::remove(temporary, ec);
    setError(error, "failed to publish ROMX cache: " + message);
    return false;
}

bool publishSizedCache(const fs::path& temporary, const fs::path& destination,
                       uint64_t expectedSize)
{
    std::error_code ec;
    fs::rename(temporary, destination, ec);
    if (!ec)
        return true;
    if (cacheFileSizeMatches(destination, expectedSize))
    {
        fs::remove(temporary, ec);
        return true;
    }
    fs::remove(temporary, ec);
    return false;
}

bool safeVirtualPath(const char* path, std::size_t size, fs::path& result)
{
    if (!path || size == 0)
        return false;
    const std::string raw(path, size);
    if (raw.find('\\') != std::string::npos || raw.find('\0') != std::string::npos)
        return false;
    const fs::path rawPath(raw);
    for (const auto& component : rawPath)
        if (component == "." || component == "..")
            return false;
    const fs::path normalized = rawPath.lexically_normal();
    if (normalized.empty() || normalized.is_absolute())
        return false;
    result = normalized;
    return true;
}

bool extractEntryToPath(const romx_reader_t* reader, uint32_t index,
                        const romx_entry_info_t& entry, const fs::path& destination,
                        std::string* error)
{
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec)
    {
        setError(error, "cannot create ROMX entry cache: " + ec.message());
        return false;
    }
    if (cacheFileMatches(destination, entry))
        return true;

    const fs::path temporary = temporaryCachePath(destination);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        setError(error, "cannot create ROMX entry cache file");
        return false;
    }
    std::vector<uint8_t> buffer(256 * 1024);
    uint64_t offset = 0;
    while (offset < entry.data_size)
    {
        const uint64_t requested = std::min<uint64_t>(buffer.size(), entry.data_size - offset);
        uint64_t read = 0;
        romx_error_t err{};
        const romx_result_t result = romx_reader_read_entry(
            reader, index, offset, buffer.data(), requested, &read, &err);
        if (result != ROMX_OK || read == 0)
        {
            output.close();
            fs::remove(temporary, ec);
            setError(error, errorText("romx_reader_read_entry", err, result));
            return false;
        }
        if (read > requested)
        {
            output.close();
            fs::remove(temporary, ec);
            setError(error, "ROMX reader returned too many entry bytes");
            return false;
        }
        output.write(reinterpret_cast<const char*>(buffer.data()),
                     static_cast<std::streamsize>(read));
        if (!output)
        {
            output.close();
            fs::remove(temporary, ec);
            setError(error, "failed writing ROMX entry cache");
            return false;
        }
        offset += read;
    }
    output.close();
    return publishEntryCache(temporary, destination, entry, error);
}

bool buildCacheIdentity(const romx_reader_t* reader, const romx_info_t& romxInfo,
                        Info& out, std::string* error)
{
    out.cacheIdentity.clear();
    out.cacheable = false;
    if (romxInfo.immutable_hash_algorithm == ROMX_IMMUTABLE_HASH_SHA256)
    {
        static constexpr char hex[] = "0123456789abcdef";
        out.cacheIdentity = "sha256:";
        for (const uint8_t byte : romxInfo.immutable_sha256)
        {
            out.cacheIdentity += hex[byte >> 4];
            out.cacheIdentity += hex[byte & 0x0f];
        }
        out.cacheIdentity += "|platform=" + std::to_string(romxInfo.platform_id);
        out.cacheIdentity += "|launch=" + std::to_string(romxInfo.launch_format_id);
        out.cacheable = true;
        return true;
    }

    if (romxInfo.entry_count == 0)
        return true;
    out.cacheIdentity = "ridx|platform=" + std::to_string(romxInfo.platform_id) +
                        "|launch=" + std::to_string(romxInfo.launch_format_id);
    for (uint32_t index = 0; index < romxInfo.entry_count; ++index)
    {
        romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
        romx_error_t err{};
        const romx_result_t result = romx_reader_get_entry(reader, index, &entry, &err);
        if (result != ROMX_OK)
        {
            setError(error, errorText("ROMX cache identity", err, result));
            out.cacheIdentity.clear();
            return false;
        }
        // A CRC-less entry cannot safely participate in a persistent
        // multi-file cache: a changed non-entrypoint file would be invisible.
        if ((entry.flags & ROMX_RIDX_HAS_CRC32) == 0)
        {
            out.cacheIdentity.clear();
            return true;
        }
        out.cacheIdentity += "|entry=" + std::to_string(index) +
                             ",flags=" + std::to_string(entry.flags) +
                             ",format=" + std::to_string(entry.format_id) +
                             ",offset=" + std::to_string(entry.data_offset) +
                             ",size=" + std::to_string(entry.data_size) +
                             ",crc=" + std::to_string(entry.crc32) +
                             ",path=";
        out.cacheIdentity.append(entry.path, entry.path_size);
    }
    out.cacheable = true;
    return true;
}

uint32_t crcFromEntryOrMetadata(const romx_entry_info_t& entry,
                                const romx_metadata_t* metadata)
{
    // Metadata crc32 is the effective database identity.  A RIDX CRC32 is
    // an integrity value and may legitimately differ (especially for
    // multi-file or transformed payloads), so use it only as a fallback.
    uint32_t crc = 0;
    romx_error_t error{};
    if (metadata && romx_metadata_get_crc32(metadata, &crc, &error) == ROMX_OK)
        return crc;
    if ((entry.flags & ROMX_RIDX_HAS_CRC32) != 0)
        return entry.crc32;
    return 0;
}
} // namespace

bool isRomxPath(const std::string& path)
{
    return isAlias(extension(path));
}

int platformFromRomxId(uint16_t id)
{
    switch (id)
    {
    case ROMX_PLATFORM_GAME_BOY_ADVANCE: return static_cast<int>(enums::EmuPlatform::EmuGBA);
    case ROMX_PLATFORM_GAME_BOY_COLOR: return static_cast<int>(enums::EmuPlatform::EmuGBC);
    case ROMX_PLATFORM_GAME_BOY: return static_cast<int>(enums::EmuPlatform::EmuGB);
    case ROMX_PLATFORM_NES: return static_cast<int>(enums::EmuPlatform::EmuNES);
    case ROMX_PLATFORM_SNES: return static_cast<int>(enums::EmuPlatform::EmuSNES);
    case ROMX_PLATFORM_NINTENDO_DS: return static_cast<int>(enums::EmuPlatform::EmuNDS);
    case ROMX_PLATFORM_NINTENDO_3DS: return static_cast<int>(enums::EmuPlatform::Emu3DS);
    case ROMX_PLATFORM_MEGA_DRIVE:
    case ROMX_PLATFORM_MEGA_DRIVE_32X: return static_cast<int>(enums::EmuPlatform::EmuGenesis);
    case ROMX_PLATFORM_ARCADE: return static_cast<int>(enums::EmuPlatform::EmuArcade);
    case ROMX_PLATFORM_DREAMCAST: return static_cast<int>(enums::EmuPlatform::EmuDreamcast);
    case ROMX_PLATFORM_PSP: return static_cast<int>(enums::EmuPlatform::EmuPSP);
    case ROMX_PLATFORM_PLAYSTATION:
    case ROMX_PLATFORM_PLAYSTATION_2: return static_cast<int>(enums::EmuPlatform::EmuPS1);
    case ROMX_PLATFORM_SEGA_SATURN: return static_cast<int>(enums::EmuPlatform::EmuSaturn);
    case ROMX_PLATFORM_GAMECUBE:
    case ROMX_PLATFORM_WII: return static_cast<int>(enums::EmuPlatform::EmuDolphin);
    default: return static_cast<int>(enums::EmuPlatform::NONE);
    }
}

static bool readInfoFromReader(const romx_reader_t* reader,
    const std::string& path, Info& out, std::string* error)
{
    out = {};
    out.sourcePath = path;
    romx_error_t err{};
    romx_result_t result;
    romx_info_t info = ROMX_INFO_INIT;
    romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
    result = romx_reader_get_info(reader, &info, &err);
    if (result == ROMX_OK)
        result = romx_reader_get_entrypoint(reader, &entry, &err);
    if (result != ROMX_OK)
    {
        setError(error, errorText("ROMX entrypoint", err, result));
        return false;
    }
    out.platformId = info.platform_id;
    if (info.platform_id != 0)
        if (const char* name = romx_platform_name(info.platform_id))
            out.platformName = name;
    out.launchFormatId = info.launch_format_id;
    out.platform = platformFromRomxId(info.platform_id);
    out.entryCount = info.entry_count;
    out.multiFile = info.entry_count > 1 || info.launch_format_id != ROMX_LAUNCH_RAW_SINGLE_FILE;
    out.entrypointPath.assign(entry.path, entry.path + entry.path_size);
    out.entrypointFormatId = entry.format_id;
    if (const char* formatName = romx_file_format_name(entry.format_id))
        out.entrypointFormat = formatName;
    out.entrypointSize = entry.data_size;

    romx_metadata_t* metadata = nullptr;
    if (info.metadata.size != 0 && romx_metadata_open(reader, &metadata, &err) == ROMX_OK)
    {
        if (copyBytes(
            [metadata](void* buffer, uint64_t capacity, uint64_t* required, romx_error_t* e) {
                return romx_metadata_copy_json(metadata, buffer, capacity, required, e);
            }, out.metadataJson, error))
        {
            const auto json = nlohmann::json::parse(out.metadataJson, nullptr, false);
            if (json.is_object())
            {
                out.title = jsonString(json, "name");
                out.serial = jsonString(json, "serial");
                const auto parseCrc = [&json](const char* key) -> uint32_t {
                    const auto value = json.find(key);
                    if (value == json.end() || !value->is_string())
                        return 0;
                    try {
                        return static_cast<uint32_t>(
                            std::stoul(value->get<std::string>(), nullptr, 16));
                    }
                    catch (...) {
                        return 0;
                    }
                };
                out.crc32 = parseCrc("crc32");
                // origin_crc32 is the exact entrypoint identity and is a
                // useful fallback when a database lookup CRC is omitted.
                if (out.crc32 == 0)
                    out.crc32 = parseCrc("origin_crc32");
            }
        }
    }
    if (metadata)
    {
        const uint32_t crc = crcFromEntryOrMetadata(entry, metadata);
        if (out.crc32 == 0 && crc != 0)
            out.crc32 = crc;
        romx_metadata_close(metadata);
    }
    else if (out.crc32 == 0 && (entry.flags & ROMX_RIDX_HAS_CRC32) != 0)
        out.crc32 = entry.crc32;

    romx_cover_info_t cover = ROMX_COVER_INFO_INIT;
    if (romx_reader_get_cover_info(reader, &cover, &err) == ROMX_OK)
    {
        out.coverSize = cover.size;
        out.hasCover = cover.size != 0;
    }
    return buildCacheIdentity(reader, info, out, error);
}

bool readInfo(const std::string& path, Info& out, std::string* error)
{
    out = {};
    romx_reader_t* reader = nullptr;
    romx_error_t err{};
    const romx_result_t result = romx_reader_open_path(path.c_str(), nullptr, &reader, &err);
    if (result != ROMX_OK)
    {
        setError(error, errorText("romx_reader_open_path", err, result));
        return false;
    }
    const bool success = readInfoFromReader(reader, path, out, error);
    romx_reader_close(reader);
    return success;
}

bool extractCover(const std::string& path, std::string& outPath, const std::string& cacheDirectory)
{
    Info info;
    std::string error;
    if (!readInfo(path, info, &error) || !info.hasCover)
        return false;

    const fs::path directory = cacheDirectory.empty()
        ? fs::path(beiklive::path::cachePath()) / "romx" / "covers"
        : fs::path(cacheDirectory);
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec)
        return false;
    outPath = (directory / (cachePathKey(info, "cover") + ".png")).string();

    romx_reader_t* reader = nullptr;
    romx_error_t err{};
    if (romx_reader_open_path(path.c_str(), nullptr, &reader, &err) != ROMX_OK)
    {
        outPath.clear();
        return false;
    }
    romx_cover_info_t cover = ROMX_COVER_INFO_INIT;
    if (romx_reader_get_cover_info(reader, &cover, &err) != ROMX_OK || cover.size == 0)
    {
        romx_reader_close(reader);
        outPath.clear();
        return false;
    }
    if (info.cacheable && cacheFileSizeMatches(outPath, cover.size))
    {
        romx_reader_close(reader);
        return true;
    }

    const fs::path temporary = temporaryCachePath(outPath);
    romx_extract_options_t options = ROMX_EXTRACT_OPTIONS_INIT;
    options.flags = ROMX_EXTRACT_REPLACE_EXISTING;
    const romx_result_t result = romx_extract_cover_path(
        reader, temporary.string().c_str(), &options, &err);
    romx_reader_close(reader);
    if (result != ROMX_OK)
    {
        fs::remove(temporary, ec);
        outPath.clear();
        return false;
    }
    if (!publishSizedCache(temporary, outPath, cover.size))
    {
        fs::remove(temporary, ec);
        outPath.clear();
        return false;
    }
    return cacheFileSizeMatches(outPath, cover.size);
}

LaunchSession::LaunchSession() = default;

LaunchSession::~LaunchSession()
{
    close();
}

bool LaunchSession::open(const std::string& path, std::string* error)
{
    close();
    if (!isRomxPath(path))
    {
        setError(error, "not a canonical ROMX 0.2.0 path");
        return false;
    }
    romx_error_t err{};
    const romx_result_t result = romx_reader_open_path(path.c_str(), nullptr, &reader_, &err);
    if (result != ROMX_OK)
    {
        setError(error, errorText("romx_reader_open_path", err, result));
        reader_ = nullptr;
        return false;
    }
    sourcePath_ = path;
    if (!readInfoFromReader(reader_, path, info_, error))
    {
        close();
        return false;
    }
    return true;
}

void LaunchSession::close()
{
    if (mapping_)
    {
        romx_payload_mapping_close(mapping_);
        mapping_ = nullptr;
    }
    if (reader_)
    {
        romx_reader_close(reader_);
        reader_ = nullptr;
    }
    sourcePath_.clear();
    info_ = {};
}

bool LaunchSession::mapPayload(const void** data, uint64_t* size, std::string* error)
{
    if (!data || !size || !reader_)
    {
        setError(error, "ROMX mapping arguments are invalid");
        return false;
    }
    if (info_.multiFile)
    {
        setError(error, "multi-file ROMX entrypoints must use VFS");
        return false;
    }
    if (!mapping_)
    {
        romx_error_t err{};
        const romx_result_t result = romx_reader_map_payload(reader_, &mapping_, &err);
        if (result != ROMX_OK)
        {
            setError(error, errorText("romx_reader_map_payload", err, result));
            return false;
        }
    }
    *data = romx_payload_mapping_data(mapping_);
    *size = romx_payload_mapping_size(mapping_);
    return *data != nullptr || *size == 0;
}

bool LaunchSession::materializeEntrypoint(const std::string& cacheDirectory,
                                          std::string& outPath, std::string* error)
{
    if (!reader_)
    {
        setError(error, "ROMX session is not open");
        return false;
    }
    fs::path directory(cacheDirectory);
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec)
    {
        setError(error, "cannot create ROMX payload cache: " + ec.message());
        return false;
    }
    const std::string key = cachePathKey(info_, "payload");

    if (info_.multiFile)
    {
        const fs::path root = directory / key;
        uint32_t count = 0;
        romx_error_t err{};
        const romx_result_t countResult = romx_reader_get_entry_count(reader_, &count, &err);
        if (countResult != ROMX_OK || count == 0)
        {
            setError(error, errorText("romx_reader_get_entry_count", err, countResult));
            return false;
        }
        fs::path entrypoint;
        for (uint32_t index = 0; index < count; ++index)
        {
            romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
            if (romx_reader_get_entry(reader_, index, &entry, &err) != ROMX_OK)
            {
                setError(error, errorText("romx_reader_get_entry", err, ROMX_E_INVALID_ARGUMENT));
                return false;
            }
            fs::path relative;
            if (!safeVirtualPath(entry.path, entry.path_size, relative))
            {
                setError(error, "ROMX entry path is unsafe");
                return false;
            }
            const fs::path target = root / relative;
            if (!extractEntryToPath(reader_, index, entry, target, error))
                return false;
            if (relative.generic_string() == info_.entrypointPath)
                entrypoint = target;
        }
        if (entrypoint.empty())
        {
            setError(error, "ROMX entrypoint was not found in the index");
            return false;
        }
        outPath = entrypoint.string();
        return true;
    }

    outPath = (directory / (key + "." +
                           (info_.entrypointFormat.empty() ? "rom" : lower(info_.entrypointFormat)))).string();
    romx_entry_info_t entrypoint = ROMX_ENTRY_INFO_INIT;
    romx_error_t err{};
    if (romx_reader_get_entrypoint(reader_, &entrypoint, &err) != ROMX_OK)
    {
        setError(error, errorText("romx_reader_get_entrypoint", err,
                                  ROMX_E_ENTRY_NOT_FOUND));
        outPath.clear();
        return false;
    }
    if (info_.cacheable && cacheFileMatches(outPath, entrypoint))
        return true;

    const fs::path temporary = temporaryCachePath(outPath);
    romx_extract_options_t options = ROMX_EXTRACT_OPTIONS_INIT;
    options.flags = ROMX_EXTRACT_REPLACE_EXISTING;
    const romx_result_t result = romx_extract_payload_path(
        reader_, temporary.string().c_str(), &options, &err);
    if (result != ROMX_OK)
    {
        fs::remove(temporary, ec);
        outPath.clear();
        setError(error, errorText("romx_extract_payload_path", err, result));
        return false;
    }
    return publishEntryCache(temporary, outPath, entrypoint, error);
}

bool LaunchSession::openVfs(const std::string& virtualPath, romx_vfs_file** outFile,
                            std::string* error)
{
    if (!reader_ || !outFile)
    {
        setError(error, "ROMX VFS arguments are invalid");
        return false;
    }
    *outFile = nullptr;
    romx_error_t err{};
    const romx_result_t result = virtualPath.empty()
        ? romx_vfs_file_open_entrypoint(reader_, outFile, &err)
        : romx_vfs_file_open(reader_, virtualPath.c_str(), outFile, &err);
    if (result != ROMX_OK)
    {
        setError(error, errorText("romx_vfs_file_open", err, result));
        return false;
    }
    return true;
}

} // namespace beiklive::romx
