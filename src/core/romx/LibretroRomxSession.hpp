#pragma once

#include "RomxFrontend.hpp"

namespace beiklive::romx
{

struct LibretroLaunchOptions
{
    bool needFullpath = false;
    bool useVfs = false;
    bool preferZipPayload = false;
};

/// Owns the frontend's mapping, VFS binding and cache fallback until the core
/// has unloaded. libromx knows neither libretro nor the frontend cache policy.
class LibretroRomxSession
{
public:
    ~LibretroRomxSession();
    LibretroRomxSession() = default;
    LibretroRomxSession(const LibretroRomxSession&) = delete;
    LibretroRomxSession& operator=(const LibretroRomxSession&) = delete;

    bool prepare(const std::string& source, const LibretroLaunchOptions& options,
                 const std::string& cacheDirectory, std::string* error = nullptr);
    bool materializeFallback(std::string* error = nullptr);
    void close();

    const std::string& path() const { return path_; }
    const void* data() const { return data_; }
    uint64_t size() const { return size_; }
    bool usesVfs() const { return vfs_; }

private:
    LaunchSession session_;
    std::string path_;
    std::string cacheDirectory_;
    const void* data_ = nullptr;
    uint64_t size_ = 0;
    bool vfs_ = false;
};

} // namespace beiklive::romx
