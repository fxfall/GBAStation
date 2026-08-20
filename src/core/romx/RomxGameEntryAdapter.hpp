#pragma once

#include "core/enums.h"

#include <string>

namespace beiklive::romx
{

enum class SyncResult
{
    Success,
    Skipped,
    Failed,
};

/// Projects ROMX 0.2.0 into the existing GameEntry without making UI pages
/// depend on libromx.  The original .romx path is always retained in entry.path.
class GameEntryAdapter
{
public:
    struct Options
    {
        bool extractCover = true;
        bool importMutable = true;
        bool preserveUserTitle = true;
    };

    static bool apply(const std::string& path, GameEntry& entry,
                      std::string* error = nullptr);
    static bool apply(const std::string& path, GameEntry& entry,
                      const Options& options, std::string* error = nullptr);

    /// Explicitly synchronizes the frontend counters into ROMX STATS/default.
    /// Failure is non-fatal when the container has no mutable capacity.
    static bool writeStats(const GameEntry& entry, std::string* error = nullptr);

    /// Exports the active frontend SAVE/CHEAT files as interoperable ROMX
    /// mutable bundles.  The operation is best-effort: containers without a
    /// reserved mutable region simply return false and leave host files intact.
    static bool writeMutable(const GameEntry& entry, std::string* error = nullptr);

    /// Explicit batch-management operations.  The restore variants replace
    /// local frontend data with the corresponding ROMX mutable object.  PSP
    /// uses the native `ppsspp` bundle key and maps SAVE/CHEAT to
    /// GBAStation/saves/PSP and GBAStation/PSP/Cheats respectively; other
    /// platforms continue to use the interoperable `libretro` object.  SAVE
    /// export for non-PSP entries contains only the core's stem-matched
    /// battery file (`.sav`, or Genesis `.srm`), never savestates/thumbnails.
    static SyncResult restoreSave(GameEntry& entry, std::string* error = nullptr);
    static SyncResult exportSave(const GameEntry& entry, std::string* error = nullptr);
    static SyncResult restoreCheat(GameEntry& entry, std::string* error = nullptr);
    static SyncResult exportCheat(const GameEntry& entry, std::string* error = nullptr);
    static SyncResult restoreStats(GameEntry& entry, std::string* error = nullptr);
    static SyncResult exportStats(const GameEntry& entry, std::string* error = nullptr);

    /// Returns the frontend cache directory used for extracted entrypoints.
    static std::string payloadCacheDirectory();
};

using RomxGameEntryAdapter = GameEntryAdapter;

} // namespace beiklive::romx
