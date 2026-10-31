# ReXGlue SDK - Xbox 360 Recompilation Toolkit
cmake_minimum_required(VERSION 3.25)

if(APPLE)
    set(REXGLUE_LANGUAGES C CXX OBJC OBJCXX)
else()
    set(REXGLUE_LANGUAGES C CXX)
endif()

project(ReXGlue
    VERSION 0.10.0
    LANGUAGES ${REXGLUE_LANGUAGES}
    DESCRIPTION "Xbox 360 Recompilation SDK"
)
# Floor only: bump MAJOR.MINOR for API changes. Patch is derived from git tags
# in cmake/rex_version.cmake. See docs/RELEASING.md.

#==========================================================
# Build Options
#==========================================================
option(REXGLUE_BUILD_TESTS       "Build PPC instruction and unit tests" OFF)
option(REXGLUE_ENABLE_SANITIZERS "Enable UndefinedBehaviorSanitizer (same as REXGLUE_SANITIZERS=undefined)" OFF)
set(REXGLUE_SANITIZERS "" CACHE STRING "Comma-separated clang sanitizers: address, undefined")
option(REXGLUE_BUILD_FUZZERS     "Build libFuzzer targets (tests/fuzz); best with REXGLUE_SANITIZERS=address" OFF)
option(REXGLUE_ENABLE_FIDELITYFX "Enable optional AMD FidelityFX integration (experimental)" OFF)
option(REXGLUE_ENABLE_TRACY                "Enable Tracy profiler integration" ON)
option(REXGLUE_ENABLE_PERF_COUNTERS        "Enable lightweight perf counter registry and overlay" ON)
option(REXGLUE_BUILD_SHADERS              "Add the rexglue_shaders target that regenerates the built-in shader headers" OFF)
# Propagate profiling defines globally (compiled out in Release)
if(REXGLUE_ENABLE_TRACY)
    add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()
if(REXGLUE_ENABLE_PERF_COUNTERS)
    add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()

# Graphics backend selection
# D3D12: Windows only, ON by default.
# Vulkan: Cross-platform, ON by default on Linux, OFF by default on Windows.
if(WIN32)
    option(REXGLUE_USE_D3D12  "Enable D3D12 graphics backend" ON)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" OFF)
else()
    set(REXGLUE_USE_D3D12 OFF)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" ON)
endif()

if(NOT REXGLUE_USE_D3D12 AND NOT REXGLUE_USE_VULKAN)
    message(FATAL_ERROR "At least one graphics backend must be enabled (REXGLUE_USE_D3D12 or REXGLUE_USE_VULKAN)")
endif()

# D3D11: experimental, Windows only, OFF by default. A third backend of
# rexgpu-xenos, chosen only explicitly (backend "d3d11"), never by "any". It
# reuses the DXBC shader translator of the D3D12 backend, so it needs D3D12.
if(WIN32)
    option(REXGLUE_USE_D3D11 "Enable the experimental D3D11 graphics backend" OFF)
else()
    set(REXGLUE_USE_D3D11 OFF)
endif()
if(REXGLUE_USE_D3D11 AND NOT REXGLUE_USE_D3D12)
    message(FATAL_ERROR "REXGLUE_USE_D3D11 requires REXGLUE_USE_D3D12")
endif()

if (REXGLUE_ENABLE_FIDELITYFX AND
    NOT (REXGLUE_USE_VULKAN OR REXGLUE_USE_D3D12))
    message(FATAL_ERROR
        "REXGLUE_ENABLE_FIDELITYFX requires at least one graphics backend")
endif()

# Root of the SDK source tree
set(REXGLUE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# SDK install support
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_helpers.cmake)

