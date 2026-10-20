# Unit Tests for ReXGlue core components

add_executable(unit_tests
    test_memory.cpp
    memory/heap_allocation_test.cpp
    kernel/object_table_test.cpp
    kernel/bignum_rsa_test.cpp
    kernel/xecrypt_md5_test.cpp
    kernel/ob_name_test.cpp
    kernel/xnet_startup_params_test.cpp
    core/cvar_test.cpp
    core/fiber_test.cpp
    core/filesystem_test.cpp
    core/vfs_overwrite_test.cpp
    core/vfs_io_queued_test.cpp
    core/vfs_rename_test.cpp
    core/vfs_mount_path_test.cpp
    core/vfs_open_status_test.cpp
    core/vfs_entry_update_test.cpp
    core/vfs_package_thumbnail_test.cpp
    core/hash_test.cpp
    core/stream_test.cpp
    core/chrono_test.cpp
    core/chrono_steady_cast_test.cpp
    core/socket_test.cpp
    core/gpu_vendor_test.cpp
    core/utf8_case_test.cpp
    ppc/import_function_test.cpp
    ppc/stack_test.cpp
    ppc/fpscr_test.cpp
    system/guest_path_test.cpp
    system/xsocket_accept_test.cpp
    system/achievement_manager_test.cpp
    system/function_dispatcher_test.cpp
    audio/conversion_test.cpp
    audio/nop_audio_driver_test.cpp
    audio/xma_packet_test.cpp
    input/input_assignment_test.cpp
    codegen/analysis_test.cpp
    codegen/codegen_writer_test.cpp
    codegen/output_partition_test.cpp
    codegen/output_stamp_test.cpp
    graphics/alu_parse_test.cpp
    graphics/fetch_parse_test.cpp
    graphics/register_file_test.cpp
    graphics/register_reset_test.cpp
    graphics/float_precision_test.cpp
    graphics/texture_fetch_constant_test.cpp
    graphics/texture_address_test.cpp
    graphics/texture_layout_test.cpp
    graphics/pipeline_priority_test.cpp
    graphics/shader_storage_test.cpp
    graphics/resolve_clear_value_test.cpp
    graphics/gamma_midpoint_test.cpp
    graphics/zpd_report_test.cpp
    graphics/gpu_completion_timeline_test.cpp
    ui/presenter_paint_config_test.cpp
)

target_sources(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/ui.cpp
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/progress.cpp
)

# Plugin-side GPU sources under test. rexgpu-xenos is a runtime-loaded plugin that nothing links,
# so the CPU-only pieces are compiled into the test directly; their runtime dependencies come
# from rexruntime.
target_sources(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/graphics/flags.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/format/ucode.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/interpreter.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/shader.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/translator.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/translator_disasm.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/register_file.cpp
)

target_include_directories(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/thirdparty/renderdoc
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(unit_tests PRIVATE
    rexcodegen
    rexruntime
    Catch2::Catch2WithMain
)

# ReXApp is shipped as source for recomp projects, rather than compiled into
# rexui. Keep its SDK-consumer compilation path covered by the test build.
add_library(rexapp_compile_test OBJECT
    ${PROJECT_SOURCE_DIR}/src/ui/rex_app.cpp
)
target_link_libraries(rexapp_compile_test PRIVATE
    rexruntime
    rexui
    rexinput
    rexaudio
)
add_dependencies(unit_tests rexapp_compile_test)

# Strip Tracy/profiling instrumentation from test targets
target_compile_options(unit_tests PRIVATE
    -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
    -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
)

set_target_properties(unit_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
if(UNIX AND NOT APPLE)
    set_target_properties(unit_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()

# Discover tests
include(Catch)
catch_discover_tests(unit_tests
    TEST_PREFIX "unit."
    PROPERTIES
        LABELS "unit"
        TIMEOUT 60
)
