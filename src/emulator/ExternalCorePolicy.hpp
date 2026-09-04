#pragma once

#include "core/enums.h"

namespace beiklive::external
{
    inline bool runsInProcess(beiklive::enums::EmuPlatform platform)
    {
#if defined(__APPLE__) && !defined(__SWITCH__)
        return platform == beiklive::enums::EmuPlatform::Emu3DS ||
               platform == beiklive::enums::EmuPlatform::EmuArcade ||
               platform == beiklive::enums::EmuPlatform::EmuPSP;
#else
        (void)platform;
        return false;
#endif
    }

    inline bool runsInProcess(beiklive::enums::FileType type)
    {
        using beiklive::enums::EmuPlatform;
        using beiklive::enums::FileType;
        switch (type)
        {
        case FileType::THREEDS_ROM:
            return runsInProcess(EmuPlatform::Emu3DS);
        case FileType::ARCADE_ROM:
            return runsInProcess(EmuPlatform::EmuArcade);
        case FileType::PSP_ROM:
            return runsInProcess(EmuPlatform::EmuPSP);
        default:
            return false;
        }
    }
}
