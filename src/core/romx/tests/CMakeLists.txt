cmake_minimum_required(VERSION 3.16)
project(gbastation_romx_tests LANGUAGES C CXX)
enable_testing()

get_filename_component(GBASTATION_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(ROMX_SOURCE_DIR "${GBASTATION_SOURCE_DIR}/third_party/libromx" CACHE PATH "libromx sources under test")
set(ROMX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ROMX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROMX_SOURCE_DIR}" libromx)
set(BOREALIS_DIR "${GBASTATION_SOURCE_DIR}/third_party/borealis/library")
add_executable(romx_frontend_tests
    LibretroRomxSessionTest.cpp
    ../LibretroRomxSession.cpp ../RomxFrontend.cpp ../RomxVfs.cpp
    "${BOREALIS_DIR}/lib/core/geometry.cpp"
    "${BOREALIS_DIR}/lib/extern/nanovg/nanovg.c")
target_compile_features(romx_frontend_tests PRIVATE cxx_std_17)
target_compile_definitions(romx_frontend_tests PRIVATE
    __GLFW__ BOREALIS_USE_OPENGL BRLS_RESOURCES="./resources/")
target_include_directories(romx_frontend_tests PRIVATE
    "${GBASTATION_SOURCE_DIR}" "${GBASTATION_SOURCE_DIR}/src"
    "${BOREALIS_DIR}/include" "${BOREALIS_DIR}/include/borealis/extern"
    "${BOREALIS_DIR}/include/borealis/extern/nanovg"
    "${BOREALIS_DIR}/include/borealis/extern/tinyxml2"
    "${BOREALIS_DIR}/lib/extern/fmt/include"
    "${BOREALIS_DIR}/lib/extern/tweeny/include"
    "${BOREALIS_DIR}/lib/extern/yoga"
    "${BOREALIS_DIR}/lib/extern/glfw/include")
target_link_libraries(romx_frontend_tests PRIVATE ROMX::romx)
add_test(NAME romx.frontend-session COMMAND romx_frontend_tests "${CMAKE_CURRENT_BINARY_DIR}")
