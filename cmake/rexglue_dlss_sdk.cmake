# rexglue_dlss_sdk.cmake - NVIDIA DLSS SDK 310.9.16 (NGX: Super Resolution /
# DLAA and Frame Generation) for the native temporal upscaler and the Vulkan
# presenter's frame generation.
#
# Downloads from the v310.9.16 tag only what Vulkan needs: the NGX headers,
# the NGX static library (/MD release and debug; linked into rexruntime only,
# one NGX per process), the release feature DLLs (nvngx_dlss.dll ~59 MB,
# nvngx_dlssg.dll ~7.5 MB, loaded by the driver's NGX from next to the
# executable) and the license (NVIDIA RTX SDKs License: unmodified release
# DLLs may be redistributed with the application). Pinned commit and SHA-256
# per file. Windows x64 only for now (the SDK has Linux .so files; not wired).
#
# Result: REXGLUE_DLSS_SDK_DIR (empty when unavailable), REXGLUE_DLSS_SDK_RUNTIME_FILES.

option(REXGLUE_ENABLE_DLSS_SDK "Download NVIDIA DLSS SDK 310.9.16 (NGX) for DLSS SR/DLAA and frame generation (Windows)" ON)
set(REXGLUE_DLSS_SDK_DIR "" CACHE INTERNAL "NVIDIA DLSS SDK 310.9.16 root (include, lib)")
set(REXGLUE_DLSS_SDK_RUNTIME_FILES "" CACHE INTERNAL "NVIDIA DLSS SDK files staged next to executables")
if(NOT REXGLUE_ENABLE_DLSS_SDK OR NOT WIN32 OR NOT REXGLUE_USE_VULKAN)
    return()
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
    return()
endif()
# The static library is /MD (release or debug CRT): not with the static CRT
# of fuzzer builds or the sanitizer runtime's CRT choice.
if(REXGLUE_BUILD_FUZZERS OR REXGLUE_SANITIZERS)
    message(STATUS "DLSS SDK 310.9.16: off with fuzzers/sanitizers (CRT)")
    return()
endif()

set(_dlss_commit 9981c6350dc58c0ce21f024fd65e31e851c31689)  # tag v310.9.16
set(_dlss_url "https://raw.githubusercontent.com/NVIDIA/DLSS/${_dlss_commit}")
set(_dlss_dir "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/dlss-sdk-310.9.16")
cmake_path(NORMAL_PATH _dlss_dir)
set(_dlss_files
    "include/nvsdk_ngx.h" dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848
    "include/nvsdk_ngx_defs.h" ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e
    "include/nvsdk_ngx_defs_dlssg.h" c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1
    "include/nvsdk_ngx_defs_vk.h" 0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031
    "include/nvsdk_ngx_helpers_dlssg_vk.h" b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316
    "include/nvsdk_ngx_helpers_vk.h" c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623
    "include/nvsdk_ngx_params.h" 943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc
    "include/nvsdk_ngx_params_dlssg.h" c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad
    "include/nvsdk_ngx_vk.h" 2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d
    "lib/Windows_x86_64/x64/nvsdk_ngx_d.lib" bc43df9ec032004fde681d3e3afd01cde239e03a0125b822e88674c84287f99c
    "lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib" 3f804d50b0323baa2c4976c4c94e2454c4a3f9c6c04e7d1f2302392d5add0009
    "lib/Windows_x86_64/rel/nvngx_dlss.dll" d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e
    "lib/Windows_x86_64/rel/nvngx_dlssg.dll" 8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7
    "LICENSE.txt" d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0
)

set(_dlss_ok TRUE)
list(LENGTH _dlss_files _dlss_count)
math(EXPR _dlss_last "${_dlss_count} - 1")
foreach(_i RANGE 0 ${_dlss_last} 2)
    math(EXPR _j "${_i} + 1")
    list(GET _dlss_files ${_i} _file)
    list(GET _dlss_files ${_j} _hash)
    set(_path "${_dlss_dir}/${_file}")
    set(_have FALSE)
    if(EXISTS "${_path}")
        file(SHA256 "${_path}" _actual)
        if(_actual STREQUAL _hash)
            set(_have TRUE)
        endif()
    endif()
    if(NOT _have)
        message(STATUS "DLSS SDK 310.9.16: downloading ${_file}")
        file(DOWNLOAD "${_dlss_url}/${_file}" "${_path}" EXPECTED_HASH SHA256=${_hash} TLS_VERIFY ON STATUS _status)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            list(GET _status 1 _message)
            message(WARNING "DLSS SDK 310.9.16: ${_file} unavailable (${_message}); DLSS is disabled")
            file(REMOVE "${_path}")
            set(_dlss_ok FALSE)
            break()
        endif()
    endif()
endforeach()

if(_dlss_ok)
    set(REXGLUE_DLSS_SDK_DIR "${_dlss_dir}" CACHE INTERNAL "NVIDIA DLSS SDK 310.9.16 root (include, lib)")
    set(REXGLUE_DLSS_SDK_RUNTIME_FILES
        "${_dlss_dir}/lib/Windows_x86_64/rel/nvngx_dlss.dll"
        "${_dlss_dir}/lib/Windows_x86_64/rel/nvngx_dlssg.dll"
        CACHE INTERNAL "NVIDIA DLSS SDK files staged next to executables")
    message(STATUS "DLSS SDK 310.9.16: ${_dlss_dir}")
endif()
unset(_dlss_ok)
unset(_dlss_files)
