#pragma once

#include "core/enums.h"

#include <string>

namespace beiklive::romx
{

struct RomxGameEntryOptions
{
    int fallbackPlatform = static_cast<int>(beiklive::enums::EmuPlatform::NONE);
    std::string fallbackTitle;
    std::string coverDirectory;
    bool verifyPayload = false;
    bool extractCover = true;
    bool forceCover = false;
    bool createSaveDirectory = true;
};

struct RomxGameEntryResult
{
    bool romxCandidate = false;
    bool romxValid = false;
    bool changed = false;
    std::string error;
};

/**
 * 将 ROMX metadata、封面和 ROM Header 信息统一应用到 GameEntry。
 * 页面和启动入口只调用这个适配器，不直接解析 ROMX。
 */
class RomxGameEntryAdapter final
{
public:
    static RomxGameEntryResult apply(GameEntry& entry,
                                     const std::string& path,
                                     const RomxGameEntryOptions& options = {});

    /** 读取 ROMX 平台；无法识别时返回调用方提供的回退平台。 */
    static int detectPlatform(const std::string& path,
                              int fallbackPlatform,
                              std::string* error = nullptr);
};

} // 命名空间 beiklive::romx
