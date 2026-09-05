#include "RomxVfs.hpp"

#include "RomxFrontend.hpp"

#include <romx/romx.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;

// The Libretro header intentionally leaves these handles opaque.  The
// frontend owns their concrete representation and only passes pointers to the
// core through the public ABI.
struct retro_vfs_file_handle
{
    bool romx = false;
    bool mapped = false;
    FILE* normal = nullptr;
    romx_vfs_file* romxFile = nullptr;
    const std::uint8_t* mappedData = nullptr;
    std::uint64_t size = 0;
    std::uint64_t position = 0;
    std::string path;
    const void* owner = nullptr;
};

struct retro_vfs_dir_handle
{
    const void* owner = nullptr;
    std::vector<std::pair<std::string, bool>> entries;
    std::size_t index = 0;
    std::string current;
    bool currentIsDirectory = false;
};

namespace beiklive::romx_vfs
{
using beiklive::romx::LaunchSession;

namespace
{
struct Binding
{
    const void* owner = nullptr;
    LaunchSession* session = nullptr;
    bool active = false;
    bool deactivationRequested = false;
    std::size_t handles = 0;
    std::string virtualPrefix;
    std::string entrypointPath;
};

Binding g_binding;
std::mutex g_mutex;

std::string fnvPathId(const std::string& path)
{
    std::uint64_t hash = UINT64_C(1469598103934665603);
    for (unsigned char byte : path)
    {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(hash));
    return buffer;
}

bool hasPrefix(const std::string& path, const std::string& prefix)
{
    return !prefix.empty() && path.compare(0, prefix.size(), prefix) == 0;
}

std::string relativeVirtualPath(const Binding& binding, const char* path)
{
    if (!path)
        return {};
    const std::string value(path);
    if (!hasPrefix(value, binding.virtualPrefix))
        return {};
    std::string relative = value.substr(binding.virtualPrefix.size());
    while (!relative.empty() && relative.front() == '/')
        relative.erase(relative.begin());
    return relative.empty() ? binding.entrypointPath : relative;
}

std::string relativeVirtualDirectoryPath(const Binding& binding, const char* path)
{
    if (!path)
        return {};
    const std::string value(path);
    if (!hasPrefix(value, binding.virtualPrefix))
        return {};
    std::string relative = value.substr(binding.virtualPrefix.size());
    while (!relative.empty() && relative.front() == '/')
        relative.erase(relative.begin());
    while (!relative.empty() && relative.back() == '/')
        relative.pop_back();
    if (relative == ".")
        relative.clear();
    return relative;
}

bool isVirtualPathLocked(const Binding& binding, const char* path)
{
    return binding.active && path && hasPrefix(path, binding.virtualPrefix);
}

void releaseBindingLocked()
{
    g_binding = {};
}

void releaseHandleLocked(const void* owner)
{
    if (owner && g_binding.owner == owner && g_binding.handles > 0)
        --g_binding.handles;
    if (owner && g_binding.owner == owner &&
        g_binding.deactivationRequested && g_binding.handles == 0)
        releaseBindingLocked();
}

const char* getPath(retro_vfs_file_handle* stream)
{
    return stream ? stream->path.c_str() : nullptr;
}

retro_vfs_file_handle* open(const char* path, unsigned mode, unsigned)
{
    if (!path || !*path)
        return nullptr;

    auto file = std::make_unique<retro_vfs_file_handle>();
    file->path = path;

    Binding* binding = nullptr;
    LaunchSession* session = nullptr;
    std::string relative;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (isVirtualPathLocked(g_binding, path))
        {
            if ((mode & RETRO_VFS_FILE_ACCESS_READ) == 0 ||
                (mode & (RETRO_VFS_FILE_ACCESS_WRITE |
                         RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING)) != 0)
                return nullptr;
            binding = &g_binding;
            session = binding->session;
            relative = relativeVirtualPath(*binding, path);
            if (!session || relative.empty())
                return nullptr;
            ++binding->handles;
        }
    }

    if (binding)
    {
        file->romx = true;
        file->owner = binding->owner;
        std::string error;
        const std::string entrypoint = session->info().entrypointPath;
        if (relative == entrypoint && !session->info().multiFile)
        {
            const void* data = nullptr;
            std::uint64_t size = 0;
            if (session->mapPayload(&data, &size, &error) && data &&
                size <= static_cast<std::uint64_t>(SIZE_MAX))
            {
                file->mapped = true;
                file->mappedData = static_cast<const std::uint8_t*>(data);
                file->size = size;
            }
        }
        if (!file->mapped)
        {
            if (!session->openVfs(relative, &file->romxFile, &error))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                releaseHandleLocked(binding->owner);
                return nullptr;
            }
            if (!file->romxFile ||
                romx_vfs_file_get_size(file->romxFile, &file->size, nullptr) != ROMX_OK)
            {
                if (file->romxFile)
                    romx_vfs_file_close(file->romxFile);
                file->romxFile = nullptr;
                std::lock_guard<std::mutex> lock(g_mutex);
                releaseHandleLocked(binding->owner);
                return nullptr;
            }
        }
        return file.release();
    }

