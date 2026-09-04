# Keep the macOS-only external libretro/Vulkan integration out of the main
# build script.  This leaves upstream-facing CMake changes at three call sites.

if (APPLE AND PLATFORM_DESKTOP)
    if (BUNDLE_MACOS_APP)
        set(_gbastation_bundle_external_cores_default ON)
    else ()
        set(_gbastation_bundle_external_cores_default OFF)
    endif ()

    option(GBASTATION_MACOS_BUNDLE_EXTERNAL_CORES
        "Copy the macOS 3DS, FBNeo and PSP cores into the app bundle"
        ${_gbastation_bundle_external_cores_default})

    set(GBASTATION_MACOS_EXTERNAL_CORES_DIR
        "$ENV{HOME}/Library/Application Support/GBAStation/GBAStation/cores"
        CACHE PATH
        "Directory containing the tested macOS external cores and PPSSPP assets")
    set(GBASTATION_MACOS_AZAHAR_CORE "" CACHE FILEPATH
        "Azahar libretro dylib to package in the macOS app")
    set(GBASTATION_MACOS_FBNEO_CORE "" CACHE FILEPATH
        "FBNeo libretro dylib to package in the macOS app")
    set(GBASTATION_MACOS_PPSSPP_CORE "" CACHE FILEPATH
        "PPSSPP libretro dylib to package in the macOS app")
    set(GBASTATION_MACOS_PPSSPP_ASSETS "" CACHE PATH
        "PPSSPP assets directory to package in the macOS app")
    set(GBASTATION_MACOS_FBNEO_PGM_BIOS "" CACHE FILEPATH
        "Optional FBNeo PGM BIOS/archive to package in the macOS app")

    # Prefer an explicitly supplied file, then the tested per-user runtime
    # directory, and finally the sibling core build outputs used by this
    # workspace.  The cache variables make CI and other checkouts portable.
    function(gbastation_resolve_macos_core variable_name core_file)
        if (DEFINED ${variable_name} AND NOT "${${variable_name}}" STREQUAL "")
            if (NOT EXISTS "${${variable_name}}")
                message(FATAL_ERROR
                    "${variable_name} points to a missing file: ${${variable_name}}")
            endif ()
            set(${variable_name} "${${variable_name}}" PARENT_SCOPE)
            return()
        endif ()

        set(_gbastation_candidates
            "${GBASTATION_MACOS_EXTERNAL_CORES_DIR}/${core_file}")
        if (core_file STREQUAL "azahar_libretro.dylib")
            list(APPEND _gbastation_candidates
                "${CMAKE_SOURCE_DIR}/../core/GBAStation_3DS/build_macos_libretro/bin/Release/${core_file}")
        elseif (core_file STREQUAL "fbneo_libretro.dylib")
            list(APPEND _gbastation_candidates
                "${CMAKE_SOURCE_DIR}/../core/GBAStation_FBNeo/src/burner/libretro/${core_file}")
        elseif (core_file STREQUAL "ppsspp_libretro.dylib")
            list(APPEND _gbastation_candidates
                "${CMAKE_SOURCE_DIR}/../core/GBAStation_ppsspp/build_macos_libretro_make/lib/${core_file}")
        endif ()

        foreach (_gbastation_candidate IN LISTS _gbastation_candidates)
            if (EXISTS "${_gbastation_candidate}")
                set(${variable_name} "${_gbastation_candidate}" CACHE FILEPATH
                    "macOS external core: ${core_file}" FORCE)
                set(${variable_name} "${_gbastation_candidate}" PARENT_SCOPE)
                return()
            endif ()
        endforeach ()

        set(${variable_name} "" PARENT_SCOPE)
    endfunction()

    gbastation_resolve_macos_core(
        GBASTATION_MACOS_AZAHAR_CORE azahar_libretro.dylib)
    gbastation_resolve_macos_core(
        GBASTATION_MACOS_FBNEO_CORE fbneo_libretro.dylib)
    gbastation_resolve_macos_core(
        GBASTATION_MACOS_PPSSPP_CORE ppsspp_libretro.dylib)

    if (NOT GBASTATION_MACOS_PPSSPP_ASSETS)
        set(_gbastation_ppsspp_asset_candidates
            "${GBASTATION_MACOS_EXTERNAL_CORES_DIR}/PPSSPP"
            "${CMAKE_SOURCE_DIR}/../core/GBAStation_ppsspp/build_macos_libretro_make/assets"
            "${CMAKE_SOURCE_DIR}/../core/GBAStation_ppsspp/assets")
        foreach (_gbastation_asset_candidate IN LISTS
                 _gbastation_ppsspp_asset_candidates)
            if (IS_DIRECTORY "${_gbastation_asset_candidate}")
                set(GBASTATION_MACOS_PPSSPP_ASSETS
                    "${_gbastation_asset_candidate}" CACHE PATH
                    "PPSSPP assets directory to package in the macOS app" FORCE)
                break()
            endif ()
        endforeach ()
    elseif (NOT IS_DIRECTORY "${GBASTATION_MACOS_PPSSPP_ASSETS}")
        message(FATAL_ERROR
            "GBASTATION_MACOS_PPSSPP_ASSETS is not a directory: "
            "${GBASTATION_MACOS_PPSSPP_ASSETS}")
    endif ()

    if (NOT GBASTATION_MACOS_FBNEO_PGM_BIOS)
        set(_gbastation_pgm_bios_candidate
            "${GBASTATION_MACOS_EXTERNAL_CORES_DIR}/pgm.zip")
        if (EXISTS "${_gbastation_pgm_bios_candidate}")
            set(GBASTATION_MACOS_FBNEO_PGM_BIOS
                "${_gbastation_pgm_bios_candidate}" CACHE FILEPATH
                "Optional FBNeo PGM BIOS/archive to package in the macOS app" FORCE)
        endif ()
    elseif (NOT EXISTS "${GBASTATION_MACOS_FBNEO_PGM_BIOS}")
        message(FATAL_ERROR
            "GBASTATION_MACOS_FBNEO_PGM_BIOS is not a file: "
            "${GBASTATION_MACOS_FBNEO_PGM_BIOS}")
    endif ()

    if (GBASTATION_MACOS_BUNDLE_EXTERNAL_CORES)
        foreach (_gbastation_required_core IN ITEMS
            GBASTATION_MACOS_AZAHAR_CORE
            GBASTATION_MACOS_FBNEO_CORE
            GBASTATION_MACOS_PPSSPP_CORE)
            if (NOT EXISTS "${${_gbastation_required_core}}")
                message(FATAL_ERROR
                    "Cannot bundle macOS external cores: ${_gbastation_required_core} "
                    "was not found. Set the corresponding CMake FILEPATH variable.")
            endif ()
        endforeach ()
        if (NOT IS_DIRECTORY "${GBASTATION_MACOS_PPSSPP_ASSETS}")
            message(FATAL_ERROR
                "Cannot bundle PPSSPP assets. Set GBASTATION_MACOS_PPSSPP_ASSETS "
                "to the PPSSPP assets directory.")
        endif ()
    endif ()
