#include "core/romx/LibretroRomxSession.hpp"
#include "core/romx/RomxVfs.hpp"
#include <romx/romx.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using beiklive::romx::LibretroLaunchOptions;
using beiklive::romx::LibretroRomxSession;

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " #expr); } while (0)

static std::string read(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static fs::path makeContainer(const fs::path& root, const char* stem,
    const char* entrypoint, uint16_t format)
{
    const fs::path payload = root / entrypoint;
    std::ofstream(payload, std::ios::binary) << "PAYLOAD-ONLY";
    const fs::path output = root / (std::string(stem) + ".romx");
    const std::string source = payload.string();
    romx_writer_path_entry_t entry = ROMX_WRITER_PATH_ENTRY_INIT;
    entry.flags = ROMX_RIDX_ENTRYPOINT;
    entry.virtual_path = entrypoint;
    entry.source_path = source.c_str();
    entry.format_id = format;
    romx_writer_options_t options = ROMX_WRITER_OPTIONS_INIT;
    options.platform_id = ROMX_PLATFORM_PSP;
    options.launch_format_id = ROMX_LAUNCH_RAW_SINGLE_FILE;
    romx_writer_report_t report = ROMX_WRITER_REPORT_INIT;
    romx_error_t error{};
    CHECK(romx_writer_write_path_entries(output.string().c_str(), &entry, 1,
        nullptr, nullptr, &options, &report, &error) == ROMX_OK);
    return output;
}

int main(int argc, char** argv)
{
    fs::path root;
    try
    {
        CHECK(argc == 2);
        root = fs::path(argv[1]) / ("session-fixture-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        CHECK(fs::create_directory(root));
        const auto romx = makeContainer(root, "game", "game.iso", ROMX_FORMAT_ISO);
        const auto zip = makeContainer(root, "arcade", "arcade.ZiP", ROMX_FORMAT_ZIP);
        const auto cache = (root / "cache").string();
        std::string error;
        LibretroRomxSession session;
        LibretroLaunchOptions options;

        CHECK(session.prepare(romx.string(), options, cache, &error));
        CHECK(session.data() && session.size() == 12);
        CHECK(std::memcmp(session.data(), "PAYLOAD-ONLY", 12) == 0);
        CHECK(!fs::exists(cache));
        session.close();
        CHECK(!session.data() && session.path().empty());

        options.needFullpath = true;
        options.useVfs = true;
        CHECK(session.prepare(romx.string(), options, cache, &error));
        CHECK(session.usesVfs() && !session.data());
        auto* vfs = beiklive::romx_vfs::interfacePtr();
        auto* file = vfs->open(session.path().c_str(), RETRO_VFS_FILE_ACCESS_READ, 0);
        CHECK(file);
        char bytes[12]{};
        CHECK(vfs->read(file, bytes, sizeof(bytes)) == 12);
        CHECK(std::memcmp(bytes, "PAYLOAD-ONLY", 12) == 0);
        CHECK(vfs->close(file) == 0);

        // A second owner must fall back without disturbing the first binding.
        {
            LibretroRomxSession second;
            CHECK(second.prepare(romx.string(), options, cache, &error));
            CHECK(!second.usesVfs() && session.usesVfs());
            CHECK(read(second.path()) == "PAYLOAD-ONLY");
        }
        CHECK(session.materializeFallback(&error));
        CHECK(!session.usesVfs() && read(session.path()) == "PAYLOAD-ONLY");
        CHECK(session.prepare(romx.string(), options, cache, &error));
        CHECK(session.usesVfs());
        session.close();

        options.useVfs = false;
        CHECK(session.prepare(romx.string(), options, cache, &error));
        CHECK(!session.usesVfs() && !session.data());
        CHECK(read(session.path()) == "PAYLOAD-ONLY");

        options.preferZipPayload = true;
        CHECK(session.prepare(zip.string(), options, cache, &error));
        CHECK(session.path() == zip.string() && session.size() == 12);
        CHECK(std::memcmp(session.data(), "PAYLOAD-ONLY", 12) == 0);
        CHECK(!session.prepare(romx.string(), options, cache, &error));
        CHECK(!error.empty() && !session.data() && session.path().empty());
        CHECK(!session.prepare((root / "missing.romx").string(), options, cache, &error));
        CHECK(!session.data() && !session.usesVfs());
        fs::remove_all(root); // Only this test's freshly created fixtures.
        std::cout << "ROMX frontend mapping/VFS/fallback/ZIP/lifecycle tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << " (fixtures retained at " << root << ")\n";
        return 1;
    }
}
