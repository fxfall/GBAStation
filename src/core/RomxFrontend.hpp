#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace beiklive::romx
{

/**
 * 前端使用的 ROMX 容器视图。
 *
 * 容器、footer 和区域校验统一由 libromx 负责；此结构体只保留 GBAStation
 * 显示游戏、选择核心和向核心适配器传递 payload 所需的字段。
 */
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
    std::uint32_t flags = 0;
    std::uint32_t version = 0;
    std::uint64_t fileSize = 0;
    std::uint64_t bodySize = 0;
    std::uint64_t romOffset = 0;
    std::uint64_t romSize = 0;
    std::uint64_t metadataOffset = 0;
    std::uint64_t metadataSize = 0;
    std::uint64_t coverOffset = 0;
    std::uint64_t coverSize = 0;
};

bool hasSupportedExtension(const std::string& path);

/** 通过 libromx 打开 ROMX，只读取前端需要的 metadata 字段。 */
std::optional<Info> readInfo(const std::string& path, std::string* error = nullptr,
                             bool verifyPayload = true);

/** 通过 libromx 提取可选的 PNG 封面。 */
std::string extractCover(const std::string& packedPath, const Info& info,
                         const std::string& destinationDir,
                         std::string* error = nullptr);

/** 仅供 path-only 核心使用的兼容回退；这里不解析 footer。 */
std::string prepareRomForLaunch(const std::string& path,
                                std::string* error = nullptr);

/**
 * 读取 ROMX 的 ROM 区域到内存。
 *
 * 仅给明确支持 retro_game_info.data 的核心使用；path-only 核心仍应调用
 * prepareRomForLaunch()，这样不会把整个容器传给核心。
 */
bool loadPayloadToMemory(const std::string& path, std::vector<std::uint8_t>& output,
                         std::string* error = nullptr);

/** 返回 ROMX 路径对应的逻辑扩展名（不含开头的点）。 */
std::string logicalExtension(const std::string& path, const Info* info = nullptr);

} // 命名空间 beiklive::romx
