#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace beiklive::romx
{

/**
 * ROMX 启动会话。
 *
 * ROMX metadata/封面读取仍由 RomxFrontend 和 RomxGameEntryAdapter 负责；
 * 核心启动统一在这里选择 payload 传递方式：支持 ROMX 的 path-only 核心
 * 直接收到原始容器路径，其他 path-only 核心才通过 materialize() 使用普通
 * payload 文件。
 */
class RomxLaunchSession final
{
public:
    explicit RomxLaunchSession(std::string sourcePath);
    ~RomxLaunchSession() = default;

    RomxLaunchSession(const RomxLaunchSession&) = delete;
    RomxLaunchSession& operator=(const RomxLaunchSession&) = delete;

    bool isRomx() const { return m_isRomx; }
    const std::string& sourcePath() const { return m_sourcePath; }

    /** 为 path-only 核心生成持久化缓存 payload；普通 ROM 原样返回。 */
    std::string materialize(std::string* error = nullptr) const;

    /**
     * 为外部核心选择启动路径。
     *
     * 支持 ROMX 的核心直接使用 sourcePath()，不会创建或写入 payload 缓存；
     * 不支持 ROMX 的核心沿用 materialize() 回退。普通 ROM 始终原样返回。
     */
    std::string pathForCore(bool coreSupportsRomx,
                            std::string* error = nullptr) const;

    /** 返回带标准 payload 扩展名的逻辑路径，不创建或提取文件。 */
    std::string logicalPath(std::string* error = nullptr) const;

    /** 为数据型核心读取 payload；普通 ROM 也由这里统一读取。 */
    bool loadPayload(std::vector<std::uint8_t>& output,
                     std::string* error = nullptr) const;

private:
    std::string m_sourcePath;
    bool m_isRomx = false;
};

} // 命名空间 beiklive::romx
