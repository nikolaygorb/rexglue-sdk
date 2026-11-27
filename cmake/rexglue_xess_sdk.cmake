# rexglue_xess_sdk.cmake - Intel XeSS SDK 3.0.2 (Super Resolution only) for the
# native temporal upscaler.
#
# Downloads from the v3.0.2 tag only what XeSS SR on Vulkan needs: libxess.dll
# (loaded at run time, ~78 MB: the cross-vendor and Intel models), the two SR
# headers, the license and the third-party notices (Intel Simplified Software
# License: unmodified binaries may be redistributed with them). Pinned commit
# and SHA-256 per file. Windows only: Intel ships no Linux runtime; elsewhere
# the provider reports itself unavailable and the renderer keeps its image.
#
# Result: REXGLUE_XESS_SDK_DIR (empty when unavailable), REXGLUE_XESS_SDK_RUNTIME_FILES.

option(REXGLUE_ENABLE_XESS_SDK "Download Intel XeSS SDK 3.0.2 (libxess.dll) for the native temporal upscaler (Windows)" ON)
set(REXGLUE_XESS_SDK_DIR "" CACHE INTERNAL "Intel XeSS SDK 3.0.2 root (inc/xess headers, bin/libxess.dll)")
set(REXGLUE_XESS_SDK_RUNTIME_FILES "" CACHE INTERNAL "Intel XeSS SDK files staged next to executables")
if(NOT REXGLUE_ENABLE_XESS_SDK OR NOT WIN32 OR NOT REXGLUE_USE_VULKAN)
    return()
endif()

set(_xess_commit 8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0)  # tag v3.0.2
set(_xess_url "https://raw.githubusercontent.com/intel/xess/${_xess_commit}")
set(_xess_dir "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/xess-sdk-3.0.2")
cmake_path(NORMAL_PATH _xess_dir)
set(_xess_files
    "inc/xess/xess.h" 98dc435f5aed252d83a161fc1f86223f20c67eb863214fe3b5cb215a3f6e3319
    "inc/xess/xess_vk.h" 859086df3c531ceccab628b287f69bd9a4528045bcbdd8c639589082dcbf8977
    "bin/libxess.dll" 251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7
    "LICENSE.txt" 784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df
    "third-party-programs.txt" e8d8899dbfc4de0e8ca25560be9358e03c1405d7c956d20a1838c3c709b9b51c
)

set(_xess_ok TRUE)
list(LENGTH _xess_files _xess_count)
math(EXPR _xess_last "${_xess_count} - 1")
foreach(_i RANGE 0 ${_xess_last} 2)
    math(EXPR _j "${_i} + 1")
    list(GET _xess_files ${_i} _file)
    list(GET _xess_files ${_j} _hash)
    set(_path "${_xess_dir}/${_file}")
    set(_have FALSE)
    if(EXISTS "${_path}")
        file(SHA256 "${_path}" _actual)
        if(_actual STREQUAL _hash)
            set(_have TRUE)
        endif()
    endif()
    if(NOT _have)
        message(STATUS "XeSS SDK 3.0.2: downloading ${_file}")
        file(DOWNLOAD "${_xess_url}/${_file}" "${_path}" EXPECTED_HASH SHA256=${_hash} TLS_VERIFY ON STATUS _status)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            list(GET _status 1 _message)
            message(WARNING "XeSS SDK 3.0.2: ${_file} unavailable (${_message}); XeSS is disabled")
            file(REMOVE "${_path}")
            set(_xess_ok FALSE)
            break()
        endif()
    endif()
endforeach()

if(_xess_ok)
    set(REXGLUE_XESS_SDK_DIR "${_xess_dir}" CACHE INTERNAL "Intel XeSS SDK 3.0.2 root (inc/xess headers, bin/libxess.dll)")
    set(REXGLUE_XESS_SDK_RUNTIME_FILES "${_xess_dir}/bin/libxess.dll"
        CACHE INTERNAL "Intel XeSS SDK files staged next to executables")
    message(STATUS "XeSS SDK 3.0.2: ${_xess_dir}")
endif()
unset(_xess_ok)
unset(_xess_files)
