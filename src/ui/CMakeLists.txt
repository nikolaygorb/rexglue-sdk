# rexui

# Core UI sources
set(REXUI_CORE_SOURCES
    displays.cpp
    graphics_upload_buffer_pool.cpp
    gpu_trace_replay.cpp
    graphics_util.cpp
    image_decode.cpp
    imgui_dialog.cpp
    imgui_drawer.cpp
    immediate_drawer.cpp
    keybinds.cpp
    menu_item.cpp
    presenter.cpp
    renderdoc_api.cpp
    sdl_virtual_key.cpp
    window.cpp
    window_sdl.cpp
    windowed_app.cpp
    windowed_app_context.cpp
    windowed_app_context_sdl.cpp
)

# Optional overlay dialogs
set(REXUI_OVERLAY_SOURCES
    overlay/achievement_icon_cache.cpp
    overlay/achievement_toast.cpp
    overlay/achievements_overlay.cpp
    overlay/debug_overlay.cpp
    overlay/console_overlay.cpp
    overlay/console_commands.cpp
    overlay/settings_overlay.cpp
)

# Vulkan UI backend
set(REXUI_VULKAN_SOURCES
    vulkan/linked_type_descriptor_set_allocator.cpp
    vulkan/nis_upscaler.cpp
    vulkan/vulkan_moltenvk.cpp
    vulkan/single_layout_descriptor_set_pool.cpp
    vulkan/spirv_tools_context.cpp
    vulkan/ui_samplers.cpp
    vulkan/vulkan_device.cpp
    vulkan/vulkan_dlss_frame_generation.cpp
    vulkan/vulkan_frame_generation.cpp
    vulkan/vulkan_gpu_completion_timeline.cpp
    vulkan/vulkan_immediate_drawer.cpp
    vulkan/vulkan_instance.cpp
    vulkan/vulkan_mem_alloc.cpp
    vulkan/vulkan_ngx.cpp
    vulkan/vulkan_presenter.cpp
    vulkan/vulkan_provider.cpp
    vulkan/vulkan_upload_buffer_pool.cpp
    vulkan/vulkan_util.cpp
    vulkan/vulkan_xess.cpp
)

# D3D12 UI backend
if(REXGLUE_USE_D3D12)
    set(REXUI_D3D12_SOURCES
        d3d12/d3d12_cpu_descriptor_pool.cpp
        d3d12/d3d12_descriptor_heap_pool.cpp
        d3d12/d3d12_gpu_completion_timeline.cpp
        d3d12/d3d12_immediate_drawer.cpp
        d3d12/d3d12_presenter.cpp
        d3d12/d3d12_provider.cpp
        d3d12/d3d12_upload_buffer_pool.cpp
        d3d12/d3d12_util.cpp
    )
endif()

# D3D11 UI backend (experimental)
if(REXGLUE_USE_D3D11)
    set(REXUI_D3D11_SOURCES
        d3d11/d3d11_immediate_drawer.cpp
        d3d11/d3d11_presenter.cpp
        d3d11/d3d11_provider.cpp
    )
endif()

# Platform-specific sources (windowing itself is SDL3, platform-independent)
if(WIN32)
    set(REXUI_PLATFORM_SOURCES
        surface_win.cpp
    )
elseif(APPLE)
    set(REXUI_PLATFORM_SOURCES
        surface_mac.cpp
    )
else()
    set(REXUI_PLATFORM_SOURCES
        surface_gnulinux.cpp
    )
endif()

# Combine sources
set(REXUI_SOURCES
    ${REXUI_CORE_SOURCES}
    ${REXUI_OVERLAY_SOURCES}
    ${REXUI_PLATFORM_SOURCES}
)

