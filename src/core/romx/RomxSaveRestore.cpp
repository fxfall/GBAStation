#include "RomxSaveRestore.hpp"

#include <cctype>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
void setError(std::string* output, const std::string& message)
{
    if (output != nullptr)
        *output = message;
}

std::string safeToken(const std::string& token)
{
    std::string result;
    result.reserve(token.size());
    for (const unsigned char character : token)
    {
        if (std::isalnum(character) != 0 || character == '-' || character == '_')
            result.push_back(static_cast<char>(character));
    }
    return result.empty() ? "transaction" : result;
}

bool existsWithoutError(const fs::path& path, std::error_code& error)
{
    const fs::file_status status = fs::symlink_status(path, error);
    if (error == std::make_error_code(std::errc::no_such_file_or_directory))
    {
        error.clear();
        return false;
    }
    // exists(file_status) treats both not_found and none as absent while
    // still considering a dangling symlink present because symlink_status()
    // reports its type without following the target.
    return !error && fs::exists(status);
}

void resetTransaction(beiklive::romx::DirectoryRestoreTransaction& transaction)
{
    transaction.finalDirectory.clear();
    transaction.stagingDirectory.clear();
    transaction.backupDirectory.clear();
    transaction.movedExisting = false;
    transaction.active = false;
}

beiklive::romx::DirectoryRestoreResult prepareDirectoryRestoreImpl(
    const fs::path& finalDirectory,
    const fs::path* suppliedStagingDirectory,
    bool overwrite,
    const std::string& token,
    beiklive::romx::DirectoryRestoreTransaction& transaction,
    std::string* error)
{
    using beiklive::romx::DirectoryRestoreResult;
    resetTransaction(transaction);
    if (finalDirectory.empty())
    {
        setError(error, "restore final directory is empty");
        return DirectoryRestoreResult::Failed;
    }

    const fs::path normalizedFinal = finalDirectory.lexically_normal();
    const fs::path parent = normalizedFinal.parent_path().empty()
        ? fs::path(".") : normalizedFinal.parent_path();
    std::error_code ec;
    if (!fs::create_directories(parent, ec) && ec)
    {
        setError(error, "cannot create restore parent directory: " + ec.message());
        return DirectoryRestoreResult::Failed;
    }

    const bool finalExists = existsWithoutError(normalizedFinal, ec);
    if (ec)
    {
        setError(error, "cannot inspect restore final directory: " + ec.message());
        return DirectoryRestoreResult::Failed;
    }
    if (finalExists)
    {
        if (!overwrite)
            return DirectoryRestoreResult::Skipped;
        if (fs::is_symlink(normalizedFinal, ec) || ec ||
            !fs::is_directory(normalizedFinal, ec) || ec)
        {
            setError(error, "restore final path is not a directory");
            return DirectoryRestoreResult::Failed;
        }
    }

    const std::string suffix = safeToken(token);
    const fs::path backup = parent / (".romx-backup-" + suffix);
    if (existsWithoutError(backup, ec) || ec)
    {
        setError(error, "restore backup path already exists");
        return DirectoryRestoreResult::Failed;
    }

    fs::path staging;
    if (suppliedStagingDirectory != nullptr)
    {
        staging = suppliedStagingDirectory->lexically_normal();
        if (staging.empty() || staging == normalizedFinal ||
            fs::is_symlink(staging, ec) || ec ||
            !fs::is_directory(staging, ec) || ec)
        {
            setError(error, "restore supplied staging path is not a directory");
            return DirectoryRestoreResult::Failed;
        }
    }
    else
    {
        staging = parent / (".romx-restore-" + suffix);
        if (existsWithoutError(staging, ec) || ec)
        {
            setError(error, "restore staging path already exists");
            return DirectoryRestoreResult::Failed;
        }
        if (!fs::create_directory(staging, ec) || ec)
        {
            setError(error, "cannot create restore staging directory: " + ec.message());
            return DirectoryRestoreResult::Failed;
        }
    }

    transaction.finalDirectory = normalizedFinal;
    transaction.stagingDirectory = staging;
    transaction.backupDirectory = backup;
    transaction.active = true;
    return DirectoryRestoreResult::Ready;
}
}

namespace beiklive::romx
{
DirectoryRestoreResult prepareDirectoryRestore(
    const fs::path& finalDirectory,
    bool overwrite,
    const std::string& token,
    DirectoryRestoreTransaction& transaction,
    std::string* error)
{
    return prepareDirectoryRestoreImpl(finalDirectory, nullptr, overwrite, token,
                                       transaction, error);
}

DirectoryRestoreResult prepareDirectoryRestoreWithStaging(
    const fs::path& finalDirectory,
    const fs::path& stagingDirectory,
    bool overwrite,
    const std::string& token,
    DirectoryRestoreTransaction& transaction,
    std::string* error)
{
    return prepareDirectoryRestoreImpl(finalDirectory, &stagingDirectory,
                                       overwrite, token, transaction, error);
}

bool commitDirectoryRestore(DirectoryRestoreTransaction& transaction,
                            std::string* error)
{
    if (!transaction.active)
    {
        setError(error, "restore transaction is not active");
        return false;
    }

    std::error_code ec;
    if (!fs::is_directory(transaction.stagingDirectory, ec) || ec)
    {
        setError(error, "restore staging directory is unavailable" +
            (ec ? ": " + ec.message() : std::string()));
        return false;
    }

    const bool finalExists = existsWithoutError(transaction.finalDirectory, ec);
    if (ec)
    {
        setError(error, "cannot inspect restore final directory: " + ec.message());
        return false;
    }
    if (finalExists)
    {
        if (existsWithoutError(transaction.backupDirectory, ec) || ec)
        {
            setError(error, "restore backup path already exists");
            return false;
        }
        fs::rename(transaction.finalDirectory, transaction.backupDirectory, ec);
        if (ec)
        {
            setError(error, "cannot move existing save directory to backup: " +
                                ec.message());
            return false;
        }
        transaction.movedExisting = true;
    }

    fs::rename(transaction.stagingDirectory, transaction.finalDirectory, ec);
    if (ec)
    {
        std::error_code restoreError;
        if (transaction.movedExisting)
            fs::rename(transaction.backupDirectory,
                       transaction.finalDirectory, restoreError);
        if (restoreError)
            setError(error, "cannot publish restore and cannot restore backup: " +
                                ec.message() + "; " + restoreError.message());
        else
            setError(error, "cannot publish restore directory: " + ec.message());
        transaction.movedExisting = false;
        transaction.active = false;
        std::error_code cleanupError;
        fs::remove_all(transaction.stagingDirectory, cleanupError);
        return false;
    }

    if (transaction.movedExisting)
    {
        fs::remove_all(transaction.backupDirectory, ec);
        if (ec)
        {
            setError(error, "cannot remove restore backup directory: " + ec.message());
            transaction.active = false;
            return false;
        }
    }
    transaction.movedExisting = false;
    transaction.active = false;
    return true;
}

void abortDirectoryRestore(DirectoryRestoreTransaction& transaction)
{
    if (!transaction.active)
        return;

    std::error_code ec;
    fs::remove_all(transaction.stagingDirectory, ec);
    if (transaction.movedExisting)
    {
        std::error_code finalError;
        const bool finalExists = fs::exists(transaction.finalDirectory, finalError);
        if (!finalError && !finalExists)
        {
            std::error_code restoreError;
            fs::rename(transaction.backupDirectory,
                       transaction.finalDirectory, restoreError);
        }
        transaction.movedExisting = false;
    }
    transaction.active = false;
}
}
