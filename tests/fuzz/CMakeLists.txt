# libFuzzer targets. Configure with REXGLUE_BUILD_FUZZERS=ON (preset
# win-amd64-fuzz); see docs/sanitizers.md for running them. A target checks
# more than ASan does when its source says so (an invariant broken aborts).

function(rex_add_fuzzer name)
    add_executable(${name} ${ARGN})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer
        -fno-sanitize-coverage=indirect-calls
        -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
        -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    target_include_directories(${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/thirdparty/renderdoc
        ${PROJECT_SOURCE_DIR}  # For thirdparty/dxbc/DXBCChecksum.h
    )
    target_link_libraries(${name} PRIVATE rexruntime)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
    )
endfunction()

# XEX LZX decompression over libmspack. lzx.cpp and the static mspack are
# linked into the executable: on Windows libFuzzer only collects coverage from
# the executable module, so the copy in rexruntime.dll would fuzz blind.
rex_add_fuzzer(lzx_fuzzer lzx_fuzzer.cpp ${PROJECT_SOURCE_DIR}/src/system/lzx.cpp)
target_link_libraries(lzx_fuzzer PRIVATE mspack)

# Xenos microcode analysis, the DXBC translator and the Direct3D 11 bytecode
# conversion, compiled in directly like in unit_tests: rexgpu-xenos is a plugin
# nothing links.
if(WIN32)
    rex_add_fuzzer(shader_dxbc_fuzzer
        shader_dxbc_fuzzer.cpp
        # Plugin cvars the translator reads, defined with the GPU core.
        ${PROJECT_SOURCE_DIR}/tests/unit/graphics/d3d11_test_cvars.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/flags.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/format/ucode.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/shader.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/translator.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/translator_disasm.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_alu.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_memexport.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_om.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/d3d11/shader_bytecode.cpp
        ${PROJECT_SOURCE_DIR}/thirdparty/dxbc/DXBCChecksum.cpp
    )
endif()

# Xenos microcode analysis and the SPIR-V translator (Vulkan), on the same
# input layout as shader_dxbc_fuzzer.
if(REXGLUE_USE_VULKAN)
    rex_add_fuzzer(shader_spirv_fuzzer
        shader_spirv_fuzzer.cpp
        # Plugin cvars the translator reads, defined with the GPU core.
        ${PROJECT_SOURCE_DIR}/tests/unit/graphics/d3d11_test_cvars.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/flags.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/format/ucode.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/shader.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/translator.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/translator_disasm.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_builder.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_translator.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_translator_alu.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_translator_fetch.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_translator_memexport.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_translator_rb.cpp
    )
    target_link_libraries(shader_spirv_fuzzer PRIVATE Vulkan::Headers glslang::SPIRV)
endif()

