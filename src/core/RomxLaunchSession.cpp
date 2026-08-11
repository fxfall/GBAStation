#include "core/RomxLaunchSession.hpp"

#include "core/RomxFrontend.hpp"

#include <romx/romx.h>

#include <filesystem>
#include <utility>

namespace beiklive::romx
{

RomxLaunchSession::RomxLaunchSession(std::string sourcePath)
    : m_sourcePath(std::move(sourcePath)),
      m_isRomx(hasSupportedExtension(m_sourcePath))
{
}

RomxLaunchSession::~RomxLaunchSession()
{
    romx_payload_mapping_close(m_mapping);
}

bool RomxLaunchSession::mapPayload(std::string* error)
{
    if (error)
        error->clear();
    if (!m_isRomx)
    {
        if (error)
            *error = "不是 ROMX 路径";
        return false;
    }
    if (m_mapping)
        return true;

    romx_reader_t* reader = nullptr;
    romx_error_t value{};
    if (romx_reader_open_path(m_sourcePath.c_str(), nullptr, &reader, &value) != ROMX_OK)
    {
        if (error)
            *error = value.message;
        return false;
    }

    const romx_result_t result = romx_reader_map_payload(reader, &m_mapping, &value);
    romx_reader_close(reader);
    if (result != ROMX_OK || !m_mapping)
    {
        if (error)
            *error = value.message[0] ? value.message : "payload mapping 失败";
        romx_payload_mapping_close(m_mapping);
        m_mapping = nullptr;
        return false;
    }
    return true;
}

const void* RomxLaunchSession::mappedData() const
{
    return romx_payload_mapping_data(m_mapping);
}

std::uint64_t RomxLaunchSession::mappedSize() const
{
    return romx_payload_mapping_size(m_mapping);
}

romx_payload_mapping_t* RomxLaunchSession::takeMapping()
{
    auto* mapping = m_mapping;
    m_mapping = nullptr;
    return mapping;
}

std::string RomxLaunchSession::materialize(std::string* error) const
{
    if (error)
        error->clear();
    if (!m_isRomx)
        return m_sourcePath;
    return prepareRomForLaunch(m_sourcePath, error);
}

std::string RomxLaunchSession::logicalExtension(std::string* error) const
{
    if (error)
        error->clear();
    if (!m_isRomx)
    {
        const std::string extension = std::filesystem::path(m_sourcePath).extension().string();
        return extension.size() > 1 ? extension.substr(1) : std::string{};
    }

    std::string readError;
    const auto info = readInfo(m_sourcePath, &readError, false);
    if (!info)
    {
        if (error)
            *error = readError;
        return {};
    }
    return beiklive::romx::logicalExtension(m_sourcePath, &*info);
}

std::uint64_t RomxLaunchSession::payloadSize(std::string* error) const
{
    if (error)
        error->clear();
    if (!m_isRomx)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(m_sourcePath, ec);
        if (ec)
        {
            if (error)
                *error = ec.message();
            return 0;
        }
        return size;
    }
    if (m_mapping)
        return mappedSize();
    std::string readError;
    const auto info = readInfo(m_sourcePath, &readError, false);
    if (!info)
    {
        if (error)
            *error = readError;
        return 0;
    }
    return info->romSize;
}

} // 命名空间 beiklive::romx
