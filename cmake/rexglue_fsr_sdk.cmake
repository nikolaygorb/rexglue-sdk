# rexglue_fsr_sdk.cmake - AMD FSR SDK 2.4.0 for the native temporal upscaler and
# frame generation.
#
# Downloads only what the runtime-loaded providers need from the v2.4.0 tag:
# the signed Vulkan DLLs (amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll,
# amd_fidelityfx_framegeneration_vk.dll), the ffx-api headers and the license
# (docs/license.md: the DLLs and headers are MIT; ship the license text with them). Pinned commit and SHA-256 per file.
# Windows only: AMD ships no Linux binaries; elsewhere the provider reports
# itself unavailable and the renderer keeps its current image (fallback).
#
# Result: REXGLUE_FSR_SDK_DIR (empty when unavailable), REXGLUE_FSR_SDK_RUNTIME_FILES.

option(REXGLUE_ENABLE_FSR_SDK "Download AMD FSR SDK 2.4 signed Vulkan DLLs for the native temporal upscaler and frame generation (Windows)" ON)
set(REXGLUE_FSR_SDK_DIR "" CACHE INTERNAL "AMD FSR SDK 2.4 root (ffx-api headers, signed DLLs)")
set(REXGLUE_FSR_SDK_RUNTIME_FILES "" CACHE INTERNAL "AMD FSR SDK files staged next to executables")
if(NOT REXGLUE_ENABLE_FSR_SDK OR NOT WIN32 OR NOT REXGLUE_USE_VULKAN)
    return()
endif()

set(_fsr_commit 7539ae8b6568ad29ed78a753404351daf3b5cca7)  # tag v2.4.0
set(_fsr_url "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${_fsr_commit}")
set(_fsr_dir "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/fsr-sdk-2.4.0")
cmake_path(NORMAL_PATH _fsr_dir)
set(_fsr_files
    "Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll" 38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9
    "Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll" 4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7
    "Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll" d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4
    "Kits/FidelityFX/api/include/ffx_api.h" 4d9dcf243ef5efbf03340a02f16b51a22053243a22e8a9a0b3ce91c26982b4bf
    "Kits/FidelityFX/api/include/ffx_api_types.h" 53f07c113a9ba493f297656e4481b6539aed0afc9f3cc8da682aa477d270c46c
    "Kits/FidelityFX/api/include/vk/ffx_api_vk.h" 09ac1855bc4c2f711243d6982909385072ca3f8d30db44d1a82ee5bd637832ad
    "Kits/FidelityFX/upscalers/include/ffx_upscale.h" 13091e7fe5fd9c82b1fb7c044b9f08f664d9653f599d2ea500738709236fca4d
    "Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h" bc78a18aa95f5157b4699b0602700592b624788c5bc55d1762cb46a1b6557ca1
    "Kits/FidelityFX/framegeneration/include/ffx_framegeneration_api_types.h" 396596af3df63310674e8036abd2ad0fcd061e61334e7a2c76653a2be3c686b4
    "Kits/FidelityFX/framegeneration/include/vk/ffx_api_framegeneration_vk.h" 321d3138005757759d9ee05281d3dcd4cc2c3608bc1d524f9b1d0a6dadd49f61
    "Kits/FidelityFX/docs/license.md" 32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb
)

set(_fsr_ok TRUE)
list(LENGTH _fsr_files _fsr_count)
math(EXPR _fsr_last "${_fsr_count} - 1")
foreach(_i RANGE 0 ${_fsr_last} 2)
    math(EXPR _j "${_i} + 1")
    list(GET _fsr_files ${_i} _file)
    list(GET _fsr_files ${_j} _hash)
    set(_path "${_fsr_dir}/${_file}")
    set(_have FALSE)
    if(EXISTS "${_path}")
        file(SHA256 "${_path}" _actual)
        if(_actual STREQUAL _hash)
            set(_have TRUE)
        endif()
    endif()
    if(NOT _have)
        message(STATUS "FSR SDK 2.4.0: downloading ${_file}")
        file(DOWNLOAD "${_fsr_url}/${_file}" "${_path}" EXPECTED_HASH SHA256=${_hash} TLS_VERIFY ON STATUS _status)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            list(GET _status 1 _message)
            message(WARNING "FSR SDK 2.4.0: ${_file} unavailable (${_message}); the native temporal upscaler is disabled")
            file(REMOVE "${_path}")
            set(_fsr_ok FALSE)
            break()
        endif()
    endif()
endforeach()

if(_fsr_ok)
    set(REXGLUE_FSR_SDK_DIR "${_fsr_dir}" CACHE INTERNAL "AMD FSR SDK 2.4 root (ffx-api headers, signed DLLs)")
    set(REXGLUE_FSR_SDK_RUNTIME_FILES
        "${_fsr_dir}/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll"
        "${_fsr_dir}/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll"
        "${_fsr_dir}/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll"
        CACHE INTERNAL "AMD FSR SDK files staged next to executables")
    message(STATUS "FSR SDK 2.4.0: ${_fsr_dir}")
endif()
unset(_fsr_ok)
unset(_fsr_files)
