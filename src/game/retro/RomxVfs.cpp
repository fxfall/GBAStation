#include "game/retro/RomxVfs.hpp"

#include <romx/romx.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>

namespace fs = std::filesystem;

namespace beiklive::romx_vfs
{
namespace
{
struct Binding
{
    const void* owner = nullptr;
    bool active = false;
    bool deactivationRequested = false;
    std::size_t handles = 0;
    std::string virtualPath;
    std::string sourcePath;
    std::uint64_t payloadSize = 0;
    romx_payload_mapping_t* mapping = nullptr;
};

struct File
{
    bool romx = false;
    bool mapped = false;
    FILE* normal = nullptr;
    romx_reader_t* reader = nullptr;
    Binding* binding = nullptr;
    const std::uint8_t* mappedData = nullptr;
    std::uint64_t size = 0;
    std::uint64_t position = 0;
    std::string path;
};

Binding active;

void releaseActive()
{
    romx_payload_mapping_close(active.mapping);
    active = {};
}

bool isActivePath(const char* path)
{
    return path && active.active && active.virtualPath == path;
}

const char* getPath(retro_vfs_file_handle* stream)
{
    auto* file = reinterpret_cast<File*>(stream);
    return file ? file->path.c_str() : nullptr;
}

retro_vfs_file_handle* open(const char* path, unsigned mode, unsigned)
{
    if (!path || !*path) return nullptr;
    auto file = std::make_unique<File>();
    file->path = path;
    if (isActivePath(path))
    {
        if ((mode & RETRO_VFS_FILE_ACCESS_READ) == 0 ||
            (mode & (RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING)) != 0)
            return nullptr;
        file->romx = true;
        file->binding = &active;
        if (active.mapping)
        {
            const auto* data = static_cast<const std::uint8_t*>(romx_payload_mapping_data(active.mapping));
            const auto size = romx_payload_mapping_size(active.mapping);
            if (!data || size != active.payloadSize || size > static_cast<std::uint64_t>(INT64_MAX)) return nullptr;
            file->mapped = true; file->mappedData = data; file->size = size;
        }
        else
        {
            romx_error_t error{};
            if (romx_reader_open_path(active.sourcePath.c_str(), nullptr, &file->reader, &error) != ROMX_OK)
                return nullptr;
            romx_info_t info = ROMX_INFO_INIT;
            if (romx_reader_get_info(file->reader, &info, &error) != ROMX_OK || info.rom.size != active.payloadSize)
            {
                romx_reader_close(file->reader); file->reader = nullptr; return nullptr;
            }
            file->size = info.rom.size;
        }
        ++active.handles;
        return reinterpret_cast<retro_vfs_file_handle*>(file.release());
    }

    const char* flags = nullptr;
    const bool read = (mode & RETRO_VFS_FILE_ACCESS_READ) != 0;
    const bool write = (mode & RETRO_VFS_FILE_ACCESS_WRITE) != 0;
    const bool update = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) != 0;
    if (read && write) flags = update ? "r+b" : "w+b";
    else if (write) flags = "wb";
    else flags = "rb";
    file->normal = std::fopen(path, flags);
    if (!file->normal) return nullptr;
    return reinterpret_cast<retro_vfs_file_handle*>(file.release());
}

int close(retro_vfs_file_handle* stream)
{
    auto* file = reinterpret_cast<File*>(stream);
    if (!file) return -1;
    if (file->normal) std::fclose(file->normal);
    romx_reader_close(file->reader);
    if (file->romx && file->binding && file->binding->handles > 0) --file->binding->handles;
    Binding* binding = file->binding;
    delete file;
    if (binding && binding->deactivationRequested && binding->handles == 0) releaseActive();
    return 0;
}

std::int64_t size(retro_vfs_file_handle* stream)
{
    auto* file = reinterpret_cast<File*>(stream);
    if (!file) return -1;
    if (file->romx) return file->size <= static_cast<std::uint64_t>(INT64_MAX) ? static_cast<std::int64_t>(file->size) : -1;
    const long current = std::ftell(file->normal);
    if (current < 0 || std::fseek(file->normal, 0, SEEK_END) != 0) return -1;
    const long end = std::ftell(file->normal);
    std::fseek(file->normal, current, SEEK_SET);
    return end < 0 ? -1 : static_cast<std::int64_t>(end);
}

std::int64_t tell(retro_vfs_file_handle* stream)
{
    auto* file = reinterpret_cast<File*>(stream);
    if (!file) return -1;
    if (file->romx) return file->position <= static_cast<std::uint64_t>(INT64_MAX) ? static_cast<std::int64_t>(file->position) : -1;
    const long position = std::ftell(file->normal);
    return position < 0 ? -1 : static_cast<std::int64_t>(position);
}

std::int64_t seek(retro_vfs_file_handle* stream, std::int64_t offset, int position)
{
    auto* file = reinterpret_cast<File*>(stream);
    if (!file) return -1;
    if (file->romx)
    {
        std::int64_t base = position == RETRO_VFS_SEEK_POSITION_END ? static_cast<std::int64_t>(file->size) :
            position == RETRO_VFS_SEEK_POSITION_CURRENT ? static_cast<std::int64_t>(file->position) : 0;
        if (offset < 0 && base < -offset) return -1;
        const std::int64_t next = base + offset;
        if (next < 0 || static_cast<std::uint64_t>(next) > file->size) return -1;
        file->position = static_cast<std::uint64_t>(next);
        return next;
    }
    const int origin = position == RETRO_VFS_SEEK_POSITION_END ? SEEK_END : position == RETRO_VFS_SEEK_POSITION_CURRENT ? SEEK_CUR : SEEK_SET;
    if (std::fseek(file->normal, static_cast<long>(offset), origin) != 0) return -1;
    return tell(stream);
}

std::int64_t read(retro_vfs_file_handle* stream, void* buffer, std::uint64_t length)
{
    auto* file = reinterpret_cast<File*>(stream);
    if (!file || (!buffer && length != 0)) return -1;
    if (file->romx)
    {
        const std::uint64_t available = file->position < file->size ? file->size - file->position : 0;
        std::uint64_t wanted = std::min(length, available);
        wanted = std::min(wanted, static_cast<std::uint64_t>(INT64_MAX));
        wanted = std::min(wanted, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()));
        if (wanted == 0) return 0;
        if (file->mapped) std::memcpy(buffer, file->mappedData + file->position, static_cast<std::size_t>(wanted));
        else
        {
            std::uint64_t count = 0; romx_error_t error{};
            if (romx_reader_read_region(file->reader, ROMX_REGION_ROM, file->position, buffer, wanted, &count, &error) != ROMX_OK) return -1;
            file->position += count;
            if (count != wanted) return static_cast<std::int64_t>(count);
        }
        if (file->mapped) file->position += wanted;
        return wanted <= static_cast<std::uint64_t>(INT64_MAX) ? static_cast<std::int64_t>(wanted) : -1;
    }
    const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(length, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())));
    return static_cast<std::int64_t>(std::fread(buffer, 1, wanted, file->normal));
}