if(UNIX AND NOT APPLE)
    # Keep installed binaries self-contained by resolving shared libs relative
    # to the install prefix (for example, bin -> ../lib).
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    list(APPEND CMAKE_INSTALL_RPATH
        "$ORIGIN"
        "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
elseif(APPLE)
    # Use loader-relative install names on macOS so installed binaries remain
    # self-contained within the install prefix.
    list(APPEND CMAKE_INSTALL_RPATH
        "@loader_path"
        "@loader_path/../${CMAKE_INSTALL_LIBDIR}"
    )
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
endif()

# Enforce a Clang toolchain (LLVM Clang or Apple's Clang)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
    message(FATAL_ERROR "ReXGlue requires the Clang compiler (LLVM Clang or AppleClang). Please set CMAKE_CXX_COMPILER to clang++")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "18.0")
        message(FATAL_ERROR "ReXGlue requires Clang 18 or newer (found ${CMAKE_CXX_COMPILER_VERSION})")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.0")
        message(FATAL_ERROR "ReXGlue requires AppleClang 16 or newer / Xcode 16+ for C++23 support (found ${CMAKE_CXX_COMPILER_VERSION})")
    endif()
endif()

# Enforce x64 architecture
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ReXGlue only supports 64‑bit architectures (x86_64 or ARM64)")
endif()

# Set C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DEBUG_POSTFIX d)
set(CMAKE_RELWITHDEBINFO_POSTFIX rd)

# CMAKE_<CONFIG>_POSTFIX seeds libraries only, so without this every config of an
# executable installs over the last one.
function(rex_config_postfix target)
    set_target_properties(${target} PROPERTIES
        DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}"
        RELWITHDEBINFO_POSTFIX "${CMAKE_RELWITHDEBINFO_POSTFIX}")
endfunction()

if(NOT WIN32)
    # Use appropriate code model based on architecture
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64" AND NOT APPLE)
        # Large code model on Linux x86_64 for linking with very large recompiled executables (35MB+).
        # macOS uses small: the ld64 linker on macOS 15+ rejects text relocations produced
        # by -mcmodel=large, and macOS maps binaries in the low address space so ±2GB is sufficient.
        add_compile_options(-mcmodel=large)
    else()
        add_compile_options(-mcmodel=small)
    endif()
    # Position-independent code for all targets
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Global compile options
add_compile_options(
    -fno-strict-aliasing
    -ffp-model=strict
    $<$<CONFIG:DEBUG>:-g>
    $<$<CONFIG:DEBUG>:-O0>
    $<$<CONFIG:RELEASE>:-O3>
    $<$<CONFIG:RELEASE>:-DNDEBUG>
    -fno-char8_t
)

if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_compile_options(-msse4.1)
endif()

# Platform detection (supports AMD64 and ARM64)
set(REX_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES REX_OSX_ARCH_COUNT)
    if(REX_OSX_ARCH_COUNT GREATER 1)
        message(FATAL_ERROR "ReXGlue macOS presets expect a single CMAKE_OSX_ARCHITECTURES value")
    endif()
    list(GET CMAKE_OSX_ARCHITECTURES 0 REX_TARGET_PROCESSOR)
endif()

if(WIN32)
    if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(REX_PLATFORM "win-amd64")
    elseif(REX_TARGET_PROCESSOR MATCHES "aarch64|ARM64|arm64")
        set(REX_PLATFORM "win-arm64")
    else()
        message(FATAL_ERROR "Unsupported Windows architecture: ${REX_TARGET_PROCESSOR}")
    endif()
    add_compile_definitions(REX_PLATFORM_WINDOWS=1)
elseif(UNIX AND NOT APPLE)
    if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(REX_PLATFORM "linux-amd64")
    elseif(REX_TARGET_PROCESSOR MATCHES "aarch64|ARM64|arm64")
        set(REX_PLATFORM "linux-arm64")
    else()
        set(REX_PLATFORM "linux-${REX_TARGET_PROCESSOR}")
    endif()
    add_compile_definitions(REX_PLATFORM_LINUX=1)
elseif(APPLE)
    if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(REX_PLATFORM "mac-amd64")
    elseif(REX_TARGET_PROCESSOR MATCHES "aarch64|ARM64|arm64")
        set(REX_PLATFORM "mac-arm64")
    else()
        set(REX_PLATFORM "mac-${REX_TARGET_PROCESSOR}")
    endif()
    add_compile_definitions(REX_PLATFORM_MAC=1)
