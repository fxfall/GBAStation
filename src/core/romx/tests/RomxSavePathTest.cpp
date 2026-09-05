#include "../RomxSavePaths.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

int main()
{
    assert(beiklive::romx::expandedMutableBundleCapacity(1024U, false) == 1024U);
    assert(beiklive::romx::expandedMutableBundleCapacity(1024U, true) ==
           1024U * 1024U + 1024U);
    assert(beiklive::romx::expandedMutableBundleCapacity(2U * 1024U * 1024U,
                                                         true) ==
           4U * 1024U * 1024U);

    std::string targetDirectory;
    const auto mapper = beiklive::romx::makePspSaveOutputMapper(
        targetDirectory, std::string());

    // The returned callback must retain the selected source directory after
    // the factory has returned.  A reference capture here is a use-after-
    // return and becomes visible on the second invocation under ASAN.
    const std::optional<fs::path> first =
        mapper(fs::path("slot-a") / "PARAM.SFO", 0, 2);
    assert(first.has_value());
    assert(first.value() == fs::path("slot-a") / "PARAM.SFO");
    assert(targetDirectory == "slot-a");

    const std::optional<fs::path> sameDirectory =
        mapper(fs::path("slot-a") / "DATA.BIN", 1, 2);
    assert(sameDirectory.has_value());
    assert(sameDirectory.value() == fs::path("slot-a") / "DATA.BIN");

    const std::optional<fs::path> otherDirectory =
        mapper(fs::path("slot-b") / "PARAM.SFO", 0, 1);
    assert(!otherDirectory.has_value());

    targetDirectory.clear();
    const auto selectedMapper = beiklive::romx::makePspSaveOutputMapper(
        targetDirectory, "slot-c");
    const std::optional<fs::path> selected =
        selectedMapper(fs::path("slot-c") / "DATA.BIN", 0, 1);
    assert(selected.has_value());
    assert(selected.value() == fs::path("slot-c") / "DATA.BIN");

    const std::optional<fs::path> rejected =
        selectedMapper(fs::path("slot-d") / "DATA.BIN", 0, 1);
    assert(!rejected.has_value());

    constexpr const char* zeroId = "00000000000000000000000000000000";
    const fs::path switchRoot = fs::path("sdmc:/GBAStation/3ds/sdmc") /
        "Nintendo 3DS" / zeroId / zeroId;
    fs::path resolved;
    std::string error;
    const fs::path switchRelative =
        fs::path("extdata/00000000/000016e1/user/save.sav");
    assert(beiklive::romx::resolveSafeMutablePath(
        switchRoot, switchRelative, resolved, &error));
    assert(resolved == switchRoot / switchRelative);
    assert(resolved.string().rfind("sdmc:/GBAStation/3ds/", 0U) == 0U);

    const auto assertRejected = [&switchRoot](const fs::path& relative) {
        fs::path output;
        std::string pathError;
        assert(!beiklive::romx::resolveSafeMutablePath(
            switchRoot, relative, output, &pathError));
        assert(pathError.find("filesystem_code=") != std::string::npos);
        assert(pathError.find("filesystem_message=") != std::string::npos);
    };
    assertRejected(fs::path{});
    assertRejected(fs::path("/save.sav"));
    assertRejected(fs::path("../save.sav"));
    assertRejected(fs::path("a/../../save.sav"));
    assertRejected(fs::path("a/./save.sav"));
    assertRejected(fs::path("sdmc:/outside/save.sav"));
    assertRejected(fs::path("a\\b"));
    assertRejected(fs::path("a:switch/save.sav"));
    assertRejected(fs::path("a//save.sav"));
    assertRejected(fs::path(std::string("a\0b", 3U)));
    error.clear();
    assert(!beiklive::romx::resolveSafeMutablePath(
        fs::path{}, switchRelative, resolved, &error));
    assert(error.find("trusted root is empty") != std::string::npos);

    const fs::path temporaryRoot = fs::temp_directory_path() /
        ("gbastation-romx-save-path-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code filesystemError;
    fs::create_directories(temporaryRoot / "root", filesystemError);
    assert(!filesystemError);
    fs::create_directories(temporaryRoot / "outside", filesystemError);
    assert(!filesystemError);

    error.clear();
    assert(beiklive::romx::resolveSafeMutablePath(
        temporaryRoot / "root", fs::path("nested/save.sav"), resolved, &error));
    assert(resolved == temporaryRoot / "root/nested/save.sav");

    // Existing ordinary directories and missing future directories are both
    // valid; only existing symlink components are rejected.
    fs::create_directory(temporaryRoot / "root" / "nested", filesystemError);
    assert(!filesystemError);
    fs::create_directory_symlink(temporaryRoot / "outside",
                                 temporaryRoot / "root" / "escape",
                                 filesystemError);
    if (!filesystemError)
    {
        error.clear();
        assert(!beiklive::romx::resolveSafeMutablePath(
            temporaryRoot / "root", fs::path("escape/save.sav"), resolved,
            &error));
        assert(error.find("symbolic link") != std::string::npos);
    }
    fs::remove_all(temporaryRoot, filesystemError);
    return 0;
}