# Texture address and layout arithmetic traps on integer overflow in the
# fuzzers: guest fetch constants and resolve registers reach it unchecked, and
# a wrapped size or address is a wrong range of guest memory rather than a
# crash. Trap mode needs no UBSan runtime; unmerged traps keep their own
# source line in the report (merged, they all point at one).
set(REX_FUZZ_TEXTURE_UTIL_SOURCES
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/util.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/info.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/info_formats.cpp
)
set_source_files_properties(${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/util.cpp
    PROPERTIES COMPILE_OPTIONS
    "-fsanitize=unsigned-integer-overflow,signed-integer-overflow;-fsanitize-trap=unsigned-integer-overflow,signed-integer-overflow;-fno-sanitize-merge=unsigned-integer-overflow,signed-integer-overflow"
)

# Guest texture layout from a texture fetch constant, and the texture
# replacement key computed over it (in rexruntime, compiled in for coverage).
rex_add_fuzzer(texture_layout_fuzzer
    texture_layout_fuzzer.cpp
    ${REX_FUZZ_TEXTURE_UTIL_SOURCES}
    ${PROJECT_SOURCE_DIR}/src/graphics/flags.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/replacement.cpp
)

# Resolve setup from the guest's registers (draw_util::GetResolveInfo).
rex_add_fuzzer(resolve_fuzzer
    resolve_fuzzer.cpp
    ${REX_FUZZ_TEXTURE_UTIL_SOURCES}
    ${PROJECT_SOURCE_DIR}/src/graphics/register_file.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/registers.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/util/draw.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/graphics_util.cpp
)

# Reading the shader and pipeline storage files (header-only, in storage.h).
rex_add_fuzzer(shader_storage_fuzzer shader_storage_fuzzer.cpp)
if(WIN32)
    # The fuzzer build links the CRT statically into every module, so a FILE*
    # opened by rexruntime.dll's OpenFile is invalid for the fread and fclose
    # storage.h calls in the executable (the CRT's invalid parameter handler
    # ends the process). The executable gets its own copy.
    target_sources(shader_storage_fuzzer PRIVATE ${PROJECT_SOURCE_DIR}/src/core/filesystem_win.cpp)
endif()

# Primitive types, index buffers and primitive reset before a draw
# (PrimitiveProcessor::Process), with a backend in host memory.
rex_add_fuzzer(primitive_processor_fuzzer
    primitive_processor_fuzzer.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/primitive_processor.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/shared_memory.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/register_file.cpp
)

# The XMA decoder context on the guest's context data and packets
# (XmaContext::Work), FFmpeg's XMA decoder included.
rex_add_fuzzer(xma_fuzzer xma_fuzzer.cpp ${PROJECT_SOURCE_DIR}/src/audio/xma_context.cpp)
target_link_libraries(xma_fuzzer PRIVATE libavcodec libavutil)

# The title's XDBF resource: title, achievements, their strings and icons.
rex_add_fuzzer(xdbf_fuzzer xdbf_fuzzer.cpp ${PROJECT_SOURCE_DIR}/src/system/util/xdbf_utils.cpp)

# Content packages and disc images, mounted from a file and read. The device
# sources are compiled with the headers rexfilesystem has in its PCH.
function(rex_add_filesystem_fuzzer name)
    rex_add_fuzzer(${name} ${ARGN}
        ${PROJECT_SOURCE_DIR}/src/filesystem/device.cpp
        ${PROJECT_SOURCE_DIR}/src/filesystem/entry.cpp
    )
    target_precompile_headers(${name} PRIVATE
        <cstring> <mutex> <rex/math.h> <rex/string.h> <rex/memory.h> <rex/filesystem.h>
        <rex/thread/mutex.h> <rex/system/xtypes.h> <rex/system/xmemory.h>
    )
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src/filesystem/devices)
endfunction()

# Loading a XEX: headers, image decompression, PE headers.
rex_add_fuzzer(xex_fuzzer xex_fuzzer.cpp
    ${PROJECT_SOURCE_DIR}/src/system/xex_module.cpp
    ${PROJECT_SOURCE_DIR}/src/system/lzx.cpp
)
target_include_directories(xex_fuzzer PRIVATE ${PROJECT_SOURCE_DIR}/thirdparty)
target_link_libraries(xex_fuzzer PRIVATE mspack)

rex_add_filesystem_fuzzer(disc_image_fuzzer disc_image_fuzzer.cpp
    ${PROJECT_SOURCE_DIR}/src/filesystem/devices/disc_image_device.cpp
    ${PROJECT_SOURCE_DIR}/src/filesystem/devices/disc_image_entry.cpp
    ${PROJECT_SOURCE_DIR}/src/filesystem/devices/disc_image_file.cpp
)

rex_add_filesystem_fuzzer(stfs_fuzzer stfs_fuzzer.cpp
    ${PROJECT_SOURCE_DIR}/src/filesystem/devices/stfs_container_device.cpp
    ${PROJECT_SOURCE_DIR}/src/filesystem/devices/stfs_container_entry.cpp
    ${PROJECT_SOURCE_DIR}/src/filesystem/devices/stfs_container_file.cpp
)
if(WIN32)
    # As for shader_storage_fuzzer: the FILE* must come from this module's CRT.
    target_sources(stfs_fuzzer PRIVATE ${PROJECT_SOURCE_DIR}/src/core/filesystem_win.cpp)
endif()