endif ()

function(gbastation_configure_external_core_dependencies)
    if (NOT (APPLE AND PLATFORM_DESKTOP))
        return()
    endif ()

    # Newer Apple Clang diagnoses the legacy Nestopia C++11 initializer tables
    # as hard narrowing errors.  Keep the compatibility exception target-local
    # instead of weakening warnings for the application and every other core.
    if (TARGET nestopia_core_obj AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(nestopia_core_obj PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-c++11-narrowing>)
    endif ()

    target_include_directories(genesis_core PRIVATE
        "${GBASTATION_KOSMICKRISP_SDK}/include"
        "${CMAKE_SOURCE_DIR}/third_party/RetroArch-1.22.2/libretro-common/include")
endfunction()

function(gbastation_configure_external_core_sources source_list)
    set(_gbastation_sources "${${source_list}}")
    if (APPLE AND PLATFORM_DESKTOP)
        set_source_files_properties(
            "${CMAKE_SOURCE_DIR}/src/game/retro/LibretroVulkanHost.cpp"
            PROPERTIES
                LANGUAGE OBJCXX
                COMPILE_DEFINITIONS VK_USE_PLATFORM_METAL_EXT)
    else ()
        list(FILTER _gbastation_sources EXCLUDE REGEX
            ".*/src/emulator/ExternalLibretroCore\\.cpp$")
        list(FILTER _gbastation_sources EXCLUDE REGEX
            ".*/src/game/retro/LibretroVulkanHost\\.cpp$")
        list(FILTER _gbastation_sources EXCLUDE REGEX
            ".*/src/platform/macos/GBAStationGLFWInput\\.cpp$")
    endif ()
    set(${source_list} "${_gbastation_sources}" PARENT_SCOPE)
endfunction()

function(gbastation_configure_external_core_target target)
    if (NOT (APPLE AND PLATFORM_DESKTOP))
        return()
    endif ()

    if (BUNDLE_MACOS_APP)
        set(_gbastation_runtime_dir
            "$<TARGET_FILE_DIR:${target}>/../Resources/vulkan")
        set(_gbastation_rpath "@loader_path/../Resources/vulkan/lib")
        set(_gbastation_icd_path
            "../Resources/vulkan/icd.d/libkosmickrisp_icd.json")
    else ()
        set(_gbastation_runtime_dir "$<TARGET_FILE_DIR:${target}>/vulkan")
        set(_gbastation_rpath "@loader_path/vulkan/lib")
        set(_gbastation_icd_path "vulkan/icd.d/libkosmickrisp_icd.json")
    endif ()

    target_compile_definitions(${target} PRIVATE
        GBASTATION_KOSMICKRISP_ICD_RELATIVE_PATH="${_gbastation_icd_path}")
    target_link_libraries(${target} PRIVATE
        gbastation_kosmic_vulkan_loader
        "-framework Metal"
        "-framework QuartzCore")
    set_target_properties(${target} PROPERTIES
        BUILD_RPATH "${_gbastation_rpath}"
        BUILD_WITH_INSTALL_RPATH TRUE
        SKIP_BUILD_RPATH TRUE
        INSTALL_RPATH "${_gbastation_rpath}")

    # Keep every bundle copy in one POST_BUILD rule.  Multiple POST_BUILD
    # rules are appended independently by some Makefile generators, and a
    # relink can then race the directory creation for the later rule.
    set(_gbastation_post_build_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_gbastation_runtime_dir}/lib"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_gbastation_runtime_dir}/icd.d"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GBASTATION_KOSMICKRISP_DRIVER}"
            "${_gbastation_runtime_dir}/lib/libvulkan_kosmickrisp.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GBASTATION_KOSMICKRISP_LOADER}"
            "${_gbastation_runtime_dir}/lib/libvulkan.1.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/libkosmickrisp_icd.json"
            "${_gbastation_runtime_dir}/icd.d/libkosmickrisp_icd.json")
    set(_gbastation_post_build_comment "Packaging KosmicKrisp Vulkan runtime")

    if (GBASTATION_MACOS_BUNDLE_EXTERNAL_CORES)
        if (BUNDLE_MACOS_APP)
            set(_gbastation_core_runtime_dir
                "$<TARGET_FILE_DIR:${target}>/../Resources/cores")
        else ()
            set(_gbastation_core_runtime_dir "$<TARGET_FILE_DIR:${target}>/cores")
        endif ()

        list(APPEND _gbastation_post_build_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${_gbastation_core_runtime_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GBASTATION_MACOS_AZAHAR_CORE}"
                "${_gbastation_core_runtime_dir}/azahar_libretro.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GBASTATION_MACOS_FBNEO_CORE}"
                "${_gbastation_core_runtime_dir}/fbneo_libretro.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GBASTATION_MACOS_PPSSPP_CORE}"
                "${_gbastation_core_runtime_dir}/ppsspp_libretro.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${GBASTATION_MACOS_PPSSPP_ASSETS}"
                "${_gbastation_core_runtime_dir}/PPSSPP")
        set(_gbastation_post_build_comment
            "Packaging macOS Vulkan runtime and external libretro cores")

        if (GBASTATION_MACOS_FBNEO_PGM_BIOS)
            list(APPEND _gbastation_post_build_commands
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${GBASTATION_MACOS_FBNEO_PGM_BIOS}"
                    "${_gbastation_core_runtime_dir}/pgm.zip")
        endif ()
    endif ()

    add_custom_command(TARGET ${target} POST_BUILD
        ${_gbastation_post_build_commands}
        COMMENT "${_gbastation_post_build_comment}")
