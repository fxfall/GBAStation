#pragma once

#include "RomxGameEntryAdapter.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <romx/romx.h>

namespace beiklive::romx
{
bool isSaveStateArtifact(const std::filesystem::path& path);

std::vector<GameEntryAdapter::LocalSaveCandidate> listThreeDsSaveCandidates(
    const GameEntry& entry, const std::filesystem::path& source,
    std::string* error = nullptr);

SyncResult writeThreeDsSaveCatalog(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::string& preferredKey,
    uint32_t* writtenCount = nullptr,
    std::string* error = nullptr);

bool collectRegularBundleFiles(
    const std::filesystem::path& root,
    std::vector<std::string>& relativePaths,
    std::vector<std::string>& sourcePaths);

SyncResult writeBundlePathEntries(
    const GameEntry& entry,
    romx_mutable_namespace_t objectNamespace,
    const char* key,
    const std::vector<std::string>& relativePaths,
    const std::vector<std::string>& sourcePaths,
    std::string* error = nullptr);
}
