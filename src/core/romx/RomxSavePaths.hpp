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
    romx_mutable_save_layout_info_t saveLayout);

std::filesystem::path threeDsSaveTransactionDirectory(
    std::string titleId,
    romx_mutable_save_layout_info_t saveLayout);
}
