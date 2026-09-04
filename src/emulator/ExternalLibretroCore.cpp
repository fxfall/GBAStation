#if defined(__APPLE__) && !defined(__SWITCH__)

#include "ExternalLibretroCore.hpp"

#include "core/CoreUtils.hpp"
#include "core/GameSignal.hpp"
#include "core/Tools.hpp"

#include <filesystem>
#include <chrono>
#include <thread>

namespace beiklive::external
{

ExternalLibretroCore::ExternalLibretroCore(std::string corePath,
                                           std::string coreName,
                                           int platform)
    : m_corePath(std::move(corePath)),
      m_coreName(std::move(coreName)),
      m_platform(platform)
{
}

ExternalLibretroCore::~ExternalLibretroCore()
{
    Cleanup();
}

bool ExternalLibretroCore::SetupGame(beiklive::GameEntry GameEntry)
{
    Cleanup();

    m_gameEntry = std::move(GameEntry);
    m_lastError.clear();
    initConfig();

    if (m_corePath.empty() || !std::filesystem::exists(m_corePath))
    {
        m_lastError = m_coreName + " 核心文件不存在:\n" + m_corePath;
        brls::Logger::error("ExternalLibretroCore: missing {} core: {}",
                            m_coreName, m_corePath);
        return false;
    }

    if (!m_core.load(m_corePath))
    {
        m_lastError = m_coreName + " 动态核心加载失败:\n" + m_corePath;
        brls::Logger::error("ExternalLibretroCore: failed to load {}: {}",
                            m_coreName, m_corePath);
        return false;
    }

    if (!m_core.initCore())
    {
        m_lastError = m_coreName + " 核心初始化失败";
        m_core.unload();
        return false;
    }

    m_core.setControllerPortDevice(0, RETRO_DEVICE_JOYPAD);
    m_core.setControllerPortDevice(1, RETRO_DEVICE_JOYPAD);

    if (m_gameEntry.path.empty() || !std::filesystem::exists(m_gameEntry.path))
    {
        m_lastError = "游戏文件不存在:\n" + m_gameEntry.path;
        m_core.unload();
        return false;
    }

    if (!m_core.loadGame(m_gameEntry.path))
    {
        m_lastError = m_coreName + " 游戏加载失败:\n" + m_gameEntry.path;
        brls::Logger::error("ExternalLibretroCore: retro_load_game failed for {}: {}",
                            m_coreName, m_gameEntry.path);
        m_core.unload();
        return false;
    }

    if (!waitForInitialFrame())
    {
        m_lastError = m_coreName + " 首帧输出超时";
        brls::Logger::error("ExternalLibretroCore: {} did not produce an initial frame",
                            m_coreName);
        m_core.unload();
        return false;
    }

    loadSram();
    loadCheats();
    m_ready = true;

    brls::Logger::info("ExternalLibretroCore: {} loaded {} ({}x{} @ {:.2f} fps)",
                       m_coreName, m_gameEntry.path,
                       m_core.gameWidth(), m_core.gameHeight(), m_core.fps());
    return true;
}

bool ExternalLibretroCore::waitForInitialFrame()
{
    // Path-oriented cores such as PPSSPP and Azahar return from
    // retro_load_game() before their loader thread reaches the first frame.
    // Yielding here prevents the frontend from immediately tearing down a
    // still-booting core when SetupGame returns.
    constexpr unsigned kMaxPolls = 10000;
    const auto started = std::chrono::steady_clock::now();
    const auto minimumStartup = m_platform == static_cast<int>(enums::EmuPlatform::EmuPSP)
        ? std::chrono::milliseconds(1500)
        : std::chrono::milliseconds(0);
    bool gotFrame = false;
    for (unsigned poll = 0; poll < kMaxPolls; ++poll)
    {
        m_core.run();
        gotFrame = gotFrame || !m_core.getVideoFrame().pixels.empty();
        if (gotFrame && std::chrono::steady_clock::now() - started >= minimumStartup)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void ExternalLibretroCore::Cleanup()
{
    if (m_ready)
        saveSramInternal();
    m_ready = false;
    m_core.unload();
    m_cheats.clear();
}

void ExternalLibretroCore::RunFrame()
{
    if (m_ready)
        m_core.run();
}

void ExternalLibretroCore::Reset()
{
    if (m_ready)
        m_core.reset();
}

bool ExternalLibretroCore::Serialize(std::vector<uint8_t>& outBuf) const
{
    if (!m_ready)
        return false;
    const size_t size = m_core.serializeSize();
    if (size == 0)
        return false;
    outBuf.resize(size);
    return m_core.serialize(outBuf.data(), size);
}

bool ExternalLibretroCore::Unserialize(const std::vector<uint8_t>& buf)
{
    if (!m_ready || buf.empty())
        return false;
    return m_core.unserialize(buf.data(), buf.size());
}

void ExternalLibretroCore::SetButtonState(unsigned player, unsigned id, bool pressed)
{
    m_core.setButtonState(player, id, pressed);
}

void ExternalLibretroCore::SetButtonsFromSignal(unsigned player)
{
    auto& signal = GameSignal::instance();
    const uint32_t mask = signal.getGameButtonMask(player);
    for (unsigned id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
        m_core.setButtonState(player, id, (mask >> id) & 1u);

    const auto analog = signal.getGameAnalogState(player);
    m_core.setAnalogState(player, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                          RETRO_DEVICE_ID_ANALOG_X, analog.leftStickX);
    m_core.setAnalogState(player, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                          RETRO_DEVICE_ID_ANALOG_Y, analog.leftStickY);
    m_core.setAnalogState(player, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                          RETRO_DEVICE_ID_ANALOG_X, analog.rightStickX);
    m_core.setAnalogState(player, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                          RETRO_DEVICE_ID_ANALOG_Y, analog.rightStickY);
    m_core.setAnalogState(player, RETRO_DEVICE_INDEX_ANALOG_BUTTON,
                          RETRO_DEVICE_ID_JOYPAD_L2,
                          static_cast<int16_t>(analog.leftTrigger * 0x7FFF / 0xFF));
    m_core.setAnalogState(player, RETRO_DEVICE_INDEX_ANALOG_BUTTON,
                          RETRO_DEVICE_ID_JOYPAD_R2,
                          static_cast<int16_t>(analog.rightTrigger * 0x7FFF / 0xFF));
}

void ExternalLibretroCore::ApplyCheats(const std::vector<CheatEntry>& cheats)
{
    m_cheats = cheats;
    updateCheats();
}

bool ExternalLibretroCore::saveSram()
{
    return saveSramInternal();
}

void ExternalLibretroCore::initConfig()
{
    ConfigManager* config = SettingManager;
    m_core.setConfigManager(config);
    m_core.setRomxVfsAllowed(false);
    m_core.setRomxPayloadPreferred(
        m_platform == static_cast<int>(enums::EmuPlatform::EmuArcade));

    // This is a transient host capability override.  It does not rewrite the
    // user's saved core preference.  The macOS host keeps the UI on OpenGL,
    // while PPSSPP/Azahar render through the negotiated Vulkan path and hand
    // frames back to LibretroLoader for readback.
    m_core.setVulkanPreferred(
        m_platform == static_cast<int>(enums::EmuPlatform::EmuPSP) ||
        m_platform == static_cast<int>(enums::EmuPlatform::Emu3DS));
    if (config)
    {
        using CV = ConfigValue;
        if (m_platform == static_cast<int>(enums::EmuPlatform::EmuPSP))
        {
            config->Set("core.ppsspp_backend", CV("vulkan"), false);
            config->Set("core.ppsspp_software_rendering", CV("disabled"), false);
        }
        else if (m_platform == static_cast<int>(enums::EmuPlatform::Emu3DS))
        {
            config->Set("core.citra_graphics_api", CV("Vulkan"), false);
        }
    }
    std::error_code directoryError;
    std::filesystem::create_directories(saveDirectory(), directoryError);
    if (directoryError)
    {
        brls::Logger::warning("ExternalLibretroCore: failed to create save directory {}: {}",
                              saveDirectory(), directoryError.message());
    }

    // PPSSPP expects <system>/PPSSPP/{assets,compat.ini}; the 3DS core uses
    // this as its system/keys root.  Keeping the roots below GBAStation/cores
    // prevents external-core assets from being mixed with BIOS files.
    m_core.setSystemDirectory(path::corePath());

    // PPSSPP treats GET_SAVE_DIRECTORY as the memstick root and creates its
    // own PSP/SAVEDATA hierarchy.  Other cores use the per-game directory.
    m_core.setSaveDirectory(saveDirectory());
}

std::string ExternalLibretroCore::saveDirectory() const
{
    if (m_platform == static_cast<int>(enums::EmuPlatform::EmuPSP))
        return path::savePath();
    if (!m_gameEntry.savePath.empty())
        return m_gameEntry.savePath;
    return tools::defaultGameSavePath(m_gameEntry.platform, m_gameEntry.path);
}

bool ExternalLibretroCore::loadSram()
{
    if (m_platform == static_cast<int>(enums::EmuPlatform::EmuPSP) ||
        m_platform == static_cast<int>(enums::EmuPlatform::Emu3DS) ||
        m_platform == static_cast<int>(enums::EmuPlatform::EmuArcade))
    {
        // These cores own platform-specific save formats/directories.  The
        // ROMX adapter synchronizes their mutable SAVE namespace on exit.
        return true;
    }
    return core_utils::loadSram(
        m_core, saveDirectory(), tools::getFileNameWithoutExtension(m_gameEntry.path));
}

bool ExternalLibretroCore::saveSramInternal()
{
    if (!m_core.isLoaded())
        return true;
    if (m_platform == static_cast<int>(enums::EmuPlatform::EmuPSP) ||
        m_platform == static_cast<int>(enums::EmuPlatform::Emu3DS) ||
        m_platform == static_cast<int>(enums::EmuPlatform::EmuArcade))
        return true;
    return core_utils::saveSram(
        m_core, saveDirectory(), tools::getFileNameWithoutExtension(m_gameEntry.path));
}

bool ExternalLibretroCore::loadCheats()
{
    return core_utils::loadCheats(m_core, m_gameEntry.cheatPath, m_cheats);
}

void ExternalLibretroCore::updateCheats()
{
    core_utils::updateCheats(m_core, m_cheats);
}

} // namespace beiklive::external

#endif // defined(__APPLE__) && !defined(__SWITCH__)
