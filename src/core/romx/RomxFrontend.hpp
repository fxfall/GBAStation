#pragma once

#include "core/enums.h"

#include <cstdint>
#include <string>

struct romx_reader;
struct romx_payload_mapping;
struct romx_vfs_file;

namespace beiklive::romx
{

/** A small, read-only description of a ROMX 0.2.0 container.
 *
 * The frontend deliberately keeps the normative platform and launch values
 * from the footer/RIDX separate from the descriptive metadata JSON.  Unknown
 * metadata fields are preserved in metadataJson, while only the fields needed
 * by GameEntry are projected into this structure.
 */
struct Info
{
    std::string sourcePath;
    std::string metadataJson;
    std::string title;
    std::string serial;
    std::string entrypointPath;
    std::string entrypointFormat;
    std::string platformName;

    int platform = static_cast<int>(enums::EmuPlatform::NONE);
    uint16_t platformId = 0;
    uint16_t launchFormatId = 0;
    uint16_t entrypointFormatId = 0;
    uint32_t entryCount = 0;
    uint32_t crc32 = 0;
    uint64_t entrypointSize = 0;
    uint64_t coverSize = 0;
    bool hasCover = false;
    bool multiFile = false;
};

/// Returns true for the canonical ROMX 0.2.0 container extension.  The
/// pre-0.2 profile aliases (gbx/gbax/nesx/...) are intentionally not accepted
/// on the new main branch.
bool isRomxPath(const std::string& path);

/// Converts a ROMX footer platform id to the frontend platform enum.
int platformFromRomxId(uint16_t platformId);

/// Reads footer, RIDX entrypoint, optional metadata, and cover information.
/// This does not validate every payload byte and never loads the payload into
/// memory merely to inspect a game.
bool readInfo(const std::string& path, Info& out, std::string* error = nullptr);

/// Extracts the embedded PNG into a stable frontend cache path.  Existing
/// files are reused after a successful ROMX open.
bool extractCover(const std::string& path, std::string& outPath,
                  const std::string& cacheDirectory = {});

/// Session used by cores and future external integrations.  A mapping is
/// independent of the reader lifetime; VFS cursors borrow the reader and are
/// therefore closed before this session.
class LaunchSession
{
public:
    LaunchSession();
    ~LaunchSession();
    LaunchSession(const LaunchSession&) = delete;
    LaunchSession& operator=(const LaunchSession&) = delete;

    bool open(const std::string& path, std::string* error = nullptr);
    void close();
    bool isOpen() const { return reader_ != nullptr; }
    const Info& info() const { return info_; }

    /// Maps only a single-file entrypoint.  The pointer remains valid until
    /// close() and is never a pointer into metadata, cover, or footer bytes.
    bool mapPayload(const void** data, uint64_t* size, std::string* error = nullptr);

    /// Materializes only the entrypoint as a fallback for cores that require a
    /// filesystem path.  Multi-file descriptors are extracted as descriptors;
    /// referenced files remain available through openVfs().
    bool materializeEntrypoint(const std::string& cacheDirectory,
                               std::string& outPath,
                               std::string* error = nullptr);

    /// Opens a virtual entry (including the entrypoint) for descriptor-aware
    /// cores.  The returned handle borrows this session and must be closed by
    /// romx_vfs_file_close().
    bool openVfs(const std::string& virtualPath, romx_vfs_file** outFile,
                 std::string* error = nullptr);

    const romx_reader* reader() const { return reader_; }

private:
    std::string sourcePath_;
    Info info_;
    romx_reader* reader_ = nullptr;
    romx_payload_mapping* mapping_ = nullptr;
};

} // namespace beiklive::romx
