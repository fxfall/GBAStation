#include "../RomxSavePaths.hpp"
#include "core/ThreeDsTitlePaths.hpp"

#include <romx/romx.h>

#include <cassert>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace
{
romx_mutable_save_layout_info_t titleLayout()
{
    romx_mutable_save_layout_info_t layout =
        ROMX_MUTABLE_SAVE_LAYOUT_INFO_INIT;
    layout.scope = ROMX_SAVE_SCOPE_3DS_TITLE;
    return layout;
}

romx_mutable_save_layout_info_t strictExtdataLayout()
{
    romx_mutable_save_layout_info_t layout =
        ROMX_MUTABLE_SAVE_LAYOUT_INFO_INIT;
    layout.scope = ROMX_SAVE_SCOPE_3DS_EXTDATA;
    layout.flags = ROMX_MUTABLE_SAVE_LAYOUT_HAS_EXTDATA_ID |
        ROMX_MUTABLE_SAVE_LAYOUT_STRICT_EXTDATA;
    layout.extdata_id_size = 16U;
    std::string("00000000000016E1").copy(layout.extdata_id, 16U);
    return layout;
}

romx_mutable_save_layout_info_t canonicalExtdataLayout()
{
    romx_mutable_save_layout_info_t layout =
        ROMX_MUTABLE_SAVE_LAYOUT_INFO_INIT;
    layout.scope = ROMX_SAVE_SCOPE_3DS_EXTDATA;
    layout.flags = ROMX_MUTABLE_SAVE_LAYOUT_HAS_EXTDATA_ID;
    layout.extdata_id_size = 16U;
    std::string("00000000000016E1").copy(layout.extdata_id, 16U);
    return layout;
}
}

int main()
{
    constexpr const char* zeroId = "00000000000000000000000000000000";
#ifdef __SWITCH__
    assert(beiklive::three_ds::sdRootPath() ==
           std::string("sdmc:/GBAStation/3ds/sdmc/Nintendo 3DS/") +
               zeroId + "/" + zeroId);
#else
    const std::string sdRoot = beiklive::three_ds::sdRootPath();
    assert(sdRoot.size() > 0U);
    assert(sdRoot.find("/Azahar/sdmc/Nintendo 3DS/") != std::string::npos);
    assert(sdRoot.rfind(std::string("/") + zeroId + "/" + zeroId) ==
           sdRoot.size() - 2U * std::string(zeroId).size() - 2U);
#endif

    const auto titleMapper = beiklive::romx::makeThreeDsSaveOutputMapper(
        "0100ABCD12345678", titleLayout());
    assert(beiklive::romx::threeDsSaveTransactionDirectory(
               "0100ABCD12345678", titleLayout()) ==
           fs::path("title/0100abcd/12345678/data/00000001"));
    const std::optional<fs::path> flat =
        titleMapper("saveData.bin", 0U, 1U);
    assert(flat.has_value());
    assert(flat.value() == fs::path("title/0100abcd/12345678/data/00000001/saveData.bin"));

    const std::optional<fs::path> prefixed =
        titleMapper(fs::path("00000001") / "system.dat", 0U, 2U);
    assert(prefixed.has_value());
    assert(prefixed.value() == fs::path("title/0100abcd/12345678/data/00000001/system.dat"));

    const std::optional<fs::path> nested =
        titleMapper(fs::path("backup") / "save00.bin", 0U, 2U);
    assert(nested.has_value());
    assert(nested.value() == fs::path("title/0100abcd/12345678/data/00000001/backup/save00.bin"));

    const auto strictMapper = beiklive::romx::makeThreeDsSaveOutputMapper(
        {}, strictExtdataLayout());
    assert(beiklive::romx::threeDsSaveTransactionDirectory(
               {}, strictExtdataLayout()) ==
           fs::path("extdata/00000000/000016e1/user"));
    const std::optional<fs::path> strictData =
        strictMapper(fs::path("000016e1") / "save.sav", 0U, 4U);
    assert(strictData.has_value());
    assert(strictData.value() == fs::path("extdata/00000000/000016e1/user/save.sav"));
    assert(!strictMapper("000016e1.dat", 1U, 4U).has_value());
    assert(!strictMapper("export.log", 2U, 4U).has_value());

    const auto canonicalMapper = beiklive::romx::makeThreeDsSaveOutputMapper(
        {}, canonicalExtdataLayout());
    assert(beiklive::romx::threeDsSaveTransactionDirectory(
               {}, canonicalExtdataLayout()) ==
           fs::path("extdata/00000000/000016e1/user"));
    const fs::path canonical =
        fs::path("extdata/00000000/000016e1/user/save.sav");
    assert(canonicalMapper(canonical, 0U, 1U) == canonical);
    return 0;
}