else()
    message(FATAL_ERROR "Unsupported platform. ReXGlue supports Windows, Linux, and macOS only.")
endif()

# Build config as compile definition (multi-config generators don't know config at configure time)
add_compile_definitions(REXGLUE_BUILD_CONFIG="$<CONFIG>")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rex_pch.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rex_version.cmake)
rex_resolve_version(REXGLUE_FULL_VERSION
    FLOOR_MAJOR ${PROJECT_VERSION_MAJOR}
    FLOOR_MINOR ${PROJECT_VERSION_MINOR}
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# Strip the -id trailer (if any) for the numeric-only version.
string(REGEX REPLACE "-.*$" "" REXGLUE_NUMERIC_VERSION "${REXGLUE_FULL_VERSION}")

# Parse up to 4 numeric components for consumers that want integer access.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?" _ "${REXGLUE_NUMERIC_VERSION}")
set(REXGLUE_DERIVED_MAJOR "${CMAKE_MATCH_1}")
set(REXGLUE_DERIVED_MINOR "${CMAKE_MATCH_2}")
if(CMAKE_MATCH_4)
    set(REXGLUE_DERIVED_PATCH "${CMAKE_MATCH_4}")
else()
    set(REXGLUE_DERIVED_PATCH "0")
endif()
if(CMAKE_MATCH_6)
    set(REXGLUE_DERIVED_TWEAK "${CMAKE_MATCH_6}")
else()
    set(REXGLUE_DERIVED_TWEAK "0")
endif()

set(REXGLUE_FLOOR_VERSION
    "${REXGLUE_DERIVED_MAJOR}.${REXGLUE_DERIVED_MINOR}.${REXGLUE_DERIVED_PATCH}")

rex_version_channel(REXGLUE_VERSION_CHANNEL VERSION "${REXGLUE_FULL_VERSION}")

# Generate version header -- must run after REX_PLATFORM is set
# Timestamp is set at configure time -- intentionally not updated on every rebuild
string(TIMESTAMP REXGLUE_BUILD_TIMESTAMP "%Y%m%d_%H%M")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/rex/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/rex/version.h
    @ONLY
)

# Sanitizers for debugging memory corruption and undefined behavior.
# REXGLUE_ENABLE_SANITIZERS is the old switch and means "undefined".
if(REXGLUE_ENABLE_SANITIZERS AND NOT REXGLUE_SANITIZERS)
    set(REXGLUE_SANITIZERS "undefined")
endif()
set(REXGLUE_OUTPUT_SUFFIX "")
if(REXGLUE_SANITIZERS)
    string(REPLACE "," ";" _rex_sanitizer_list "${REXGLUE_SANITIZERS}")
    foreach(_rex_sanitizer IN LISTS _rex_sanitizer_list)
        if(_rex_sanitizer STREQUAL "address")
            string(APPEND REXGLUE_OUTPUT_SUFFIX "-asan")
        elseif(_rex_sanitizer STREQUAL "undefined")
            string(APPEND REXGLUE_OUTPUT_SUFFIX "-ubsan")
        else()
            message(FATAL_ERROR "REXGLUE_SANITIZERS: unsupported sanitizer '${_rex_sanitizer}' (use address, undefined)")
        endif()
    endforeach()
    message(STATUS "Enabling sanitizers: ${REXGLUE_SANITIZERS}")
    # Guest memory is mapped at the first free 1 << n base (see Memory::Initialize),
    # so it does not collide with the ASan shadow.
    add_compile_options(-fsanitize=${REXGLUE_SANITIZERS} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${REXGLUE_SANITIZERS})
    if(WIN32 AND "address" IN_LIST _rex_sanitizer_list)
        # The Windows ASan runtime is a DLL and does not support the debug CRT.
        # Fuzzer builds switch to the static CRT below.
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        add_link_options(-shared-libsan)
    endif()
