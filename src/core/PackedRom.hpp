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
        std::string releaseDate;
        std::vector<std::string> genre;
        std::string region;
        std::string romSha256;
        std::string payloadFormat;
        std::string romExtension;
        std::string metadataJson;
        bool legacyContainer = false;
        std::uint64_t romOffset = 0;
        std::uint64_t romSize = 0;
        std::uint64_t metadataOffset = 0;
        std::uint64_t metadataSize = 0;
        std::uint64_t coverOffset = 0;
        std::uint64_t coverSize = 0;
    };

    /// True for a supported "normal ROM extension + x" packed-ROM alias.
    bool hasSupportedExtension(const std::string& path);

    /// Parses and validates a ROMX 1.0 container. Legacy GBAX files remain readable.
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
