# Standalone project building the SPIR-V tools used to regenerate the built-in
# shader headers (see build_shaders.py) from the thirdparty submodules. It is
# configured and built by the rexglue_shaders target when REXGLUE_BUILD_SHADERS
# is ON and no Vulkan SDK is set in VULKAN_SDK; it is not part of the SDK build.
cmake_minimum_required(VERSION 3.22)
project(rexglue_shader_tools LANGUAGES C CXX)

set(REXGLUE_THIRDPARTY_DIR "${CMAKE_CURRENT_LIST_DIR}/../../thirdparty")

# SPIRV-Tools: spirv-opt, spirv-dis, spirv-as.
set(SPIRV-Headers_SOURCE_DIR "${REXGLUE_THIRDPARTY_DIR}/spirv-headers")
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${REXGLUE_THIRDPARTY_DIR}/spirv-tools" spirv-tools)

# glslang: glslangValidator, spirv-remap.
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER ON CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
add_subdirectory("${REXGLUE_THIRDPARTY_DIR}/glslang" glslang)

# Put all the tools in one directory for build_shaders.py --tools-dir.
set(REXGLUE_SHADER_TOOLS glslangValidator spirv-remap spirv-opt spirv-dis spirv-as)
foreach(tool ${REXGLUE_SHADER_TOOLS})
  set_target_properties(${tool} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin")
endforeach()
add_custom_target(rexglue_shader_tools ALL DEPENDS ${REXGLUE_SHADER_TOOLS})
