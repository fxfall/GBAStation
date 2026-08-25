#pragma once

#include "core/enums.h"

#include <cstdint>
#include <string>
#include <vector>

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
    /// A named SAVE mutable object.  ROMX keys are UTF-8, so displayName may
    /// contain Chinese (or any other valid UTF-8 text) and is deliberately
    /// kept separate from the bundle's internal relative file path.
    struct SaveSlot
    {
        /// The mutable object key.  Multiple logical slots can originate from
        /// one SAVE bundle, so use selectionKey when invoking restoreSave or
        /// exportSave for an existing slot.
        std::string key;
        std::string displayName;
        /// Opaque adapter selector.  Empty means `key`; libromx-derived bundle
        /// slots use this to retain both the object key and selected slot key.
        std::string selectionKey;
        std::string entryPath;
        uint64_t dataSize = 0;
        uint64_t generation = 0;
        uint32_t entryCount = 0;
    };

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

    /// Enumerates active ROMX SAVE bundle objects.  An empty result means the
    /// container has no readable SAVE objects (or no mutable region); this is
    /// not itself an error so callers can still offer “new save”.
    static std::vector<SaveSlot> listSaveSlots(const GameEntry& entry,
                                               std::string* error = nullptr);

    /// Validates a user supplied UTF-8 SAVE object key before a write.  Keys
    /// are labels, not local paths: slash, dot components, NUL, and an empty
    /// label are rejected.  The byte limit follows ROMX 0.2.0's 448-byte key
    /// capacity (not a character count).
    static bool validateSaveSlotKey(const std::string& key,
                                    std::string* error = nullptr);

    /// Explicit batch-management operations.  The restore variants replace
    /// local frontend data with the corresponding ROMX mutable object.  PSP
    /// maps SAVE/CHEAT to GBAStation/saves/PSP and GBAStation/PSP/Cheats;
    /// each PSP SAVE directory is presented as a separate frontend slot, but
    /// restore always replaces one local active DISC_ID directory.  The
    /// selectionKey returned by listSaveSlots is an adapter-only selector for
    /// a logical slot inside an RMBL SAVE bundle.  Other platforms continue
    /// to use the interoperable `libretro` object.  On built-in
    /// battery-backed platforms, SAVE restore derives the local destination
    /// from `GameEntry.path` (`<stem>.sav`, or Genesis `.srm`) instead of
    /// trusting the mutable bundle's relative filename.  SAVE export for
    /// non-PSP entries contains only the core's stem-matched battery file,
    /// never savestates/thumbnails.
    static SyncResult restoreSave(GameEntry& entry, std::string* error = nullptr);
    /// Restores one named SAVE object.  The key is the ROMX slot label and is
    /// interpreted as UTF-8; selecting an existing key is an overwrite of the
    /// local battery file, not a new local filename.  Callers should pass
    /// SaveSlot::selectionKey when selecting a projected bundle slot (PSP
    /// directory slot or a non-PSP multi-file slot).
    static SyncResult restoreSave(GameEntry& entry, const std::string& key,
                                  std::string* error = nullptr);
    static SyncResult exportSave(const GameEntry& entry, std::string* error = nullptr);
    /// Writes one named SAVE object.  Existing keys are replaced; a new key
    /// consumes another mutable object slot when the ROMX region has room.
    static SyncResult exportSave(const GameEntry& entry, const std::string& key,
                                 std::string* error = nullptr);
    static SyncResult restoreCheat(GameEntry& entry, std::string* error = nullptr);
    static SyncResult exportCheat(const GameEntry& entry, std::string* error = nullptr);
    static SyncResult restoreStats(GameEntry& entry, std::string* error = nullptr);
    static SyncResult exportStats(const GameEntry& entry, std::string* error = nullptr);

    /// Returns the frontend cache directory used for extracted entrypoints.
    static std::string payloadCacheDirectory();
};

using RomxGameEntryAdapter = GameEntryAdapter;

} // namespace beiklive::romx
