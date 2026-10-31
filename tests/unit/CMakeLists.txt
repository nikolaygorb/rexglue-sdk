# Unit Tests for ReXGlue core components

add_executable(unit_tests
    test_memory.cpp
    memory/heap_allocation_test.cpp
    memory/unwatched_invalidation_test.cpp
    memory/read_watch_test.cpp
    memory/range_access_test.cpp
    kernel/object_table_test.cpp
    kernel/bignum_rsa_test.cpp
    kernel/xecrypt_md5_test.cpp
    kernel/ob_name_test.cpp
    kernel/xnet_startup_params_test.cpp
    core/cvar_test.cpp
    core/fiber_test.cpp
    core/filesystem_test.cpp
    core/content_device_test.cpp
    core/vfs_overwrite_test.cpp
    core/vfs_io_queued_test.cpp
    core/vfs_rename_test.cpp
    core/vfs_mount_path_test.cpp
    core/vfs_open_status_test.cpp
    core/vfs_entry_update_test.cpp
    core/vfs_delete_on_close_test.cpp
    core/vfs_host_errors_test.cpp
    core/vfs_overlay_test.cpp
    core/vfs_package_thumbnail_test.cpp
    core/hash_test.cpp
    core/stream_test.cpp
    core/chrono_test.cpp
    core/chrono_steady_cast_test.cpp
    core/socket_test.cpp
    core/gpu_vendor_test.cpp
    core/utf8_case_test.cpp
    core/dynlib_attach_test.cpp
    core/bit_scan_test.cpp
    core/sleep_duration_test.cpp
    core/event_pulse_test.cpp
    core/logging_file_sink_test.cpp
    ppc/import_function_test.cpp
    ppc/stack_test.cpp
    ppc/fpscr_test.cpp
    ppc/fp_fma_test.cpp
    system/guest_path_test.cpp
    system/xsocket_accept_test.cpp
    system/achievement_manager_test.cpp
    system/function_dispatcher_test.cpp
    system/content_name_test.cpp
    system/xex_identity_test.cpp
    system/xex_load_test.cpp
    system/xdbf_test.cpp
    audio/conversion_test.cpp
    audio/nop_audio_driver_test.cpp
    audio/xma_packet_test.cpp
    audio/xma_work_test.cpp
    audio/audio_output_fallback_test.cpp
    input/input_assignment_test.cpp
    input/mnk_stick_test.cpp
    input/split_screen_test.cpp
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
    graphics/resolve_info_test.cpp
    graphics/primitive_reset_test.cpp
    graphics/edram_tile_count_test.cpp
    graphics/gamma_midpoint_test.cpp
    graphics/zpd_report_test.cpp
    graphics/gpu_completion_timeline_test.cpp
    graphics/float_constant_runs_test.cpp
    graphics/memexport_write_back_test.cpp
    graphics/texture_replacement_test.cpp
    graphics/scaled_resolve_coverage_test.cpp
    graphics/half_pixel_offset_test.cpp
    graphics/texture_swizzle_swap_test.cpp
    graphics/pipeline_wait_test.cpp
    graphics/render_target_trim_test.cpp
    graphics/memory_placement_test.cpp
    graphics/gpu_time_test.cpp
    graphics/shader_translation_memo_test.cpp
    graphics/display_transfer_test.cpp
    graphics/d3d11_shader_bytecode_test.cpp
    graphics/d3d11_geometry_shader_test.cpp
    ui/presenter_paint_config_test.cpp
    ui/keybind_names_test.cpp
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
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/replacement.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/primitive_processor.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/register_file.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/registers.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/shared_memory.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/util/draw.cpp
    # FloatToD3D11Fixed16p8 for draw.cpp (rexui, which unit_tests doesn't link).
    ${PROJECT_SOURCE_DIR}/src/ui/graphics_util.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/d3d11/geometry_shader.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/d3d11/shader_bytecode.cpp
    ${PROJECT_SOURCE_DIR}/thirdparty/dxbc/DXBCChecksum.cpp
)

target_include_directories(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/thirdparty/renderdoc
    ${PROJECT_SOURCE_DIR}  # For thirdparty/dxbc/DXBCChecksum.h
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(unit_tests PRIVATE
    rexcodegen
    rexruntime
    Catch2::Catch2WithMain
)
if(REXGLUE_USE_D3D11)
    # The D3D11 UI backend is in rexui only with the option on.
    target_sources(unit_tests PRIVATE ui/d3d11_presenter_test.cpp)
endif()
if(WIN32)
    # WARP checks of the D3D11 shaders; the render target shader copy uses
    # dxgiformat.h.
    target_link_libraries(unit_tests PRIVATE d3d11)
    target_sources(unit_tests PRIVATE
        graphics/d3d11_draw_path_test.cpp
        graphics/d3d11_render_target_shaders_test.cpp
        graphics/d3d11_resolution_scale_test.cpp
        graphics/d3d11_texture_upload_test.cpp
        graphics/d3d11_test_cvars.cpp
        graphics/dxbc_translator_test.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/d3d11/render_target_shaders.cpp
        # Number format and gamma helpers used by the copied generators.
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_alu.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_memexport.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_om.cpp
    )
endif()

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
# The D3D11 backend's generated copies of D3D12 code must match their
# originals (tools/d3d11/sync_copies.py).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME unit.d3d11_copies_in_sync
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/d3d11/sync_copies.py" --check)
    set_tests_properties(unit.d3d11_copies_in_sync PROPERTIES LABELS unit)
endif()

catch_discover_tests(unit_tests
    TEST_PREFIX "unit."
    PROPERTIES
        LABELS "unit"
        TIMEOUT 60
)