std::int64_t write(retro_vfs_file_handle* stream, const void* buffer, std::uint64_t length)
{
    auto* file = reinterpret_cast<File*>(stream);
    if (!file || file->romx || (!buffer && length != 0)) return -1;
    const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(length, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())));
    return static_cast<std::int64_t>(std::fwrite(buffer, 1, wanted, file->normal));
}

int flush(retro_vfs_file_handle* stream)
{
    auto* file = reinterpret_cast<File*>(stream);
    return !file || file->romx ? -1 : std::fflush(file->normal);
}

std::int64_t truncate(retro_vfs_file_handle*, std::int64_t)
{
    return -1;
}

int remove(const char* path)
{
    if (isActivePath(path)) return -1;
    return std::remove(path) == 0 ? 0 : -1;
}

int rename(const char* oldPath, const char* newPath)
{
    if (isActivePath(oldPath) || isActivePath(newPath)) return -1;
    return std::rename(oldPath, newPath) == 0 ? 0 : -1;
}

int stat(const char* path, std::int32_t* resultSize)
{
    if (!path) return 0;
    std::uint64_t sizeValue = 0;
    if (isActivePath(path)) sizeValue = active.payloadSize;
    else
    {
        std::error_code error;
        if (!fs::is_regular_file(path, error)) return 0;
        sizeValue = fs::file_size(path, error);
        if (error) return 0;
    }
    if (resultSize) *resultSize = sizeValue > static_cast<std::uint64_t>(INT32_MAX) ? INT32_MAX : static_cast<std::int32_t>(sizeValue);
    return RETRO_VFS_STAT_IS_VALID;
}

retro_vfs_interface interfaceValue{
    getPath, open, close, size, tell, seek, read, write, flush,
    remove, rename, truncate, stat, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
}

bool activate(const void* owner, const std::string& virtualPath,
              const std::string& sourcePath, std::uint64_t payloadSize,
              romx_payload_mapping_t* mapping)
{
    if (!owner || virtualPath.empty() || sourcePath.empty() || active.owner) return false;
    active.owner = owner; active.active = true; active.virtualPath = virtualPath;
    active.sourcePath = sourcePath; active.payloadSize = payloadSize; active.mapping = mapping;
    return true;
}

void deactivate(const void* owner)
{
    if (!owner || active.owner != owner) return;
    active.active = false; active.deactivationRequested = true;
    if (active.handles == 0) releaseActive();
}

retro_vfs_interface* interfacePtr()
{
    return &interfaceValue;
}

} // 命名空间 beiklive::romx_vfs