    const bool read = (mode & RETRO_VFS_FILE_ACCESS_READ) != 0;
    const bool write = (mode & RETRO_VFS_FILE_ACCESS_WRITE) != 0;
    const bool update = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) != 0;
    const char* flags = nullptr;
    if (read && write)
        flags = update ? "r+b" : "w+b";
    else if (write)
        flags = "wb";
    else
        flags = "rb";
    file->normal = std::fopen(path, flags);
    if (!file->normal)
        return nullptr;
    return file.release();
}

int close(retro_vfs_file_handle* stream)
{
    if (!stream)
        return -1;
    if (stream->normal)
        std::fclose(stream->normal);
    if (stream->romxFile)
        romx_vfs_file_close(stream->romxFile);
    const void* owner = stream->owner;
    delete stream;

    if (owner)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        releaseHandleLocked(owner);
    }
    return 0;
}

std::int64_t size(retro_vfs_file_handle* stream)
{
    if (!stream)
        return -1;
    if (stream->romx) {
        if (stream->size > static_cast<std::uint64_t>(INT64_MAX))
            return -1;
        return static_cast<std::int64_t>(stream->size);
    }
    const long current = std::ftell(stream->normal);
    if (current < 0 || std::fseek(stream->normal, 0, SEEK_END) != 0)
        return -1;
    const long end = std::ftell(stream->normal);
    (void)std::fseek(stream->normal, current, SEEK_SET);
    return end < 0 ? -1 : static_cast<std::int64_t>(end);
}

std::int64_t tell(retro_vfs_file_handle* stream)
{
    if (!stream)
        return -1;
    if (stream->romx)
        return stream->position > static_cast<std::uint64_t>(INT64_MAX)
            ? -1 : static_cast<std::int64_t>(stream->position);
    const long position = std::ftell(stream->normal);
    return position < 0 ? -1 : static_cast<std::int64_t>(position);
}

std::int64_t seek(retro_vfs_file_handle* stream, std::int64_t offset, int origin)
{
    if (!stream)
        return -1;
    if (stream->romx)
    {
        std::uint64_t base = 0;
        if (origin == RETRO_VFS_SEEK_POSITION_END)
            base = stream->size;
        else if (origin == RETRO_VFS_SEEK_POSITION_CURRENT)
            base = stream->position;
        std::uint64_t target = 0;
        if (offset >= 0)
        {
            const auto positive = static_cast<std::uint64_t>(offset);
            if (base > UINT64_MAX - positive)
                return -1;
            target = base + positive;
        }
        else
        {
            const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1U;
            if (magnitude > base)
                return -1;
            target = base - magnitude;
        }
        if (target > stream->size || target > static_cast<std::uint64_t>(INT64_MAX))
            return -1;
        if (!stream->mapped && stream->romxFile) {
            uint64_t newPosition = 0;
            romx_error_t error{};
            const auto vfsOrigin = origin == RETRO_VFS_SEEK_POSITION_END
                ? ROMX_PAYLOAD_SEEK_END
                : origin == RETRO_VFS_SEEK_POSITION_CURRENT
                    ? ROMX_PAYLOAD_SEEK_CURRENT : ROMX_PAYLOAD_SEEK_START;
            if (romx_vfs_file_seek(stream->romxFile, offset, vfsOrigin,
                                   &newPosition, &error) != ROMX_OK)
                return -1;
            target = newPosition;
        }
        stream->position = target;
        return static_cast<std::int64_t>(target);
    }
    const int whence = origin == RETRO_VFS_SEEK_POSITION_END ? SEEK_END
        : origin == RETRO_VFS_SEEK_POSITION_CURRENT ? SEEK_CUR : SEEK_SET;
    if (std::fseek(stream->normal, static_cast<long>(offset), whence) != 0)
        return -1;
    return tell(stream);
}

std::int64_t read(retro_vfs_file_handle* stream, void* buffer, std::uint64_t length)
{
    if (!stream || (!buffer && length != 0))
        return -1;
    if (stream->romx)
    {
        const std::uint64_t available = stream->position < stream->size
            ? stream->size - stream->position : 0;
        const std::uint64_t wanted = std::min(length, available);
        if (wanted == 0)
            return 0;
        if (wanted > static_cast<std::uint64_t>(SIZE_MAX) ||
            wanted > static_cast<std::uint64_t>(INT64_MAX))
            return -1;
        if (stream->mapped)
        {
            std::memcpy(buffer, stream->mappedData + stream->position,
                        static_cast<std::size_t>(wanted));
            stream->position += wanted;
            return static_cast<std::int64_t>(wanted);
        }
        uint64_t count = 0;
        romx_error_t error{};
        const romx_result_t result = romx_vfs_file_read(
            stream->romxFile, buffer, wanted, &count, &error);
        if (result != ROMX_OK)
            return -1;
        stream->position += count;
        return count > static_cast<std::uint64_t>(INT64_MAX)
            ? -1 : static_cast<std::int64_t>(count);
    }
    const std::uint64_t wanted = std::min(length,
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()));
    return static_cast<std::int64_t>(std::fread(buffer, 1,
        static_cast<std::size_t>(wanted), stream->normal));
}