if(REXGLUE_USE_VULKAN)
    list(APPEND REXUI_SOURCES ${REXUI_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXUI_SOURCES ${REXUI_D3D12_SOURCES})
endif()

if(REXGLUE_USE_D3D11)
    list(APPEND REXUI_SOURCES ${REXUI_D3D11_SOURCES})
endif()

add_library(rexui OBJECT ${REXUI_SOURCES})
add_library(rex::ui ALIAS rexui)

rex_target_pch(rexui <rex/math.h>)

target_include_directories(rexui PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexui PRIVATE
    ${PROJECT_BINARY_DIR}/include  # Generated macOS Vulkan path data
    ${REXGLUE_ROOT}/src  # For ui/shaders/bytecode includes
    ${REXGLUE_ROOT}/thirdparty/stb  # stb_image.h for icon decoding
    ${REXGLUE_ROOT}/thirdparty/nis  # NIS_Config.h for the Vulkan presenter's NIS pass
)

target_link_libraries(rexui PUBLIC
    rexcore
    renderdoc
    imgui::imgui
    SDL3::SDL3
)
target_link_libraries(rexui PRIVATE utf8cpp)

if(REXGLUE_FIDELITYFX_SOURCE_DIR AND EXISTS "${REXGLUE_FIDELITYFX_SOURCE_DIR}/sdk/include")
    target_include_directories(rexui PRIVATE
        "${REXGLUE_FIDELITYFX_SOURCE_DIR}/sdk/include"
        "${REXGLUE_FIDELITYFX_SOURCE_DIR}/ffx-api/include"
    )
endif()

set(REXUI_HAS_FIDELITYFX_RUNTIME OFF)
if(TARGET amd_fidelityfx_vk)
    target_link_libraries(rexui PRIVATE amd_fidelityfx_vk)
    set(REXUI_HAS_FIDELITYFX_RUNTIME ON)
endif()
if(TARGET amd_fidelityfx_dx12)
    target_link_libraries(rexui PRIVATE amd_fidelityfx_dx12)
    set(REXUI_HAS_FIDELITYFX_RUNTIME ON)
endif()
if(REXUI_HAS_FIDELITYFX_RUNTIME)
    target_compile_definitions(rexui PRIVATE REX_HAS_FIDELITYFX_RUNTIME=1)
endif()

# AMD FSR frame generation (cmake/rexglue_fsr_sdk.cmake): ffx-api headers for
# vulkan/vulkan_frame_generation.cpp only; the signed DLLs load at run time.
if(REXGLUE_USE_VULKAN AND REXGLUE_FSR_SDK_DIR)
    set_source_files_properties(vulkan/vulkan_frame_generation.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${REXGLUE_FSR_SDK_DIR}/Kits/FidelityFX"
        COMPILE_DEFINITIONS REX_HAS_FSR_SDK=1
        SKIP_PRECOMPILE_HEADERS ON)
endif()

# Intel XeSS (cmake/rexglue_xess_sdk.cmake): SR headers for vulkan/vulkan_xess.cpp
# only; libxess.dll loads at run time.
if(REXGLUE_USE_VULKAN AND REXGLUE_XESS_SDK_DIR)
    set_source_files_properties(vulkan/vulkan_xess.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${REXGLUE_XESS_SDK_DIR}/inc"
        COMPILE_DEFINITIONS REX_HAS_XESS_SDK=1
        SKIP_PRECOMPILE_HEADERS ON)
endif()

# NVIDIA DLSS (cmake/rexglue_dlss_sdk.cmake): NGX headers for
# vulkan/vulkan_ngx.cpp only (the one NGX user); the NGX static library is
# linked into rexruntime (src/kernel/CMakeLists.txt), the feature DLLs load at
# run time.
if(REXGLUE_USE_VULKAN AND REXGLUE_DLSS_SDK_DIR)
    set_source_files_properties(vulkan/vulkan_ngx.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${REXGLUE_DLSS_SDK_DIR}/include"
        COMPILE_DEFINITIONS REX_HAS_DLSS_SDK=1
        SKIP_PRECOMPILE_HEADERS ON)
endif()

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexui PUBLIC
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        spirv-tools-headers
    )
endif()

# Platform-specific dependencies
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)
    # Required, not optional: vulkan/api.h pulls in <wayland-client.h> on every
    # Linux build.
    pkg_check_modules(WAYLAND REQUIRED wayland-client)

    target_include_directories(rexui PUBLIC
        ${X11_XCB_INCLUDE_DIRS}
        ${WAYLAND_INCLUDE_DIRS}
    )
    target_link_libraries(rexui PUBLIC
        ${X11_XCB_LIBRARIES}
        ${WAYLAND_LIBRARIES}
    )
elseif(APPLE)
    target_link_libraries(rexui PRIVATE "-framework CoreFoundation")
endif()

if(WIN32)
    target_link_libraries(rexui PUBLIC dxgi dxguid)
endif()

# D3D12 backend dependencies
if(REXGLUE_USE_D3D12)
    target_link_libraries(rexui PUBLIC dxc-headers)
endif()

