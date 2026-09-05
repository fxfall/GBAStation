#pragma once

#include <filesystem>
#include <string>

namespace beiklive::romx
{
enum class DirectoryRestoreResult
{
    Ready,
    Skipped,
    Failed,
};

struct DirectoryRestoreTransaction
{
    std::filesystem::path finalDirectory;
    std::filesystem::path stagingDirectory;
    std::filesystem::path backupDirectory;
    bool movedExisting = false;
    bool active = false;
};

DirectoryRestoreResult prepareDirectoryRestore(
    const std::filesystem::path& finalDirectory,
    bool overwrite,
    const std::string& token,
    DirectoryRestoreTransaction& transaction,
    std::string* error);

// Adopts an already populated staging directory. This is useful for platform
// importers (such as PSP savedata) that must validate or amend files before
// the shared directory-level backup/publish step.
DirectoryRestoreResult prepareDirectoryRestoreWithStaging(
    const std::filesystem::path& finalDirectory,
    const std::filesystem::path& stagingDirectory,
    bool overwrite,
    const std::string& token,
    DirectoryRestoreTransaction& transaction,
    std::string* error);

bool commitDirectoryRestore(DirectoryRestoreTransaction& transaction,
                            std::string* error);

void abortDirectoryRestore(DirectoryRestoreTransaction& transaction);
}
