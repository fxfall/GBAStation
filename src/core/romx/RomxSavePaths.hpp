#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace beiklive::romx
{
using SavePathMapper = std::function<std::optional<std::filesystem::path>(
    const std::filesystem::path& relative, uint32_t index, uint32_t count)>;

// The returned mapper owns the selected source directory. targetDirectory is
// intentionally a reference because the caller uses it as the destination
// selection for the duration of the synchronous bundle operation.
SavePathMapper makePspSaveOutputMapper(std::string& targetDirectory,
                                       std::string requestedDirectory);
}
