#include "core/PackedRom.hpp"

#include "core/constexpr.h"
#include "core/enums.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace beiklive::packed_rom
{
    namespace
    {
        constexpr std::uint64_t FooterSize = 128;
        constexpr std::uint32_t SupportedVersion = 1;
        constexpr std::uint32_t HasMetadata = 1U << 0;
        constexpr std::uint32_t HasCover = 1U << 1;
        constexpr std::uint32_t HasBodySha256 = 1U << 2;
        constexpr std::uint32_t KnownFlags = HasMetadata | HasCover | HasBodySha256;
        constexpr std::uint64_t MaxMetadataSize = 1024 * 1024;
        constexpr std::uint64_t MaxCoverSize = 32 * 1024 * 1024;

        struct InfoCacheEntry
        {
            std::uint64_t fileSize = 0;
            fs::file_time_type modified{};
            bool fullyVerified = false;
            Info info;
        };

        std::mutex InfoCacheMutex;
        std::unordered_map<std::string, InfoCacheEntry> InfoCache;

        class Sha256
        {
        public:
            Sha256()
                : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                         0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U}
            {
            }

            void update(const unsigned char* data, std::size_t size)
            {
                totalSize_ += size;
                while (size != 0)
                {
                    const std::size_t count = std::min(size, block_.size() - blockSize_);
                    std::memcpy(block_.data() + blockSize_, data, count);
                    blockSize_ += count;
                    data += count;
                    size -= count;
                    if (blockSize_ == block_.size())
                    {
                        transform(block_.data());
                        blockSize_ = 0;
                    }
                }
            }

            std::array<unsigned char, 32> finish()
            {
                const std::uint64_t bitSize = totalSize_ * 8;
                block_[blockSize_++] = 0x80;
                if (blockSize_ > 56)
                {
                    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(blockSize_),
                              block_.end(), 0);
                    transform(block_.data());
                    blockSize_ = 0;
                }
                std::fill(block_.begin() + static_cast<std::ptrdiff_t>(blockSize_),
                          block_.begin() + 56, 0);
                for (int i = 0; i < 8; ++i)
                    block_[63 - i] = static_cast<unsigned char>(bitSize >> (i * 8));
                transform(block_.data());

                std::array<unsigned char, 32> digest{};
                for (std::size_t i = 0; i < state_.size(); ++i)
                {
                    digest[i * 4] = static_cast<unsigned char>(state_[i] >> 24);
                    digest[i * 4 + 1] = static_cast<unsigned char>(state_[i] >> 16);
                    digest[i * 4 + 2] = static_cast<unsigned char>(state_[i] >> 8);
                    digest[i * 4 + 3] = static_cast<unsigned char>(state_[i]);
                }
                return digest;
            }

        private:
            static std::uint32_t rotateRight(std::uint32_t value, unsigned count)
            {
                return (value >> count) | (value << (32 - count));
            }

            void transform(const unsigned char* data)
            {
                static constexpr std::array<std::uint32_t, 64> K{
                    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
                    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
                    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
                    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
                    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
                    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
                    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
                    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
                    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
                    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
                    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
                    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
                    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
                    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
                    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
                    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
                std::array<std::uint32_t, 64> words{};
                for (std::size_t i = 0; i < 16; ++i)
                {
                    words[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24) |
                        (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16) |
                        (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8) |
                        static_cast<std::uint32_t>(data[i * 4 + 3]);
                }
                for (std::size_t i = 16; i < words.size(); ++i)
                {
                    const std::uint32_t s0 = rotateRight(words[i - 15], 7) ^
                        rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
                    const std::uint32_t s1 = rotateRight(words[i - 2], 17) ^
                        rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
                    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
                }

                std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
                std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
                for (std::size_t i = 0; i < words.size(); ++i)
                {
                    const std::uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
                    const std::uint32_t choice = (e & f) ^ (~e & g);
                    const std::uint32_t temp1 = h + sum1 + choice + K[i] + words[i];
                    const std::uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
                    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                    const std::uint32_t temp2 = sum0 + majority;
                    h = g; g = f; f = e; e = d + temp1;
                    d = c; c = b; b = a; a = temp1 + temp2;
                }
                state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
                state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
            }

            std::array<std::uint32_t, 8> state_{};
            std::array<unsigned char, 64> block_{};
            std::size_t blockSize_ = 0;
            std::uint64_t totalSize_ = 0;
        };

        std::uint32_t readLe32(const unsigned char* p)
        {
            return static_cast<std::uint32_t>(p[0]) |
                (static_cast<std::uint32_t>(p[1]) << 8) |
                (static_cast<std::uint32_t>(p[2]) << 16) |
                (static_cast<std::uint32_t>(p[3]) << 24);
        }

        std::uint16_t readLe16(const unsigned char* p)
        {
            return static_cast<std::uint16_t>(p[0]) |
                static_cast<std::uint16_t>(p[1] << 8);
        }

        std::uint64_t readLe64(const unsigned char* p)
        {
            std::uint64_t value = 0;
            for (int i = 0; i < 8; ++i)
                value |= static_cast<std::uint64_t>(p[i]) << (i * 8);
            return value;
        }

        std::uint32_t readBe32(const unsigned char* p)
        {
            return (static_cast<std::uint32_t>(p[0]) << 24) |
                (static_cast<std::uint32_t>(p[1]) << 16) |
                (static_cast<std::uint32_t>(p[2]) << 8) |
                static_cast<std::uint32_t>(p[3]);
        }

        std::string lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::string removeTrailingJsonCommas(const std::string& input)
        {
            std::string output;
            output.reserve(input.size());
            bool inString = false;
            bool escaped = false;
            for (std::size_t i = 0; i < input.size(); ++i)
            {
                const char c = input[i];
                if (inString)
                {
                    output.push_back(c);
                    if (escaped)
                        escaped = false;
                    else if (c == '\\')
                        escaped = true;
                    else if (c == '"')
                        inString = false;
                    continue;
                }
                if (c == '"')
                {
                    inString = true;
                    output.push_back(c);
                    continue;
                }
                if (c == ',')
                {
                    std::size_t next = i + 1;
                    while (next < input.size() &&
                           std::isspace(static_cast<unsigned char>(input[next])))
                        ++next;
                    if (next < input.size() && (input[next] == '}' || input[next] == ']'))
                        continue;
                }
                output.push_back(c);
            }
            return output;
        }

        std::string metadataString(const nlohmann::json& metadata, const char* key)
        {
            const auto it = metadata.find(key);
            return it != metadata.end() && it->is_string()
                ? it->get<std::string>() : std::string{};
        }

        std::string metadataStringOrList(const nlohmann::json& metadata, const char* key)
        {
            const auto it = metadata.find(key);
            if (it == metadata.end())
                return {};
            if (it->is_string())
                return it->get<std::string>();
            if (!it->is_array())
                return {};
            std::string result;
            for (const auto& value : *it)
            {
                if (!value.is_string() || value.get<std::string>().empty())
                    continue;
                if (!result.empty())
                    result += " / ";
                result += value.get<std::string>();
            }
            return result;
        }

        std::string trimHeaderTitle(const unsigned char* data, std::size_t size)
        {
            std::string title(reinterpret_cast<const char*>(data), size);
            const auto nul = title.find('\0');
            if (nul != std::string::npos)
                title.resize(nul);
            while (!title.empty() &&
                   (title.back() == ' ' || static_cast<unsigned char>(title.back()) == 0xFF))
                title.pop_back();
            return title;
        }

        int metadataPlatform(const nlohmann::json& metadata)
        {
            std::string platform = lower(metadataString(metadata, "platform"));
            if (platform == "gba" || platform == "game boy advance")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuGBA);
            if (platform == "gbc" || platform == "game boy color")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuGBC);
            if (platform == "gb" || platform == "game boy")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuGB);
            if (platform == "nes" || platform == "fc" || platform == "famicom")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuNES);
            if (platform == "snes" || platform == "sfc" || platform == "super famicom")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuSNES);
            if (platform == "nds" || platform == "nintendo ds")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuNDS);
            if (platform == "3ds" || platform == "nintendo 3ds")
                return static_cast<int>(beiklive::enums::EmuPlatform::Emu3DS);
            if (platform == "md" || platform == "genesis" ||
                platform == "mega drive" || platform == "megadrive")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuGenesis);
            return 0;
        }

        std::string packedBaseExtension(const std::string& path)
        {
            std::string ext = lower(fs::path(path).extension().string());
            if (ext.size() > 2 && ext.front() == '.' && ext.back() == 'x')
                ext.pop_back();
            return ext;
        }

        int extensionPlatform(const std::string& extension)
        {
            if (extension == ".gba") return static_cast<int>(beiklive::enums::EmuPlatform::EmuGBA);
            if (extension == ".gbc") return static_cast<int>(beiklive::enums::EmuPlatform::EmuGBC);
            if (extension == ".gb") return static_cast<int>(beiklive::enums::EmuPlatform::EmuGB);
            if (extension == ".nes" || extension == ".fds")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuNES);
            if (extension == ".sfc" || extension == ".smc")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuSNES);
            if (extension == ".nds") return static_cast<int>(beiklive::enums::EmuPlatform::EmuNDS);
            if (extension == ".cia" || extension == ".cci" || extension == ".3ds")
                return static_cast<int>(beiklive::enums::EmuPlatform::Emu3DS);
            if (extension == ".md" || extension == ".gen" ||
                extension == ".smd")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuGenesis);
            return 0;
        }

        int payloadPlatform(const std::string& format)
        {
            if (format == "gb") return static_cast<int>(beiklive::enums::EmuPlatform::EmuGB);
            if (format == "gbc") return static_cast<int>(beiklive::enums::EmuPlatform::EmuGBC);
            if (format == "gba") return static_cast<int>(beiklive::enums::EmuPlatform::EmuGBA);
            if (format == "nes" || format == "fds")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuNES);
            if (format == "sfc" || format == "smc")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuSNES);
            if (format == "nds") return static_cast<int>(beiklive::enums::EmuPlatform::EmuNDS);
            if (format == "3ds" || format == "cci" || format == "cia")
                return static_cast<int>(beiklive::enums::EmuPlatform::Emu3DS);
            if (format == "md" || format == "gen" || format == "smd" || format == "bin")
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuGenesis);
            return 0;
        }

        std::string payloadExtension(const std::string& format)
        {
            return payloadPlatform(format) == 0 ? std::string{} : "." + format;
        }

        bool readAt(std::ifstream& input, std::uint64_t offset, void* output, std::size_t size)
        {
            if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
                return false;
            input.clear();
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            if (!input)
                return false;
            input.read(static_cast<char*>(output), static_cast<std::streamsize>(size));
            return input.gcount() == static_cast<std::streamsize>(size);
        }

        bool sha256Range(std::ifstream& input, std::uint64_t offset, std::uint64_t size,
                         std::array<unsigned char, 32>& digest)
        {
            if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
                return false;
            input.clear();
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            if (!input)
                return false;
            Sha256 sha;
            std::array<unsigned char, 64 * 1024> buffer{};
            std::uint64_t remaining = size;
            while (remaining != 0)
            {
                const std::size_t chunk = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, buffer.size()));
                input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(chunk));
                if (input.gcount() != static_cast<std::streamsize>(chunk))
                    return false;
                sha.update(buffer.data(), chunk);
                remaining -= chunk;
            }
            digest = sha.finish();
            return true;
        }

        bool verifyContainerHashes(std::ifstream& input, std::uint64_t bodySize,
                                   std::uint64_t romOffset, std::uint64_t romSize,
                                   const unsigned char* expectedRom,
                                   bool checkBody, const unsigned char* expectedBody)
        {
            input.clear();
            input.seekg(0, std::ios::beg);
            if (!input)
                return false;

            Sha256 romSha;
            Sha256 bodySha;
            std::array<unsigned char, 64 * 1024> buffer{};
            const std::uint64_t romEnd = romOffset + romSize;
            std::uint64_t position = 0;
            while (position < bodySize)
            {
                const std::size_t chunk = static_cast<std::size_t>(
                    std::min<std::uint64_t>(bodySize - position, buffer.size()));
                input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(chunk));
                if (input.gcount() != static_cast<std::streamsize>(chunk))
                    return false;
                if (checkBody)
                    bodySha.update(buffer.data(), chunk);

                const std::uint64_t intersectionStart = std::max(position, romOffset);
                const std::uint64_t intersectionEnd = std::min(position + chunk, romEnd);
                if (intersectionStart < intersectionEnd)
                {
                    romSha.update(buffer.data() + static_cast<std::size_t>(intersectionStart - position),
                                  static_cast<std::size_t>(intersectionEnd - intersectionStart));
                }
                position += chunk;
            }

            const auto romDigest = romSha.finish();
            if (!std::equal(romDigest.begin(), romDigest.end(), expectedRom))
                return false;
            if (checkBody)
            {
                const auto bodyDigest = bodySha.finish();
                if (!std::equal(bodyDigest.begin(), bodyDigest.end(), expectedBody))
                    return false;
            }
            return true;
        }

        bool validStandardMetadata(const nlohmann::json& metadata)
        {
            if (!metadata.is_object() || metadataString(metadata, "schema_version") != "1.0")
                return false;
            const std::string label = metadataString(metadata, "label");
            const std::string platform = metadataString(metadata, "platform");
            const std::string format = metadataString(metadata, "payload_format");
            static constexpr std::array<const char*, 8> Platforms{
                "gb", "gbc", "gba", "nes", "snes", "nds", "3ds", "genesis"};
            if (label.empty() || label.size() > 512 ||
                std::find(Platforms.begin(), Platforms.end(), platform) == Platforms.end() ||
                payloadPlatform(format) == 0)
                return false;
            if (metadataPlatform(metadata) != payloadPlatform(format))
                return false;
            const auto cover = metadata.find("cover");
            if (cover != metadata.end() &&
                (!cover->is_object() || metadataString(*cover, "mime_type") != "image/png"))
                return false;
            return true;
        }

        bool rangesOverlap(std::uint64_t firstOffset, std::uint64_t firstSize,
                           std::uint64_t secondOffset, std::uint64_t secondSize)
        {
            if (firstSize == 0 || secondSize == 0)
                return false;
            return std::max(firstOffset, secondOffset) <
                std::min(firstOffset + firstSize, secondOffset + secondSize);
        }

        std::string shaHex(const unsigned char* bytes);

        bool fileMatchesSha256(const fs::path& path, std::uint64_t size,
                               const std::string& expectedSha256)
        {
            std::error_code ec;
            if (!fs::exists(path, ec) || fs::file_size(path, ec) != size || ec)
                return false;
            std::ifstream input(path, std::ios::binary);
            std::array<unsigned char, 32> digest{};
            return input && sha256Range(input, 0, size, digest) &&
                shaHex(digest.data()) == lower(expectedSha256);
        }

        int detectRomPlatform(std::ifstream& input, Info& info, std::string* headerTitle)
        {
            std::array<unsigned char, 0x200> header{};
            const std::size_t wanted = static_cast<std::size_t>(
                std::min<std::uint64_t>(header.size(), info.romSize));
            if (wanted == 0 || !readAt(input, info.romOffset, header.data(), wanted))
                return 0;

            if (wanted >= 4 && header[0] == 'N' && header[1] == 'E' &&
                header[2] == 'S' && header[3] == 0x1A)
            {
                info.romExtension = ".nes";
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuNES);
            }
            if (wanted >= 4 && header[0] == 'F' && header[1] == 'D' &&
                header[2] == 'S' && header[3] == 0x1A)
            {
                info.romExtension = ".fds";
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuNES);
            }

            static constexpr std::array<unsigned char, 8> NdsLogoPrefix{
                0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21};
            if (wanted >= 0x160 &&
                std::equal(NdsLogoPrefix.begin(), NdsLogoPrefix.end(), header.begin() + 0xC0))
            {
                if (headerTitle && headerTitle->empty())
                    *headerTitle = trimHeaderTitle(header.data(), 12);
                info.romExtension = ".nds";
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuNDS);
            }

            if (wanted >= 0x104 &&
                header[0x100] == 'N' && header[0x101] == 'C' &&
                header[0x102] == 'S' && header[0x103] == 'D')
            {
                info.romExtension = ".cci";
                return static_cast<int>(beiklive::enums::EmuPlatform::Emu3DS);
            }

            if (wanted >= 0x154 &&
                header[0x100] == 'S' && header[0x101] == 'E' &&
                header[0x102] == 'G' && header[0x103] == 'A')
            {
                if (headerTitle && headerTitle->empty())
                {
                    *headerTitle = trimHeaderTitle(header.data() + 0x150, 48);
                    if (headerTitle->empty())
                        *headerTitle = trimHeaderTitle(header.data() + 0x120, 48);
                }
                info.romExtension = ".md";
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuGenesis);
            }

            static constexpr std::array<unsigned char, 8> GbaLogoPrefix{
                0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21};
            // GBA Nintendo logo begins at 0x04 and the fixed value is at 0xB2.
            if (wanted > 0xB2 && header[0xB2] == 0x96 &&
                std::equal(GbaLogoPrefix.begin(), GbaLogoPrefix.end(), header.begin() + 0x04))
            {
                if (headerTitle && headerTitle->empty() && wanted >= 0xAC)
                    *headerTitle = trimHeaderTitle(header.data() + 0xA0, 12);
                info.romExtension = ".gba";
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuGBA);
            }

            // GB/GBC Nintendo logo signature begins at 0x104.
            static constexpr std::array<unsigned char, 8> GbLogoPrefix{
                0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B};
            if (wanted >= 0x144 &&
                std::equal(GbLogoPrefix.begin(), GbLogoPrefix.end(), header.begin() + 0x104))
            {
                const unsigned char cgbFlag = header[0x143];
                // 0xC0 is GBC-only. 0x80 is a dual-mode cartridge, so valid
                // ROMX metadata may choose whether it belongs in GB or GBC.
                const bool color = cgbFlag == 0xC0 ||
                    (cgbFlag == 0x80 && info.payloadFormat != "gb");
                if (headerTitle && headerTitle->empty())
                    *headerTitle = trimHeaderTitle(header.data() + 0x134, color ? 15 : 16);
                info.romExtension = color ? ".gbc" : ".gb";
                return static_cast<int>(color
                    ? beiklive::enums::EmuPlatform::EmuGBC
                    : beiklive::enums::EmuPlatform::EmuGB);
            }

            struct SnesCandidate { std::uint64_t offset; };
            static constexpr std::array<SnesCandidate, 6> SnesHeaders{{
                {0x7FC0}, {0xFFC0}, {0x40FFC0},
                {0x7FC0 + 512}, {0xFFC0 + 512}, {0x40FFC0 + 512}}};
            int bestScore = 0;
            std::string bestTitle;
            for (const auto candidate : SnesHeaders)
            {
                if (candidate.offset + 64 > info.romSize)
                    continue;
                std::array<unsigned char, 64> snes{};
                if (!readAt(input, info.romOffset + candidate.offset, snes.data(), snes.size()))
                    continue;
                int score = 0;
                const unsigned char mapMode = snes[0x15] & 0x3F;
                if (mapMode == 0x20 || mapMode == 0x21 || mapMode == 0x22 ||
                    mapMode == 0x23 || mapMode == 0x25 || mapMode == 0x30 ||
                    mapMode == 0x31 || mapMode == 0x32 || mapMode == 0x35)
                    score += 2;
                const std::uint16_t complement = readLe16(snes.data() + 0x1C);
                const std::uint16_t checksum = readLe16(snes.data() + 0x1E);
                if (checksum != 0 && static_cast<std::uint16_t>(checksum + complement) == 0xFFFF)
                    score += 3;
                int printable = 0;
                for (std::size_t i = 0; i < 21; ++i)
                    if (snes[i] == ' ' || (snes[i] >= 0x21 && snes[i] <= 0x7E))
                        ++printable;
                if (printable >= 18)
                    score += 2;
                if (score > bestScore)
                {
                    bestScore = score;
                    bestTitle = trimHeaderTitle(snes.data(), 21);
                }
            }
            if (bestScore >= 4)
            {
                if (headerTitle && headerTitle->empty())
                    *headerTitle = bestTitle;
                info.romExtension = ".sfc";
                return static_cast<int>(beiklive::enums::EmuPlatform::EmuSNES);
            }
            return 0;
        }

        std::string shaHex(const unsigned char* bytes)
        {
            std::ostringstream out;
            out << std::hex << std::setfill('0');
            for (std::size_t i = 0; i < 32; ++i)
                out << std::setw(2) << static_cast<unsigned>(bytes[i]);
            return out.str();
        }

        std::string coverExtension(std::ifstream& input, const Info& info)
        {
            std::array<unsigned char, 12> signature{};
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>(signature.size(), info.coverSize));
            if (count != 0 && readAt(input, info.coverOffset, signature.data(), count))
            {
                static constexpr unsigned char Png[] =
                    {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
                if (count >= sizeof(Png) && std::equal(std::begin(Png), std::end(Png), signature.begin()))
                    return ".png";
                if (count >= 3 && signature[0] == 0xFF && signature[1] == 0xD8 && signature[2] == 0xFF)
                    return ".jpg";
                if (count >= 12 && std::equal(signature.begin(), signature.begin() + 4, "RIFF") &&
                    std::equal(signature.begin() + 8, signature.begin() + 12, "WEBP"))
                    return ".webp";
            }
            return ".img";
        }

        bool copyRange(const std::string& source, std::uint64_t offset, std::uint64_t size,
                       const fs::path& target, std::string* error,
                       const std::string& expectedSha256 = {})
        {
            std::ifstream input(source, std::ios::binary);
            if (!input || offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
            {
                if (error) *error = "无法打开打包游戏";
                return false;
            }
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            const fs::path temporary = target.string() + ".tmp";
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!input || !output)
            {
                if (error) *error = "无法创建解包文件";
                return false;
            }

            std::array<char, 64 * 1024> buffer{};
            Sha256 sha;
            std::uint64_t remaining = size;
            while (remaining != 0)
            {
                const std::size_t chunk = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, buffer.size()));
                input.read(buffer.data(), static_cast<std::streamsize>(chunk));
                if (input.gcount() != static_cast<std::streamsize>(chunk))
                {
                    if (error) *error = "打包游戏数据不完整";
                    output.close();
                    std::error_code removeEc;
                    fs::remove(temporary, removeEc);
                    return false;
                }
                output.write(buffer.data(), static_cast<std::streamsize>(chunk));
                if (!output)
                {
                    if (error) *error = "写入解包文件失败";
                    output.close();
                    std::error_code removeEc;
                    fs::remove(temporary, removeEc);
                    return false;
                }
                sha.update(reinterpret_cast<const unsigned char*>(buffer.data()), chunk);
                remaining -= chunk;
            }
            output.flush();
            if (!output)
            {
                if (error) *error = "写入解包文件失败";
                output.close();
                std::error_code removeEc;
                fs::remove(temporary, removeEc);
                return false;
            }
            output.close();

            const auto digest = sha.finish();
            if (!expectedSha256.empty() && shaHex(digest.data()) != lower(expectedSha256))
            {
                if (error) *error = "解包后的 ROM SHA-256 校验失败";
                std::error_code removeEc;
                fs::remove(temporary, removeEc);
                return false;
            }

            std::error_code ec;
            fs::rename(temporary, target, ec);
            if (!ec)
                return true;

            const fs::path backup = target.string() + ".bak";
            fs::remove(backup, ec);
            ec.clear();
            if (fs::exists(target, ec))
            {
                ec.clear();
                fs::rename(target, backup, ec);
                if (ec)
                {
                    fs::remove(temporary, ec);
                    if (error) *error = "无法替换已有的解包文件";
                    return false;
                }
            }
            ec.clear();
            fs::rename(temporary, target, ec);
            if (!ec)
            {
                std::error_code removeEc;
                fs::remove(backup, removeEc);
                return true;
            }
            std::error_code restoreEc;
            if (fs::exists(backup, restoreEc))
                fs::rename(backup, target, restoreEc);
            fs::remove(temporary, restoreEc);
            if (error) *error = "无法完成解包文件的原子替换";
            return false;
        }
    }

    bool hasSupportedExtension(const std::string& path)
    {
        const std::string ext = lower(fs::path(path).extension().string());
        static constexpr std::array<const char*, 15> Extensions{
            ".gbx", ".gbcx", ".gbax", ".nesx", ".fdsx", ".sfcx", ".smcx",
            ".ndsx", ".mdx", ".genx", ".smdx", ".binx", ".ciax", ".ccix",
            ".3dsx"};
        return std::find(Extensions.begin(), Extensions.end(), ext) != Extensions.end();
    }

    std::optional<Info> readInfo(const std::string& path, std::string* error,
                                 bool verifyPayload)
    {
        if (error) error->clear();
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            if (error) *error = "无法打开打包游戏";
            return std::nullopt;
        }
        input.seekg(0, std::ios::end);
        const std::streamoff end = input.tellg();
        if (end < static_cast<std::streamoff>(FooterSize))
        {
            if (error) *error = "文件太小，不是有效的 ROMX 容器";
            return std::nullopt;
        }
        const std::uint64_t fileSize = static_cast<std::uint64_t>(end);
        std::error_code modifiedEc;
        const auto modified = fs::last_write_time(path, modifiedEc);
        if (!modifiedEc)
        {
            std::lock_guard<std::mutex> lock(InfoCacheMutex);
            const auto cached = InfoCache.find(path);
            if (cached != InfoCache.end() && cached->second.fileSize == fileSize &&
                cached->second.modified == modified &&
                (!verifyPayload || cached->second.fullyVerified))
            {
                return cached->second.info;
            }
        }
        std::array<unsigned char, FooterSize> footer{};
        if (!readAt(input, fileSize - FooterSize, footer.data(), footer.size()))
        {
            if (error) *error = "无法读取 ROMX 文件尾";
            return std::nullopt;
        }
        const bool isStandard = std::equal(footer.begin(), footer.begin() + 4, "ROMX");
        const bool isLegacy = std::equal(footer.begin(), footer.begin() + 4, "GBAX");
        if (!isStandard && !isLegacy)
        {
            if (error) *error = "缺少 ROMX 文件尾";
            return std::nullopt;
        }
        if (readLe32(footer.data() + 4) != SupportedVersion)
        {
            if (error) *error = "不支持的 ROMX 版本";
            return std::nullopt;
        }

        Info info;
        info.legacyContainer = isLegacy;
        info.romOffset = readLe64(footer.data() + 0x08);
        info.romSize = readLe64(footer.data() + 0x10);
        info.metadataOffset = readLe64(footer.data() + 0x18);
        info.metadataSize = readLe64(footer.data() + 0x20);
        info.coverOffset = readLe64(footer.data() + 0x28);
        info.coverSize = readLe64(footer.data() + 0x30);
        info.romSha256 = shaHex(footer.data() + 0x38);

        const std::uint32_t flags = readLe32(footer.data() + 0x58);
        if (isStandard)
        {
            if (readLe32(footer.data() + 0x5C) != FooterSize)
            {
                if (error) *error = "ROMX footer_size 不是 128";
                return std::nullopt;
            }
            if ((flags & ~KnownFlags) != 0)
            {
                if (error) *error = "ROMX 使用了未知的 Footer 标志";
                return std::nullopt;
            }
            if (((flags & HasMetadata) != 0) != (info.metadataSize != 0) ||
                ((flags & HasCover) != 0) != (info.coverSize != 0))
            {
                if (error) *error = "ROMX Footer 标志与数据区域不一致";
                return std::nullopt;
            }
            if ((flags & HasBodySha256) == 0 &&
                std::any_of(footer.begin() + 0x60, footer.end(), [](unsigned char value) {
                    return value != 0;
                }))
            {
                if (error) *error = "ROMX 未启用 body_sha256，但哈希字段不是零";
                return std::nullopt;
            }
        }

        const std::uint64_t payloadEnd = fileSize - FooterSize;
        const auto validRange = [payloadEnd](std::uint64_t offset, std::uint64_t size) {
            return size == 0 || (offset <= payloadEnd && size <= payloadEnd - offset);
        };
        if (info.romSize == 0 || !validRange(info.romOffset, info.romSize) ||
            !validRange(info.metadataOffset, info.metadataSize) ||
            !validRange(info.coverOffset, info.coverSize))
        {
            if (error) *error = "ROMX 偏移或数据大小无效";
            return std::nullopt;
        }
        if (rangesOverlap(info.romOffset, info.romSize, info.metadataOffset, info.metadataSize) ||
            rangesOverlap(info.romOffset, info.romSize, info.coverOffset, info.coverSize) ||
            rangesOverlap(info.metadataOffset, info.metadataSize, info.coverOffset, info.coverSize))
        {
            if (error) *error = "ROMX 数据区域发生重叠";
            return std::nullopt;
        }

        if (isStandard && verifyPayload)
        {
            if (!verifyContainerHashes(input, payloadEnd, info.romOffset, info.romSize,
                                       footer.data() + 0x38,
                                       (flags & HasBodySha256) != 0,
                                       footer.data() + 0x60))
            {
                if (error) *error = "ROMX SHA-256 校验失败";
                return std::nullopt;
            }
        }

        nlohmann::json metadata = nlohmann::json::object();
        if (info.metadataSize != 0 && info.metadataSize <= MaxMetadataSize)
        {
            std::string bytes(static_cast<std::size_t>(info.metadataSize), '\0');
            if (readAt(input, info.metadataOffset, bytes.data(), bytes.size()) &&
                !(isStandard && bytes.size() >= 3 &&
                  static_cast<unsigned char>(bytes[0]) == 0xEF &&
                  static_cast<unsigned char>(bytes[1]) == 0xBB &&
                  static_cast<unsigned char>(bytes[2]) == 0xBF))
            {
                metadata = nlohmann::json::parse(bytes, nullptr, false);
            }
            if (isLegacy && metadata.is_discarded())
                metadata = nlohmann::json::parse(removeTrailingJsonCommas(bytes), nullptr, false);
            if (metadata.is_discarded() || !metadata.is_object())
                metadata = nlohmann::json::object();
            if (isStandard && !validStandardMetadata(metadata))
                metadata = nlohmann::json::object();
        }

        info.title = metadataString(metadata, "label");
        if (isLegacy && info.title.empty()) info.title = metadataString(metadata, "title");
        if (isLegacy && info.title.empty()) info.title = metadataString(metadata, "name");
        info.developer = metadataString(metadata, "developer");
        info.releaseDate = metadataString(metadata, "release_date");
        if (isLegacy && info.releaseDate.empty())
            info.releaseDate = metadataString(metadata, "releaseDate");
        info.region = metadataStringOrList(metadata, "region");
        if (isStandard && !metadata.empty())
            info.metadataJson = metadata.dump();
        if (metadata.contains("genre"))
        {
            if (metadata["genre"].is_array())
            {
                for (const auto& item : metadata["genre"])
                    if (item.is_string() && !item.get<std::string>().empty())
                        info.genre.push_back(item.get<std::string>());
            }
            else if (isLegacy && metadata["genre"].is_string())
            {
                info.genre.push_back(metadata["genre"].get<std::string>());
            }
        }

        info.payloadFormat = lower(metadataString(metadata, "payload_format"));
        std::string headerTitle = metadataString(metadata, "header_title");
        const int detected = detectRomPlatform(input, info, &headerTitle);
        const int declared = metadataPlatform(metadata);
        const std::string aliasExtension = packedBaseExtension(path);
        const int aliased = extensionPlatform(aliasExtension);
        const std::string aliasFormat = aliasExtension.size() > 1
            ? aliasExtension.substr(1) : std::string{};
        info.platform = detected != 0 ? detected : declared != 0 ? declared : aliased;
        if (!info.payloadFormat.empty() && payloadPlatform(info.payloadFormat) == info.platform)
            info.romExtension = payloadExtension(info.payloadFormat);
        else if (payloadPlatform(aliasFormat) == info.platform)
        {
            info.payloadFormat = aliasFormat;
            info.romExtension = aliasExtension;
        }
        else if (info.payloadFormat.empty() && !info.romExtension.empty())
            info.payloadFormat = info.romExtension.substr(1);
        if (info.title.empty())
            info.title = headerTitle;
        if (info.platform == 0)
        {
            if (error) *error = "无法识别 ROMX 容器内的 ROM 平台";
            return std::nullopt;
        }

        if (isStandard && info.coverSize != 0)
        {
            static constexpr std::array<unsigned char, 8> PngSignature{
                0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
            std::array<unsigned char, 24> header{};
            bool validCover = info.coverSize <= MaxCoverSize && info.coverSize >= header.size() &&
                readAt(input, info.coverOffset, header.data(), header.size()) &&
                std::equal(PngSignature.begin(), PngSignature.end(), header.begin()) &&
                std::equal(header.begin() + 12, header.begin() + 16, "IHDR");
            if (validCover)
            {
                const std::uint32_t width = readBe32(header.data() + 16);
                const std::uint32_t height = readBe32(header.data() + 20);
                validCover = width >= 1 && width <= 8192 && height >= 1 && height <= 8192;
            }
            const auto coverMetadata = metadata.find("cover");
            if (verifyPayload && validCover &&
                coverMetadata != metadata.end() && coverMetadata->is_object())
            {
                const std::string expectedCoverSha = metadataString(*coverMetadata, "sha256");
                if (!expectedCoverSha.empty())
                {
                    std::array<unsigned char, 32> coverDigest{};
                    validCover = expectedCoverSha.size() == 64 &&
                        sha256Range(input, info.coverOffset, info.coverSize, coverDigest) &&
                        shaHex(coverDigest.data()) == expectedCoverSha;
                }
            }
            if (!validCover)
            {
                // An invalid optional cover must not make the ROM unusable.
                info.coverSize = 0;
            }
        }
        if (!modifiedEc)
        {
            std::lock_guard<std::mutex> lock(InfoCacheMutex);
            if (InfoCache.size() >= 1024 && InfoCache.find(path) == InfoCache.end())
                InfoCache.clear();
            const auto existing = InfoCache.find(path);
            if (existing == InfoCache.end() || verifyPayload || !existing->second.fullyVerified)
                InfoCache[path] = InfoCacheEntry{fileSize, modified, verifyPayload, info};
        }
        return info;
    }

    std::string extractCover(const std::string& packedPath, const Info& info,
                             const std::string& destinationDir, std::string* error)
    {
        if (info.coverSize == 0 || destinationDir.empty())
            return {};
        std::error_code ec;
        fs::create_directories(destinationDir, ec);
        if (ec)
        {
            if (error) *error = ec.message();
            return {};
        }
        std::ifstream input(packedPath, std::ios::binary);
        if (!input)
            return {};
        const std::string key = info.romSha256.empty() ? "unknown" : info.romSha256.substr(0, 16);
        const fs::path output = fs::path(destinationDir) /
            ("packed_cover_" + key + coverExtension(input, info));
        if (fs::exists(output, ec) && fs::file_size(output, ec) == info.coverSize)
            return output.string();
        return copyRange(packedPath, info.coverOffset, info.coverSize, output, error)
            ? output.string() : std::string{};
    }

    std::string prepareRomForLaunch(const std::string& path, std::string* error)
    {
        if (!hasSupportedExtension(path))
            return path;
        const auto info = readInfo(path, error);
        if (!info)
            return {};

        const std::string ext = info->romExtension.empty()
            ? packedBaseExtension(path) : info->romExtension;
        const fs::path dir = fs::path(beiklive::path::cachePath()) / "packed_roms";
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec)
        {
            if (error) *error = ec.message();
            return {};
        }
        const std::string key = info->romSha256.empty() ? "unknown" : info->romSha256;
        const fs::path output = dir / (key + ext);
        if ((info->legacyContainer && fs::exists(output, ec) &&
             fs::file_size(output, ec) == info->romSize) ||
            (!info->legacyContainer &&
             fileMatchesSha256(output, info->romSize, info->romSha256)))
            return output.string();
        return copyRange(path, info->romOffset, info->romSize, output, error,
                         info->legacyContainer ? std::string{} : info->romSha256)
            ? output.string() : std::string{};
    }
}