std::int64_t write(retro_vfs_file_handle* stream, const void* buffer,
                   std::uint64_t length)
{
    if (!stream || stream->romx || (!buffer && length != 0))
        return -1;
    const std::uint64_t wanted = std::min(length,
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()));
    return static_cast<std::int64_t>(std::fwrite(buffer, 1,
        static_cast<std::size_t>(wanted), stream->normal));
}

int flush(retro_vfs_file_handle* stream)
{
    return !stream || stream->romx ? -1 : std::fflush(stream->normal);
}

std::int64_t truncate(retro_vfs_file_handle* stream, std::int64_t length)
{
    if (!stream || length < 0 || stream->romx)
        return -1;
    std::error_code error;
    fs::resize_file(stream->path, static_cast<std::uintmax_t>(length), error);
    return error ? -1 : 0;
}

int removePath(const char* path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (isVirtualPathLocked(g_binding, path))
        return -1;
    return std::remove(path) == 0 ? 0 : -1;
}

int renamePath(const char* oldPath, const char* newPath)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (isVirtualPathLocked(g_binding, oldPath) ||
        isVirtualPathLocked(g_binding, newPath))
        return -1;
    return std::rename(oldPath, newPath) == 0 ? 0 : -1;
}

bool hiddenName(const std::string& name)
{
    return name.size() > 1 && name.front() == '.';
}

bool addDirectoryEntry(retro_vfs_dir_handle& directory,
                       const std::string& name, bool isDirectory,
                       bool includeHidden)
{
    if (name.empty() || (!includeHidden && hiddenName(name)))
        return true;
    for (auto& entry : directory.entries) {
        if (entry.first == name) {
            entry.second = entry.second || isDirectory;
            return true;
        }
    }
    directory.entries.emplace_back(name, isDirectory);
    return true;
}

bool collectRomxDirectoryEntries(const LaunchSession& session,
                                 const std::string& directoryPath,
                                 bool includeHidden,
                                 retro_vfs_dir_handle& output)
{
    const romx_reader* reader = session.reader();
    if (!reader)
        return false;
    uint32_t count = 0;
    if (romx_reader_get_entry_count(reader, &count, nullptr) != ROMX_OK)
        return false;
    std::string prefix = directoryPath;
    if (!prefix.empty())
        prefix.push_back('/');
    for (uint32_t index = 0; index < count; ++index) {
        romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
        if (romx_reader_get_entry(reader, index, &entry, nullptr) != ROMX_OK)
            return false;
        const std::string path(entry.path);
        if (!prefix.empty() && path.compare(0, prefix.size(), prefix) != 0)
            continue;
        const std::string remainder = path.substr(prefix.size());
        if (remainder.empty())
            continue;
        const std::size_t slash = remainder.find('/');
        const std::string child = remainder.substr(0, slash);
        if (!addDirectoryEntry(output, child, slash != std::string::npos,
                               includeHidden))
            return false;
    }
    return true;
}

retro_vfs_dir_handle* openDirectory(const char* path, bool includeHidden)
{
    if (!path || !*path)
        return nullptr;
    auto directory = std::make_unique<retro_vfs_dir_handle>();

    LaunchSession* session = nullptr;
    std::string relative;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (isVirtualPathLocked(g_binding, path)) {
            session = g_binding.session;
            relative = relativeVirtualDirectoryPath(g_binding, path);
            if (!session || !collectRomxDirectoryEntries(*session, relative,
                                                          includeHidden,
                                                          *directory))
                return nullptr;
            directory->owner = g_binding.owner;
            ++g_binding.handles;
            return directory.release();
        }
    }

    std::error_code error;
    const fs::directory_iterator iterator(path, error);
    if (error)
        return nullptr;
    for (const auto& item : iterator) {
        const std::string name = item.path().filename().string();
        if (!includeHidden && hiddenName(name))
            continue;
        std::error_code itemError;
        addDirectoryEntry(*directory, name, item.is_directory(itemError),
                          includeHidden);
    }
    return directory.release();
}

