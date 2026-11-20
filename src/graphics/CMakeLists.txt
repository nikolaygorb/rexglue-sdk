# rexgpu-xenos: Xenos GPU emulation plugin (ported from Xenia)
# Namespace: rex::graphics
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this
# target. The Xenos format layer is compiled into rexcore.
#
# The plugin is plugin_main.cpp over rexgpu-core, a static library with
# everything else: the graphics systems, the command processors (the PM4
# executor in the base CommandProcessor), the shader front end and
# translators, the caches and the backends. A title's own GPU plugin
# (rexglue_add_gpu_plugin in cmake/rexglue_helpers.cmake) links the same
# library, so a title renderer derives from the SDK's classes instead of
# copying them.

set(REXGPU_XENOS_SOURCES
    # Core GPU types
    flags.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    format/ucode.cpp
    sampler_info.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    null_command_processor.cpp
    trace_player.cpp
    trace_reader.cpp
    trace_writer.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    util/smaa_textures.cpp
    packet_disassembler.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/texture/replacement.cpp
    pipeline/shader/interpreter.cpp
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGPU_XENOS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/null_graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/device_memory.cpp
    vulkan/glsl_compile.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/query_pool.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/tessellation_shaders.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if(REXGLUE_USE_D3D12)
    # DXBC third-party dependency
    set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # DXBC shader translation (required for D3D12)
    set(REXGPU_XENOS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        pipeline/shader/dxbc_translator_om.cpp
    )

    # D3D12 GPU backend
    set(REXGPU_XENOS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/null_graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/pipeline_cache.cpp
        d3d12/query_pool.cpp
    )
endif()

# D3D11 backend (experimental; selected only explicitly, never by "any").
# Uses the DXBC sources of the D3D12 backend, so it requires D3D12.
if(REXGLUE_USE_D3D11)
    set(REXGPU_XENOS_D3D11_SOURCES
        d3d11/command_processor.cpp
        d3d11/command_processor_copies.cpp
        d3d11/geometry_shader.cpp
        d3d11/graphics_system.cpp
        d3d11/pipeline_cache.cpp
        d3d11/pipeline_cache_copies.cpp
        d3d11/primitive_processor.cpp
        d3d11/render_target_cache.cpp
        d3d11/render_target_shaders.cpp
        d3d11/shader_bytecode.cpp
        d3d11/shared_memory.cpp
        d3d11/texture_cache.cpp
        d3d11/texture_cache_copies.cpp
    )
endif()

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_SPIRV_SOURCES} ${REXGPU_XENOS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
endif()

if(REXGLUE_USE_D3D11)
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_D3D11_SOURCES})
endif()

# Static, and only in a source-tree build (REXSDK_DIR): it is not installed.
# Its objects are compiled with hidden visibility like the plugin's, and with
# position-independent code because they end up in shared libraries.
add_library(rexgpu-core STATIC ${REXGPU_XENOS_SOURCES})
add_library(rex::gpu-core ALIAS rexgpu-core)

set_target_properties(rexgpu-core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(rexgpu-core PRIVATE
    ${REXGLUE_ROOT}  # For thirdparty/dxbc/DXBCChecksum.h include
)

target_compile_options(rexgpu-core PRIVATE -Wno-unused-variable)

rex_target_pch(rexgpu-core
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/memory.h>
    <rex/thread/mutex.h>
    <rex/system/xmemory.h>
    <rex/graphics/xenos.h>
    <rex/graphics/registers.h>
    <rex/graphics/format/ucode.h>
    <rex/graphics/pipeline/shader/shader.h>
)

if(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES)
    set_source_files_properties(${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# Never link the OBJECT libraries (rexcore/rexui) here; rexruntime already
# contains those objects and relinking them would duplicate global state.
# PUBLIC: what a plugin built on the core includes or calls itself (the SDK
# headers through rexruntime, the graphics APIs); PRIVATE: libraries only the
# core's own code uses (passed on to the plugin's link line only).
target_link_libraries(rexgpu-core
    PUBLIC
        rexruntime
        # renderdoc_app.h (header only): rex/ui/vulkan/instance.h includes it,
        # so a plugin's own sources that include the Vulkan headers need it.
        renderdoc
    PRIVATE
        xxHash::xxhash
)

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexgpu-core
        PUBLIC
            Vulkan::Headers
            GPUOpen::VulkanMemoryAllocator
            # SPIRV/SpvBuilder.h: the SPIR-V translator's headers
            # (rex/graphics/pipeline/shader/spirv_builder.h) include it, so a
            # plugin's own sources that use the translator need it.
            glslang::SPIRV
        PRIVATE
            glslang
            MachineIndependent
            GenericCodeGen
            OSDependent
            OGLCompiler
    )
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexgpu-core PUBLIC
        d3d12
        dxgi
        dxguid
    )
endif()

if(REXGLUE_USE_D3D11)
    target_link_libraries(rexgpu-core PUBLIC d3d11)
endif()

add_library(rexgpu-xenos SHARED plugin_main.cpp)
add_library(rex::gpu-xenos ALIAS rexgpu-xenos)

set_target_properties(rexgpu-xenos PROPERTIES
    EXPORT_NAME gpu-xenos
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rexgpu-xenos DESCRIPTION "ReXGlue Xenos GPU emulation")

# The whole archive: a static library hands the linker only the objects
# something references, and objects reached only through static
# initialisers (cvar definitions, registrations) would be dropped. Linked
# whole, the plugin has exactly the objects it had as one SHARED target.
# Everything PRIVATE: the plugin has no consumers.
target_link_libraries(rexgpu-xenos PRIVATE
    rexruntime
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,rexgpu-core>"
)

# rexgpu-native: the native renderer (docs/gpu-plugins.md), Vulkan only, one
# plugin for every title, chosen with gpu_plugin = "native". Built on the whole
# of rexgpu-core like rexgpu-xenos; not installed (rexgpu-core is not).
if(REXGLUE_USE_VULKAN)
    set(REXGPU_NATIVE_SOURCES
        native/plugin_main.cpp
        native/graphics_system.cpp
        native/bindings.cpp
        native/command_processor.cpp
        native/composite.cpp
        native/draw_state.cpp
        native/edram_ownership.cpp
        native/edram_transfer.cpp
        native/edram_tiles.cpp
        native/memexport.cpp
        native/pipeline_storage.cpp
        native/pipelines.cpp
        native/present.cpp
        native/primitive_processor.cpp
        native/render_targets.cpp
        native/resolve.cpp
        native/resolve_memory.cpp
        native/resolve_pass.cpp
        native/resolve_placement.cpp
        native/resolve_readback.cpp
        native/resolve_rect.cpp
        native/shaders.cpp
        native/shared_memory.cpp
        native/submission.cpp
        native/system_constants.cpp
        native/texture_cache.cpp
    )
    add_library(rexgpu-native SHARED ${REXGPU_NATIVE_SOURCES})
    add_library(rex::gpu-native ALIAS rexgpu-native)
    set_target_properties(rexgpu-native PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    rexglue_add_version_resource(rexgpu-native DESCRIPTION "ReXGlue native renderer")
    target_link_libraries(rexgpu-native PRIVATE
        rexruntime
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,rexgpu-core>"
    )
endif()
