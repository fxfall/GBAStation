#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <romx/romx.h>

namespace beiklive::romx
{
using SavePathMapper = std::function<std::optional<std::filesystem::path>(
    const std::filesystem::path& relative, uint32_t index, uint32_t count)>;

// Validates and normalizes a ROMX-relative path without consulting the host's
// canonical path machinery. This is shared by bundle input validation and the
// destination resolver so a mapper cannot weaken the path policy.
bool normalizeSafeMutableRelativePath(
    const std::filesystem::path& relative,
    std::filesystem::path& normalized,
    std::string* error = nullptr);

// Resolves a validated ROMX-relative path below a caller-selected trusted
// root. The root may be a platform virtual path such as Switch's sdmc:/...;
// it is intentionally never canonicalized.
bool resolveSafeMutablePath(
    const std::filesystem::path& trustedRoot,
    const std::filesystem::path& relative,
    std::filesystem::path& output,
    std::string* error = nullptr);

// The returned mapper owns the selected source directory. targetDirectory is
// intentionally a reference because the caller uses it as the destination
// selection for the duration of the synchronous bundle operation.
SavePathMapper makePspSaveOutputMapper(std::string& targetDirectory,
                                       std::string requestedDirectory);

uint64_t expandedMutableBundleCapacity(uint64_t measuredSize,
                                       bool allowGrowth);

// Converts libromx's semantic 3DS layout into paths relative to the native
// SD root. The layout is copied so the mapper never borrows a caller-local
// metadata structure.
SavePathMapper makeThreeDsSaveOutputMapper(
    std::string titleId,
    romx_mutable_save_layout_info_t saveLayout,
    std::string* mappingError = nullptr);

std::filesystem::path threeDsSaveTransactionDirectory(
    std::string titleId,
    romx_mutable_save_layout_info_t saveLayout);
}