bool readDirectory(retro_vfs_dir_handle* directory)
{
    if (!directory || directory->index >= directory->entries.size())
        return false;
    directory->current = directory->entries[directory->index].first;
    directory->currentIsDirectory = directory->entries[directory->index].second;
    ++directory->index;
    return true;
}

const char* directoryEntryName(retro_vfs_dir_handle* directory)
{
    return directory && !directory->current.empty()
        ? directory->current.c_str() : nullptr;
}

bool directoryEntryIsDirectory(retro_vfs_dir_handle* directory)
{
    return directory && directory->currentIsDirectory;
}

int closeDirectory(retro_vfs_dir_handle* directory)
{
    if (!directory)
        return -1;
    const void* owner = directory->owner;
    delete directory;
    if (owner) {
        std::lock_guard<std::mutex> lock(g_mutex);
        releaseHandleLocked(owner);
    }
    return 0;
}

int stat(const char* path, std::int32_t* resultSize)
{
    if (!path)
        return 0;
    std::uint64_t sizeValue = 0;
    int resultFlags = RETRO_VFS_STAT_IS_VALID;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (isVirtualPathLocked(g_binding, path))
        {
            const std::string directoryRelative =
                relativeVirtualDirectoryPath(g_binding, path);
            if (directoryRelative.empty()) {
                if (!g_binding.session)
                    return 0;
                retro_vfs_dir_handle directory;
                if (!collectRomxDirectoryEntries(*g_binding.session, {}, true,
                                                  directory))
                    return 0;
                resultFlags |= RETRO_VFS_STAT_IS_DIRECTORY;
                if (resultSize)
                    *resultSize = 0;
                return resultFlags;
            }
            const std::string relative = relativeVirtualPath(g_binding, path);
            if (!g_binding.session)
                return 0;
            romx_vfs_file* file = nullptr;
            if (g_binding.session->openVfs(relative, &file, nullptr)) {
                const bool ok = romx_vfs_file_get_size(file, &sizeValue, nullptr) == ROMX_OK;
                romx_vfs_file_close(file);
                if (!ok)
                    return 0;
            } else {
                retro_vfs_dir_handle directory;
                if (!collectRomxDirectoryEntries(
                        *g_binding.session, relative, true, directory) ||
                    directory.entries.empty())
                    return 0;
                resultFlags |= RETRO_VFS_STAT_IS_DIRECTORY;
            }
        }
        else
        {
            std::error_code error;
            const fs::file_status status = fs::status(path, error);
            if (error || status.type() == fs::file_type::not_found)
                return 0;
            if (fs::is_directory(status))
                resultFlags |= RETRO_VFS_STAT_IS_DIRECTORY;
            else if (fs::is_regular_file(status)) {
                sizeValue = fs::file_size(path, error);
                if (error)
                    return 0;
            } else
                return 0;
        }
    }
    if (resultSize)
        *resultSize = sizeValue > static_cast<std::uint64_t>(INT32_MAX)
            ? INT32_MAX : static_cast<std::int32_t>(sizeValue);
    return resultFlags;
}

int mkdirPath(const char* path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (isVirtualPathLocked(g_binding, path))
        return -1;
    std::error_code error;
    if (fs::create_directory(path, error))
        return 0;
    return error ? -1 : -2;
}

retro_vfs_interface g_interface{
    getPath, open, close, size, tell, seek, read, write, flush,
    removePath, renamePath, truncate, stat, mkdirPath,
    openDirectory, readDirectory, directoryEntryName,
    directoryEntryIsDirectory, closeDirectory
};

} // namespace

std::string makeVirtualPath(const std::string& sourcePath,
                            const std::string& entrypointPath)
{
    std::string relative = entrypointPath;
    if (relative.empty())
        relative = "entrypoint.rom";
    while (!relative.empty() && relative.front() == '/')
        relative.erase(relative.begin());
    return "romx://" + fnvPathId(sourcePath) + "/" + relative;
}

bool activate(const void* owner, const std::string& virtualPath,
              LaunchSession* session)
{
    if (!owner || !session || virtualPath.empty())
        return false;
    const std::size_t schemeEnd = virtualPath.find("://");
    const std::size_t slash = schemeEnd == std::string::npos
        ? std::string::npos : virtualPath.find('/', schemeEnd + 3);
    if (slash == std::string::npos)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_binding.owner)
        return false;
    g_binding.owner = owner;
    g_binding.session = session;
    g_binding.active = true;
    g_binding.virtualPrefix = virtualPath.substr(0, slash + 1);
    g_binding.entrypointPath = session->info().entrypointPath;
    return true;
}

void deactivate(const void* owner)
{
    if (!owner)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_binding.owner != owner)
        return;
    g_binding.active = false;
    g_binding.deactivationRequested = true;
    if (g_binding.handles == 0)
        releaseBindingLocked();
}

retro_vfs_interface* interfacePtr()
{
    return &g_interface;
}

} // namespace beiklive::romx_vfs
