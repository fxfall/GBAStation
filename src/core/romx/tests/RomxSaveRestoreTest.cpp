#include "../RomxSaveRestore.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
fs::path uniqueRoot()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
        ("gbastation-romx-restore-test-" + std::to_string(ticks));
}

void writeText(const fs::path& path, const char* text)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}
}

int main()
{
    const fs::path root = uniqueRoot();
    fs::create_directories(root);

    const fs::path finalDirectory = root / "game";
    const fs::path adjacentDirectory = root / "other-game";
    writeText(finalDirectory / "old.dat", "old");
    writeText(adjacentDirectory / "keep.dat", "keep");

    beiklive::romx::DirectoryRestoreTransaction transaction;
    std::string error;
    assert(beiklive::romx::prepareDirectoryRestore(
               finalDirectory, true, "success", transaction, &error) ==
           beiklive::romx::DirectoryRestoreResult::Ready);
    writeText(transaction.stagingDirectory / "new.dat", "new");
    assert(beiklive::romx::commitDirectoryRestore(transaction, &error));
    assert(fs::exists(finalDirectory / "new.dat"));
    assert(!fs::exists(finalDirectory / "old.dat"));
    assert(fs::exists(adjacentDirectory / "keep.dat"));
    assert(!fs::exists(transaction.stagingDirectory));
    assert(!fs::exists(transaction.backupDirectory));

    const fs::path adoptedDirectory = root / "adopted";
    writeText(adoptedDirectory / "old.dat", "old");
    const fs::path suppliedStaging = root / "psp-staging" / "SAVEDATA01";
    writeText(suppliedStaging / "new.dat", "new");
    assert(beiklive::romx::prepareDirectoryRestoreWithStaging(
               adoptedDirectory, suppliedStaging, true, "adopted", transaction,
               &error) == beiklive::romx::DirectoryRestoreResult::Ready);
    assert(beiklive::romx::commitDirectoryRestore(transaction, &error));
    assert(fs::exists(adoptedDirectory / "new.dat"));
    assert(!fs::exists(adoptedDirectory / "old.dat"));
    assert(!fs::exists(suppliedStaging));

    const fs::path skippedDirectory = root / "skip";
    writeText(skippedDirectory / "old.dat", "old");
    assert(beiklive::romx::prepareDirectoryRestore(
               skippedDirectory, false, "skip", transaction, &error) ==
           beiklive::romx::DirectoryRestoreResult::Skipped);
    assert(fs::exists(skippedDirectory / "old.dat"));

    const fs::path recoverDirectory = root / "recover";
    writeText(recoverDirectory / "old.dat", "old");
    assert(beiklive::romx::prepareDirectoryRestore(
               recoverDirectory, true, "commit-failure", transaction, &error) ==
           beiklive::romx::DirectoryRestoreResult::Ready);
    writeText(transaction.stagingDirectory / "new.dat", "new");
    // A pre-existing backup path makes the commit fail before the final
    // directory is moved. The old directory must remain intact.
    fs::create_directories(transaction.backupDirectory);
    assert(!beiklive::romx::commitDirectoryRestore(transaction, &error));
    assert(fs::exists(recoverDirectory / "old.dat"));
    assert(!fs::exists(recoverDirectory / "new.dat"));
    beiklive::romx::abortDirectoryRestore(transaction);
    fs::remove_all(transaction.backupDirectory);

    fs::remove_all(root);
    return 0;
}
