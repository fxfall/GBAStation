#include "LibretroRomxSession.hpp"
#include "RomxVfs.hpp"

#include <algorithm>
#include <cctype>

namespace beiklive::romx
{

LibretroRomxSession::~LibretroRomxSession()
{
    close();
}

bool LibretroRomxSession::prepare(const std::string& source,
    const LibretroLaunchOptions& options, const std::string& cacheDirectory,
    std::string* error)
{
    close();
    if (error)
        error->clear();
    if (!session_.open(source, error))
        return false;
    path_ = source;
    cacheDirectory_ = cacheDirectory;

    if (options.preferZipPayload && options.needFullpath)
    {
        std::string entrypoint = session_.info().entrypointPath;
        std::transform(entrypoint.begin(), entrypoint.end(), entrypoint.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (session_.info().entryCount != 1 || entrypoint.size() < 4 ||
            entrypoint.compare(entrypoint.size() - 4, 4, ".zip") != 0)
        {
            if (error)
                *error = "FBNeo ROMX must contain exactly one ZIP entrypoint";
            close();
            return false;
        }
        // FBNeo identifies the driver using the container filename and consumes
        // the mapped ZIP bytes itself. Never unzip arcade payloads here.
        if (session_.mapPayload(&data_, &size_, error) && data_ && size_ != 0)
            return true;
        if (error && error->empty())
            *error = "ROMX ZIP payload is empty or cannot be mapped";
        close();
        return false;
    }

    if (options.needFullpath && options.useVfs)
    {
        const std::string virtualPath = romx_vfs::makeVirtualPath(
            source, session_.info().entrypointPath);
        if (romx_vfs::activate(this, virtualPath, &session_))
        {
            vfs_ = true;
            path_ = virtualPath;
            return true;
        }
    }
    else if (!options.needFullpath && session_.mapPayload(&data_, &size_, nullptr))
        return true;

    if (materializeFallback(error))
        return true;
    close();
    return false;
}

bool LibretroRomxSession::materializeFallback(std::string* error)
{
    if (vfs_)
        romx_vfs::deactivate(this);
    vfs_ = false;
    data_ = nullptr;
    size_ = 0;
    return session_.materializeEntrypoint(cacheDirectory_, path_, error);
}

void LibretroRomxSession::close()
{
    if (vfs_)
        romx_vfs::deactivate(this);
    vfs_ = false;
    session_.close();
    data_ = nullptr;
    size_ = 0;
    path_.clear();
    cacheDirectory_.clear();
}

} // namespace beiklive::romx