endfunction()

if (APPLE AND PLATFORM_DESKTOP)
    enable_language(OBJC)
    enable_language(OBJCXX)

    set(_gbastation_default_vulkan_sdk "$ENV{VULKAN_SDK}")
    if (NOT _gbastation_default_vulkan_sdk)
        file(GLOB _gbastation_vulkan_sdk_candidates LIST_DIRECTORIES TRUE
            "${CMAKE_SOURCE_DIR}/../VulkanSDK/*/macOS"
            "$ENV{HOME}/VulkanSDK/*/macOS"
            "/Applications/VulkanSDK/*/macOS")
        list(REVERSE _gbastation_vulkan_sdk_candidates)
        foreach (_gbastation_vulkan_sdk_candidate IN LISTS
                 _gbastation_vulkan_sdk_candidates)
            if (EXISTS
                "${_gbastation_vulkan_sdk_candidate}/lib/libvulkan_kosmickrisp.dylib")
                set(_gbastation_default_vulkan_sdk
                    "${_gbastation_vulkan_sdk_candidate}")
                break()
            endif ()
        endforeach ()
    endif ()
    set(GBASTATION_KOSMICKRISP_SDK "${_gbastation_default_vulkan_sdk}"
        CACHE PATH "KosmicKrisp Vulkan SDK root (the directory containing include/ and lib/)")
    if (EXISTS "${GBASTATION_KOSMICKRISP_SDK}/macOS/include/vulkan/vulkan.h")
        set(GBASTATION_KOSMICKRISP_SDK
            "${GBASTATION_KOSMICKRISP_SDK}/macOS" CACHE PATH
            "KosmicKrisp Vulkan SDK root (the directory containing include/ and lib/)" FORCE)
    endif ()

    set(GBASTATION_KOSMICKRISP_LOADER
        "${GBASTATION_KOSMICKRISP_SDK}/lib/libvulkan.1.dylib")
    set(GBASTATION_KOSMICKRISP_DRIVER
        "${GBASTATION_KOSMICKRISP_SDK}/lib/libvulkan_kosmickrisp.dylib")
    foreach (_gbastation_required_file IN ITEMS
        "${GBASTATION_KOSMICKRISP_SDK}/include/vulkan/vulkan.h"
        "${GBASTATION_KOSMICKRISP_LOADER}"
        "${GBASTATION_KOSMICKRISP_DRIVER}")
        if (NOT EXISTS "${_gbastation_required_file}")
            message(FATAL_ERROR
                "KosmicKrisp SDK is incomplete: ${_gbastation_required_file}. "
                "Set GBASTATION_KOSMICKRISP_SDK to the SDK macOS directory.")
        endif ()
    endforeach ()

    add_library(gbastation_kosmic_vulkan_loader SHARED IMPORTED)
    set_target_properties(gbastation_kosmic_vulkan_loader PROPERTIES
        IMPORTED_LOCATION "${GBASTATION_KOSMICKRISP_LOADER}"
        INTERFACE_INCLUDE_DIRECTORIES
            "${GBASTATION_KOSMICKRISP_SDK}/include;${CMAKE_SOURCE_DIR}/third_party/RetroArch-1.22.2/libretro-common/include")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/libkosmickrisp_icd.json.in"
        "${CMAKE_BINARY_DIR}/libkosmickrisp_icd.json" @ONLY)
endif ()
