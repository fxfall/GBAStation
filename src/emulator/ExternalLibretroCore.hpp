#pragma once

#if defined(__APPLE__) && !defined(__SWITCH__)

#include "emulator/IEmulatorCore.hpp"

#include <string>

namespace beiklive::external
{

/// Desktop adapter for a dynamically loaded libretro core.
///
/// On macOS, PSP and 3DS request the libretro Vulkan path and the host copies
/// each completed Vulkan frame into the existing GameView video pipeline.
/// FBNeo keeps its normal CPU video callback.  The external-core boundary
/// remains in one place, so the existing UI and renderer do not need to know
/// which path the core uses.
class ExternalLibretroCore final : public IEmulatorCore
{
public:
    ExternalLibretroCore(std::string corePath, std::string coreName, int platform);
    ~ExternalLibretroCore() override;

    bool SetupGame(beiklive::GameEntry GameEntry) override;
    void Cleanup() override;
    void RunFrame() override;
    void Reset() override;

    bool Serialize(std::vector<uint8_t>& outBuf) const override;
    bool Unserialize(const std::vector<uint8_t>& buf) override;

    LibretroLoader::VideoFrame GetVideoFrame() const override { return m_core.getVideoFrame(); }
    bool DrainAudio(std::vector<int16_t>& out) override { return m_core.drainAudio(out); }

    void SetButtonState(unsigned player, unsigned id, bool pressed) override;
    void SetButtonsFromSignal(unsigned player) override;

    unsigned GameWidth() const override { return m_core.gameWidth(); }
    unsigned GameHeight() const override { return m_core.gameHeight(); }
    double Fps() const override { return m_core.fps(); }
    double SampleRate() const override { return m_core.sampleRate(); }

    void SetFastForwarding(bool ff) override { m_core.setFastForwarding(ff); }
    void NotifyConfigUpdated() override { m_core.notifyConfigUpdated(); }

    void ApplyCheats(const std::vector<CheatEntry>& cheats) override;
    void SetCheatPath(const std::string& path) override { m_gameEntry.cheatPath = path; }

    bool IsReady() const override { return m_ready; }
    std::string LastError() const override { return m_lastError; }

    LibretroLoader::DiskControlState GetDiskControlState() const override
    {
        return m_core.diskControlState();
    }
    bool SetDiskEjected(bool ejected) override { return m_core.setDiskEjected(ejected); }
    bool SetDiskImageIndex(unsigned index, bool insertAfter = true) override
    {
        return m_core.switchDiskImage(index, insertAfter);
    }

    const void* getSramData() const override
    {
        return m_core.getMemoryData(RETRO_MEMORY_SAVE_RAM);
    }
    size_t getSramSize() const override
    {
        return m_core.getMemorySize(RETRO_MEMORY_SAVE_RAM);
    }
    bool saveSram() override;

private:
    void initConfig();
    bool waitForInitialFrame();
    bool loadSram();
    bool saveSramInternal();
    bool loadCheats();
    void updateCheats();
    std::string saveDirectory() const;

    GameEntry m_gameEntry;
    LibretroLoader m_core;
    std::vector<CheatEntry> m_cheats;
    std::string m_corePath;
    std::string m_coreName;
    int m_platform = static_cast<int>(enums::EmuPlatform::NONE);
    std::string m_lastError;
    bool m_ready = false;
};

} // namespace beiklive::external

#endif // defined(__APPLE__) && !defined(__SWITCH__)
