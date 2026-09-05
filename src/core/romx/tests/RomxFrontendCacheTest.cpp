#include "core/romx/RomxFrontend.hpp"

#include <romx/romx.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using beiklive::romx::LaunchSession;

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " #expr); } while (0)

static void writeFile(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output);
    output << value;
    CHECK(output.good());
}

static std::string readFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    CHECK(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static fs::path makeContainer(const fs::path& root, const std::string& stem,
    const std::string& mainValue, const std::string& auxiliaryValue,
    bool mutableRegion, bool immutableHash = true, bool entryCrc = true)
{
    const fs::path sourceRoot = root / (stem + "-sources");
    const fs::path mainSource = sourceRoot / "main.bin";
    const fs::path auxiliarySource = sourceRoot / "aux.bin";
    writeFile(mainSource, mainValue);
    writeFile(auxiliarySource, auxiliaryValue);
    const std::string mainSourceString = mainSource.string();
    const std::string auxiliarySourceString = auxiliarySource.string();

    romx_writer_path_entry_t entries[2] = {
        ROMX_WRITER_PATH_ENTRY_INIT, ROMX_WRITER_PATH_ENTRY_INIT};
    entries[0].flags = ROMX_RIDX_ENTRYPOINT |
        (entryCrc ? ROMX_RIDX_HAS_CRC32 : 0U);
    entries[0].virtual_path = "main.bin";
    entries[0].source_path = mainSourceString.c_str();
    entries[0].format_id = ROMX_FORMAT_BIN;
    entries[1].flags = entryCrc ? ROMX_RIDX_HAS_CRC32 : 0U;
    entries[1].virtual_path = "aux.bin";
    entries[1].source_path = auxiliarySourceString.c_str();
    entries[1].format_id = ROMX_FORMAT_BIN;

    romx_writer_options_t options = ROMX_WRITER_OPTIONS_INIT;
    options.flags = ROMX_WRITER_REPLACE_EXISTING |
        (immutableHash ? ROMX_WRITER_IMMUTABLE_SHA256 : 0U);
    options.platform_id = ROMX_PLATFORM_PSP;
    options.launch_format_id = ROMX_LAUNCH_DIRECTORY;
    if (mutableRegion)
        options.mutable_capacity = 32768;

    const fs::path output = root / (stem + ".romx");
    romx_writer_report_t report = ROMX_WRITER_REPORT_INIT;
    romx_error_t error{};
    CHECK(romx_writer_write_path_entries(output.string().c_str(), entries, 2,
        nullptr, nullptr, &options, &report, &error) == ROMX_OK);
    return output;
}

static void updateMutable(const fs::path& romx, const fs::path& root)
{
    const fs::path source = root / "save.dat";
    writeFile(source, "updated mutable data");
    const std::string sourceString = source.string();
    romx_mutable_bundle_path_entry_t entry = ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT;
    entry.relative_path = "save.dat";
    entry.source_path = sourceString.c_str();
    romx_mutable_bundle_options_t bundle = ROMX_MUTABLE_BUNDLE_OPTIONS_INIT;
    romx_mutable_write_options_t write = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
    romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
    romx_error_t error{};
    CHECK(romx_mutable_bundle_write_path_entries(romx.string().c_str(),
        ROMX_MUTABLE_NAMESPACE_SAVE, "slot", &entry, 1, &bundle, &write,
        &object, &error) == ROMX_OK);
}

int main(int argc, char** argv)
{
    fs::path root;
    try
    {
        CHECK(argc == 2);
        root = fs::path(argv[1]) / ("frontend-cache-fixture-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        CHECK(fs::create_directory(root));
        const fs::path cache = root / "cache";
        const fs::path romx = makeContainer(root, "game", "immutable main",
            "auxiliary v1", true);
        std::string error;
        LaunchSession first;
        CHECK(first.open(romx.string(), &error));
        CHECK(first.info().cacheable && !first.info().cacheIdentity.empty());
        std::string firstPath;
        CHECK(first.materializeEntrypoint(cache.string(), firstPath, &error));
        CHECK(readFile(fs::path(firstPath)) == "immutable main");
        const fs::path firstRoot = fs::path(firstPath).parent_path();
        CHECK(readFile(firstRoot / "aux.bin") == "auxiliary v1");

        // The entrypoint is unchanged, but another immutable entry changes.
        // The old adapter keyed only the entrypoint CRC and reused aux.bin.
        const fs::path rewritten = makeContainer(root, "game", "immutable main",
            "auxiliary v2", true);
        CHECK(rewritten == romx);
        LaunchSession second;
        CHECK(second.open(romx.string(), &error));
        std::string secondPath;
        CHECK(second.materializeEntrypoint(cache.string(), secondPath, &error));
        CHECK(secondPath != firstPath);
        CHECK(readFile(fs::path(secondPath)) == "immutable main");
        CHECK(readFile(fs::path(secondPath).parent_path() / "aux.bin") == "auxiliary v2");

        // A truncated cache entry must be regenerated instead of accepted by
        // existence alone.
        fs::resize_file(secondPath, 1);
        LaunchSession third;
        CHECK(third.open(romx.string(), &error));
        std::string repairedPath;
        CHECK(third.materializeEntrypoint(cache.string(), repairedPath, &error));
        CHECK(repairedPath == secondPath);
        CHECK(readFile(fs::path(repairedPath)) == "immutable main");

        // Mutable updates are outside the immutable identity and retain the
        // same launch cache path.
        updateMutable(romx, root);
        LaunchSession afterMutable;
        CHECK(afterMutable.open(romx.string(), &error));
        std::string afterMutablePath;
        CHECK(afterMutable.materializeEntrypoint(cache.string(), afterMutablePath, &error));
        CHECK(afterMutablePath == secondPath);

        // The cache identity is content-based, not host-path-based.
        const fs::path copied = root / "copied.romx";
        fs::copy_file(romx, copied, fs::copy_options::overwrite_existing);
        LaunchSession copySession;
        CHECK(copySession.open(copied.string(), &error));
        std::string copiedPath;
        CHECK(copySession.materializeEntrypoint(cache.string(), copiedPath, &error));
        CHECK(copiedPath == secondPath);

        const fs::path fallback = makeContainer(root, "fallback", "main",
            "aux", false, false, true);
        LaunchSession fallbackSession;
        CHECK(fallbackSession.open(fallback.string(), &error));
        CHECK(fallbackSession.info().cacheable);
        CHECK(fallbackSession.info().cacheIdentity.rfind("ridx|", 0) == 0);

        const fs::path uncacheable = makeContainer(root, "uncacheable", "main",
            "aux", false, false, false);
        LaunchSession uncacheableSession;
        CHECK(uncacheableSession.open(uncacheable.string(), &error));
        CHECK(!uncacheableSession.info().cacheable);
        std::string uncacheablePath1;
        CHECK(uncacheableSession.materializeEntrypoint(
            cache.string(), uncacheablePath1, &error));
        LaunchSession uncacheableSession2;
        CHECK(uncacheableSession2.open(uncacheable.string(), &error));
        std::string uncacheablePath2;
        CHECK(uncacheableSession2.materializeEntrypoint(
            cache.string(), uncacheablePath2, &error));
        CHECK(uncacheablePath1 != uncacheablePath2);

        fs::remove_all(root);
        std::cout << "ROMX content-identity cache/truncation tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << " (fixtures retained at " << root << ")\n";
        return 1;
    }
}
