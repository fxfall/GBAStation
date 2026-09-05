function(romx_test_target target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE ROMX::romx)
    target_compile_features(${target} PRIVATE c_std_99)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endfunction()

romx_test_target(romx_tests test_v2.c)
if(EXISTS "${ROMX_CONFORMANCE_FIXTURE_DIR}/minimal-single.romx")
    add_test(NAME romx.0.2.0 COMMAND romx_tests
        "${ROMX_CONFORMANCE_FIXTURE_DIR}")
endif()

romx_test_target(romx_writer_mutable_tests test_writer_mutable.c)
add_test(NAME romx.writer-mutable COMMAND romx_writer_mutable_tests
    "${CMAKE_CURRENT_BINARY_DIR}")

romx_test_target(romx_save_manager_tests test_save_manager.c)
add_test(NAME romx.save-manager COMMAND romx_save_manager_tests
    "${CMAKE_CURRENT_BINARY_DIR}")

romx_test_target(romx_probe_sample probe_sample.c)