endif()
if(REXGLUE_BUILD_FUZZERS)
    # Coverage for libFuzzer everywhere, third-party code included (libmspack is
    # the LZX decoder). The fuzzer executables add -fsanitize=fuzzer for main().
    # No indirect call coverage: its callback has no funclet bundle, so LLVM makes
    # an indirect call in a catch or cleanup funclet unreachable (int3 on Windows,
    # which ends the process with no report and no artifact).
    add_compile_options(-fsanitize=fuzzer-no-link -fno-sanitize-coverage=indirect-calls)
    if(WIN32)
        # clang_rt.fuzzer is built with the static CRT and without the MSVC STL
        # ASan container annotations, and lld-link rejects mixing either
        # (/failifmismatch). Each module then has its own CRT; with ASan the
        # static runtime thunk sends every module's heap to the ASan DLL, so
        # memory still crosses the rexruntime boundary safely.
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
        add_compile_definitions(_DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
    endif()
    string(APPEND REXGLUE_OUTPUT_SUFFIX "-fuzz")
endif()

# Output directories (multi-config appends /<Config>/ automatically). Sanitizer
# builds get their own directory so they never replace the normal binaries.
set(REXGLUE_OUTPUT_DIR "${REXGLUE_ROOT}/out/${REX_PLATFORM}${REXGLUE_OUTPUT_SUFFIX}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REXGLUE_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${REXGLUE_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${REXGLUE_OUTPUT_DIR}")

# The Windows ASan runtime DLL has to sit next to the executables.
if(WIN32 AND REXGLUE_SANITIZERS MATCHES "address")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=clang_rt.asan_dynamic-x86_64.dll
        OUTPUT_VARIABLE _rex_asan_dll
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT EXISTS "${_rex_asan_dll}")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
            OUTPUT_VARIABLE _rex_clang_resource_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(_rex_asan_dll "${_rex_clang_resource_dir}/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
    endif()
    if(NOT EXISTS "${_rex_asan_dll}")
        message(FATAL_ERROR "ASan runtime DLL not found: ${_rex_asan_dll}")
    endif()
    if(CMAKE_CONFIGURATION_TYPES)
        foreach(_rex_config IN LISTS CMAKE_CONFIGURATION_TYPES)
            file(COPY "${_rex_asan_dll}" DESTINATION "${REXGLUE_OUTPUT_DIR}/${_rex_config}")
        endforeach()
    else()
        file(COPY "${_rex_asan_dll}" DESTINATION "${REXGLUE_OUTPUT_DIR}")
    endif()
endif()

if(REXGLUE_BUILD_TESTS)
    list(APPEND CMAKE_MODULE_PATH "${REXGLUE_ROOT}/cmake")
    list(APPEND CMAKE_MODULE_PATH "${REXGLUE_ROOT}/thirdparty/catch2/extras")
endif()

# Third-party dependencies (vendored sources only)
add_subdirectory(thirdparty)

# Enable warnings after thirdparty. clang-cl reads -Wall as MSVC's /Wall
# (-Weverything), so it gets the clang flags through /clang:.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_compile_options(/clang:-Wall /clang:-Wextra)
else()
    add_compile_options(
        -Wall
        -Wextra
    )
endif()

include_directories(${REXGLUE_ROOT}/include)

add_subdirectory(src/core)
add_subdirectory(src/filesystem)
add_subdirectory(src/ui)
add_subdirectory(src/input)
add_subdirectory(src/audio)
add_subdirectory(src/graphics)
add_subdirectory(src/system)
add_subdirectory(src/kernel)
add_subdirectory(src/codegen)
add_subdirectory(src/rexglue)

#==========================================================
# Built-in shader regeneration (maintainers only)
#==========================================================
# The generated headers are checked in. With REXGLUE_BUILD_SHADERS=ON,
# `cmake --build <dir> --target rexglue_shaders` rebuilds them from the sources
# in src/graphics/shaders and src/ui/shaders (tools/shaders/build_shaders.py).
# SPIR-V tools come from the Vulkan SDK in VULKAN_SDK if set, otherwise they are
# built from the thirdparty submodules by tools/shaders/CMakeLists.txt.
if(REXGLUE_BUILD_SHADERS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(REXGLUE_CLANG_FORMAT NAMES clang-format)
    set(REXGLUE_SHADERS_ARGS)
    if(REXGLUE_CLANG_FORMAT)
        list(APPEND REXGLUE_SHADERS_ARGS --clang-format "${REXGLUE_CLANG_FORMAT}")
    endif()
    if(NOT DEFINED ENV{VULKAN_SDK})
        set(REXGLUE_SHADER_TOOLS_DIR "${CMAKE_BINARY_DIR}/shader-tools")
        set(REXGLUE_SHADER_TOOLS_CONFIGURE_ARGS
            -S "${PROJECT_SOURCE_DIR}/tools/shaders"
            -B "${REXGLUE_SHADER_TOOLS_DIR}"
            -G "${CMAKE_GENERATOR}"
            -DCMAKE_BUILD_TYPE=Release
            "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
        if(CMAKE_GENERATOR_PLATFORM)
            list(APPEND REXGLUE_SHADER_TOOLS_CONFIGURE_ARGS -A "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        add_custom_target(rexglue_shader_tools
            COMMAND "${CMAKE_COMMAND}" ${REXGLUE_SHADER_TOOLS_CONFIGURE_ARGS}
            COMMAND "${CMAKE_COMMAND}" --build "${REXGLUE_SHADER_TOOLS_DIR}" --config Release
            COMMENT "Building the SPIR-V shader tools from thirdparty..."
            USES_TERMINAL
            VERBATIM)
        list(APPEND REXGLUE_SHADERS_ARGS --tools-dir "${REXGLUE_SHADER_TOOLS_DIR}/bin")
    endif()
    add_custom_target(rexglue_shaders
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/shaders/build_shaders.py"
                ${REXGLUE_SHADERS_ARGS}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Regenerating the built-in shader headers..."
        USES_TERMINAL
        VERBATIM)
    if(TARGET rexglue_shader_tools)
        add_dependencies(rexglue_shaders rexglue_shader_tools)
    endif()
endif()

# Add tests subdirectory if tests are enabled
if(REXGLUE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(REXGLUE_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()

include(cmake/rexglue_link_audit.cmake)

# Print configuration summary
message(STATUS "===========================================")
message(STATUS "ReXGlue Configuration Summary:")
message(STATUS "  Version: ${REXGLUE_FULL_VERSION} (${REXGLUE_VERSION_CHANNEL})")
message(STATUS "  Platform: ${REX_PLATFORM}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Configurations: ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  Output Dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Graphics: D3D12=${REXGLUE_USE_D3D12} Vulkan=${REXGLUE_USE_VULKAN} D3D11=${REXGLUE_USE_D3D11}")
message(STATUS "  FidelityFX: ${REXGLUE_ENABLE_FIDELITYFX}")
message(STATUS "  Tracy: ${REXGLUE_ENABLE_TRACY}")
message(STATUS "  Perf Counters: ${REXGLUE_ENABLE_PERF_COUNTERS}")
message(STATUS "===========================================")

include(cmake/rexglue_export_targets.cmake)

# SDK install/export rules (skipped when consumed as a subdirectory)
if(PROJECT_IS_TOP_LEVEL)
    include(cmake/rexglue_devenv.cmake)
    include(cmake/rexglue_install.cmake)
else()
    # When consumed via add_subdirectory(), provide the same interface as find_package().
    # This sets REXGLUE_SHARE_DIR and makes rexglue_configure_target() available.
    set(REXGLUE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ui" PARENT_SCOPE)
    set(REXGLUE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_helpers.cmake)
    foreach(target IN LISTS REXGLUE_INSTALL_TARGETS REXGLUE_INSTALL_FIDELITYFX_TARGETS)
        get_target_property(export_name ${target} EXPORT_NAME)
        if(NOT export_name)
            set(export_name ${target})
        endif()
        if(NOT TARGET rex::${export_name})
            add_library(rex::${export_name} ALIAS ${target})
        endif()
    endforeach()
endif()
