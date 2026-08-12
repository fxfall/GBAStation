#include "core/RomxLaunchSession.hpp"

#include "core/RomxFrontend.hpp"

#include <filesystem>
#include <utility>

namespace beiklive::romx
{

RomxLaunchSession::RomxLaunchSession(std::string sourcePath)
    : m_sourcePath(std::move(sourcePath)),
      m_isRomx(hasSupportedExtension(m_sourcePath))
{
}

std::string RomxLaunchSession::materialize(std::string* error) const
{
    if (error)
        error->clear();
    if (!m_isRomx)
        return m_sourcePath;
    return prepareRomForLaunch(m_sourcePath, error);
}

std::string RomxLaunchSession::pathForCore(bool coreSupportsRomx,
                                           std::string* error) const
{
    if (error)
        error->clear();
    if (!m_isRomx || coreSupportsRomx)
        return m_sourcePath;
    return materialize(error);
}

std::string RomxLaunchSession::logicalPath(std::string* error) const
{
    if (error)
        error->clear();
    if (!m_isRomx)
        return m_sourcePath;

    std::string readError;
    const auto info = readInfo(m_sourcePath, &readError, false);
    if (!info)
    {
        if (error)
            *error = readError;
        return {};
    }

    std::string extension = info->romExtension;
    if (extension.empty())
    {
        const std::string format = logicalExtension(m_sourcePath, info.operator->());
        if (!format.empty())
            extension = "." + format;
    }
    if (extension.empty())
        return m_sourcePath;

    std::filesystem::path path(m_sourcePath);
    path.replace_extension(extension);
    return path.string();
}

bool RomxLaunchSession::loadPayload(std::vector<std::uint8_t>& output,
                                    std::string* error) const
{
    return loadPayloadToMemory(m_sourcePath, output, error);
}

} // 命名空间 beiklive::romx
