#include "../RomxSavePaths.hpp"

#include <cassert>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

int main()
{
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
    return 0;
}
