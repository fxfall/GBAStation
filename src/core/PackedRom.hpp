#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace beiklive::packed_rom
{
    struct Info
    {
        int platform = 0;
        std::string title;
        std::string developer;
        std::string publisher;
        std::string origin;
        std::string franchise;
        std::string releaseDate;
        std::vector<std::string> genre;
        std::string region;
        std::string crc32;
        std::uint32_t lookupCrc32 = 0;
        std::string originCrc32;
        std::string dumpStatus;
        std::string bodySha256;
        std::string payloadSha256;
        std::string payloadFormat;
        std::string romExtension;
        std::string metadataJson;
        std::uint64_t romOffset = 0;
        std::uint64_t romSize = 0;
        std::uint64_t metadataOffset = 0;
        std::uint64_t metadataSize = 0;
        std::uint64_t coverOffset = 0;
        std::uint64_t coverSize = 0;
    };

    /// True for a supported "normal ROM extension + x" packed-ROM alias.
    bool hasSupportedExtension(const std::string& path);

    /// Parses and validates a ROMX 1.0 container.
    std::optional<Info> readInfo(const std::string& path, std::string* error = nullptr,
                                 bool verifyPayload = true);

    /// Extracts the embedded cover into destinationDir and returns the resulting path.
    std::string extractCover(const std::string& packedPath, const Info& info,
                             const std::string& destinationDir,
                             std::string* error = nullptr);

    /// Returns an extracted standard ROM path suitable for the selected emulator core.
    std::string prepareRomForLaunch(const std::string& path,
                                    std::string* error = nullptr);
}
