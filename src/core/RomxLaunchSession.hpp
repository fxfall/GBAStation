#pragma once

#include <cstdint>
#include <string>

struct romx_payload_mapping;
typedef struct romx_payload_mapping romx_payload_mapping_t;

namespace beiklive::romx
{

/**
 * ROMX 启动会话，统一管理 payload mapping、逻辑路径和解包回退。
 * mapping 的所有权由会话持有，调用 takeMapping() 后转交给核心适配器。
 */
class RomxLaunchSession final
{
public:
    explicit RomxLaunchSession(std::string sourcePath);
    ~RomxLaunchSession();

    RomxLaunchSession(const RomxLaunchSession&) = delete;
    RomxLaunchSession& operator=(const RomxLaunchSession&) = delete;

    bool isRomx() const { return m_isRomx; }
    const std::string& sourcePath() const { return m_sourcePath; }

    /** 打开并建立 payload mapping；非 ROMX 路径返回 false。 */
    bool mapPayload(std::string* error = nullptr);

    const void* mappedData() const;
    std::uint64_t mappedSize() const;
    romx_payload_mapping_t* takeMapping();

    /** 为只支持路径的核心生成缓存 payload；普通 ROM 原样返回。 */
    std::string materialize(std::string* error = nullptr) const;

    /** 返回核心应看到的 payload 扩展名。 */
    std::string logicalExtension(std::string* error = nullptr) const;

    /** 返回 ROMX payload 大小；映射不可用时仍可通过元数据取得。 */
    std::uint64_t payloadSize(std::string* error = nullptr) const;

private:
    std::string m_sourcePath;
    bool m_isRomx = false;
    romx_payload_mapping_t* m_mapping = nullptr;
};

} // 命名空间 beiklive::romx
