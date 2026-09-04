#include "IEmulatorCore.hpp"
#include "emulator/CoreFceumm.hpp"
#include "emulator/CoreSnes9x.hpp"
#include "emulator/mgba_native/MgbaNativeCore.hpp"
#include "emulator/genesis/GenesisCore.h"
#if defined(__APPLE__) && !defined(__SWITCH__)
#include "emulator/ExternalLibretroCore.hpp"
#endif

namespace beiklive {

IEmulatorCore* CreateEmulatorCore(const beiklive::GameEntry& entry)
{
    const int platform = entry.platform;
    const std::string coreId = beiklive::NormalizeCoreId(platform, entry.core);
    switch (static_cast<beiklive::enums::EmuPlatform>(platform))
    {
    case beiklive::enums::EmuPlatform::EmuGBA:
        return new beiklive::mgba_native::MgbaNativeCore();
    case beiklive::enums::EmuPlatform::EmuGBC:
    case beiklive::enums::EmuPlatform::EmuGB:
        if (coreId == "gambatte")
            return new beiklive::fceumm::CoreFceumm(beiklive::CoreType::Gambatte, "Gambatte");
        return new beiklive::mgba_native::MgbaNativeCore();
    case beiklive::enums::EmuPlatform::EmuNES:
        if (coreId == "nestopia")
            return new beiklive::fceumm::CoreFceumm(beiklive::CoreType::Nestopia, "Nestopia");
        return new beiklive::fceumm::CoreFceumm(beiklive::CoreType::Fceumm, "FCEUmm");
    case beiklive::enums::EmuPlatform::EmuSNES:
        if (coreId == "snes9x2005")
            return new beiklive::snes9x::CoreSnes9x(beiklive::CoreType::Snes9x2005, "Snes9x 2005");
        if (coreId == "snes9x")
            return new beiklive::snes9x::CoreSnes9x(beiklive::CoreType::Snes9x, "Snes9x");
        return new beiklive::snes9x::CoreSnes9x(beiklive::CoreType::Snes9x2005, "Snes9x 2005");
    case beiklive::enums::EmuPlatform::EmuGenesis:
        return new beiklive::genesis::GenesisCore();
#if defined(__APPLE__) && !defined(__SWITCH__)
    case beiklive::enums::EmuPlatform::Emu3DS:
        return new beiklive::external::ExternalLibretroCore(
            beiklive::GetCorePath(platform), "Azahar", platform);
    case beiklive::enums::EmuPlatform::EmuArcade:
        return new beiklive::external::ExternalLibretroCore(
            beiklive::GetCorePath(platform), "FBNeo", platform);
    case beiklive::enums::EmuPlatform::EmuPSP:
        return new beiklive::external::ExternalLibretroCore(
            beiklive::GetCorePath(platform), "PPSSPP", platform);
#else
    case beiklive::enums::EmuPlatform::EmuNDS:
    case beiklive::enums::EmuPlatform::Emu3DS:
    case beiklive::enums::EmuPlatform::EmuArcade:
    case beiklive::enums::EmuPlatform::EmuDreamcast:
    case beiklive::enums::EmuPlatform::EmuPSP:
#endif
    case beiklive::enums::EmuPlatform::EmuPS1:
    case beiklive::enums::EmuPlatform::EmuSaturn:
    case beiklive::enums::EmuPlatform::EmuDolphin:
        return nullptr;
    default:
        return nullptr;
    }
}

} // namespace beiklive
