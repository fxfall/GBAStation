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
    std::string("12345678000016E1").copy(layout.extdata_id, 16U);
    return layout;
}

romx_mutable_save_layout_info_t canonicalExtdataLayout()
{
    romx_mutable_save_layout_info_t layout =
        ROMX_MUTABLE_SAVE_LAYOUT_INFO_INIT;
    layout.scope = ROMX_SAVE_SCOPE_3DS_EXTDATA;
    layout.flags = ROMX_MUTABLE_SAVE_LAYOUT_HAS_EXTDATA_ID;
    layout.extdata_id_size = 16U;
    std::string("12345678000016E1").copy(layout.extdata_id, 16U);
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
    assert(!titleMapper(fs::path("../outside.sav"), 0U, 1U).has_value());

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
           fs::path("extdata/12345678/000016e1/user"));
    const std::optional<fs::path> strictData =
        strictMapper(fs::path("000016e1") / "save.sav", 0U, 4U);
    assert(strictData.has_value());
    assert(strictData.value() == fs::path("extdata/12345678/000016e1/user/save.sav"));
    assert(!strictMapper("000016e1.dat", 1U, 4U).has_value());
    assert(!strictMapper("000016e1_.dat", 2U, 4U).has_value());
    assert(!strictMapper("export.log", 2U, 4U).has_value());

    std::string mappingError;
    const auto canonicalMapper = beiklive::romx::makeThreeDsSaveOutputMapper(
        {}, canonicalExtdataLayout(), &mappingError);
    assert(beiklive::romx::threeDsSaveTransactionDirectory(
               {}, canonicalExtdataLayout()) ==
           fs::path("extdata/12345678/000016e1/user"));
    const fs::path canonical =
        fs::path("extdata/12345678/000016e1/user/save.sav");
    assert(canonicalMapper(canonical, 0U, 1U) == canonical);
    assert(canonicalMapper(
               fs::path("EXTDATA/12345678/000016E1/USER/save.sav"), 0U, 1U) ==
           canonical);
    // Citra/Azahar also stores ExtData metadata beside `user`.  Those files
    // are intentionally excluded from the user-only restore transaction.
    mappingError.clear();
    assert(!canonicalMapper(
               fs::path("extdata/12345678/000016e1/icon"), 0U, 1U));
    assert(mappingError.empty());
    assert(!canonicalMapper(
               fs::path("extdata/12345678/000016e1/metadata"), 0U, 1U));
    assert(mappingError.empty());

    const auto mismatchMapper = beiklive::romx::makeThreeDsSaveOutputMapper(
        {}, canonicalExtdataLayout(), &mappingError);
    assert(!mismatchMapper(
                fs::path("extdata/12345678/000016e2/user/save.sav"), 0U, 1U));
    assert(mappingError.find("does not match layout extdata_id") !=
           std::string::npos);

    mappingError.clear();
    assert(!mismatchMapper(
                fs::path("extdata/12345678/000016e1/cache/save.sav"), 0U, 1U));
    assert(mappingError.find("user directory") != std::string::npos);

    romx_mutable_save_layout_info_t unknownLayout =
        ROMX_MUTABLE_SAVE_LAYOUT_INFO_INIT;
    assert(beiklive::romx::threeDsSaveTransactionDirectory(
               "0100ABCD12345678", unknownLayout).empty());
    mappingError.clear();
    const auto unknownMapper = beiklive::romx::makeThreeDsSaveOutputMapper(
        "0100ABCD12345678", unknownLayout, &mappingError);
    assert(!unknownMapper("save.sav", 0U, 1U));
    assert(mappingError.find("unknown 3DS SAVE layout") != std::string::npos);

    // The public helper is intentionally relative to the native SD root; the
    // adapter must compose it before performing absolute containment checks.
    const fs::path nativeRoot = fs::path("/native-sd");
    assert(nativeRoot / beiklive::romx::threeDsSaveTransactionDirectory(
               "0100ABCD12345678", titleLayout()) ==
           fs::path("/native-sd/title/0100abcd/12345678/data/00000001"));
    return 0;
}
