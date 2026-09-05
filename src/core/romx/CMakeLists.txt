# Application integration, separate from the portable libromx C API.
set(ROMX_BUILD_TESTS OFF CACHE BOOL "Build libromx tests inside GBAStation" FORCE)
set(ROMX_BUILD_EXAMPLES OFF CACHE BOOL "Build libromx examples inside GBAStation" FORCE)
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/libromx"
                 "${PROJECT_BINARY_DIR}/third_party/libromx")

if (PLATFORM_SWITCH)
    # libnx/newlib SDMC lacks these POSIX capabilities. The frontend serializes
    # explicit writes; the library still fsyncs the data file on every commit.
    target_compile_definitions(romx PRIVATE
        ROMX_NO_MMAP=1 ROMX_NO_PREAD=1 ROMX_NO_FCNTL_LOCK=1
        ROMX_NO_DIRECTORY_SYNC=1)
endif()

# Compile in the consumer to retain its existing SDK/UI/platform flags on NS,
# macOS and Windows. An explicit source list prevents accidental glob coupling.
add_library(gbastation_romx INTERFACE)
add_library(GBAStation::ROMX ALIAS gbastation_romx)
target_sources(gbastation_romx INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/RomxFrontend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/RomxGameEntryAdapter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/RomxVfs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/LibretroRomxSession.cpp")
target_link_libraries(gbastation_romx INTERFACE ROMX::romx)
