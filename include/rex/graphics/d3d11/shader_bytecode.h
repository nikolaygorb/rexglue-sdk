/**
 * @file        graphics/d3d11/shader_bytecode.h
 * @brief       Shader Model 5.1 to 5.0 bytecode conversion for Direct3D 11
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *              Ported for the ReXGlue fork from TheSimpsonsGameRecomp
 *              (tools/rexglue-sdk/include/rex/graphics/d3d11/shader_bytecode.h,
 *              commit 66bb08c0, BSD 3-Clause).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rex::graphics::d3d11 {

// Values are the DXBC operand types of the bindings.
enum class ResourceKind : uint32_t {
  kSampler = 6,
  kShaderResource = 7,
  kConstantBuffer = 8,
  kUnorderedAccess = 30,
};

struct ResourceBinding {
  ResourceKind kind;
  uint32_t id;
  uint32_t source_register;
  uint32_t host_register;
};

// Guest and host-format channel mappings, composed by the texture cache.
// Four 3-bit selectors choose RGBA, zero or one (0 through 5 respectively).
struct TextureSwizzle {
  uint32_t srv_register;
  uint32_t swizzle;
  bool operator==(const TextureSwizzle&) const = default;
};
constexpr uint32_t kIdentityTextureSwizzle = 0 | (1 << 3) | (2 << 6) | (3 << 9);

struct ShaderBytecodeOptions {
  // Features of the owning device, as SFI0 bits. The default allows only the
  // shader features of feature level 11.0.
  uint64_t supported_feature_flags = 0;
  // 8 at feature level 11.0, 64 at 11.1.
  uint32_t uav_register_count = 8;
  // Graphics UAVs share the output merger slots with the render targets, so
  // they are placed above the four guest render targets.
  uint32_t graphics_uav_register_offset = 4;
  struct ReadOnlyUavAlias {
    uint32_t uav_register;
    uint32_t srv_register;
  };
  // The DXBC translator reads shared memory through either the SRV or the UAV,
  // chosen at run time (kSysFlag_SharedMemoryIsUAV), so every vertex fetch
  // declares the UAV, even without memexport. When the pipeline does not write
  // shared memory, the UAV reads are redirected to the SRV, and the UAV
  // declaration is dropped, which feature level 11.0 requires outside pixel
  // and compute shaders. An aliased UAV that is written or used atomically
  // fails the conversion rather than losing the write.
  std::vector<ReadOnlyUavAlias> read_only_uav_aliases;
  // Direct3D 11 views have no component mapping. Texture fetches from these
  // SRVs get the mapping in the shader: permutations change the fetch's
  // swizzle, constant zero and one add a masked MOV after it.
  std::vector<TextureSwizzle> texture_swizzles;
};

struct ShaderBytecode {
  std::vector<uint8_t> data;
  std::vector<ResourceBinding> bindings;
  std::vector<ResourceBinding> aliased_uav_reads;
  uint32_t program_type = 0;
  uint32_t texture_swizzle_moves = 0;
};

// Options of the backend for a device with these SFI0 feature bits and UAV
// slots: graphics UAVs after the four render targets. With
// alias_shared_memory_uav (translated guest shaders of a pipeline without
// memexport), the translator's shared memory UAV (u0) reads go to its SRV (t0);
// never for shaders whose u0 is something else, such as the EDRAM dump shaders.
ShaderBytecodeOptions GetBackendShaderBytecodeOptions(uint64_t supported_feature_flags,
                                                      uint32_t uav_register_count,
                                                      bool alias_shared_memory_uav);

// Converts a DXBC container with Shader Model 5.1 bindings (one resource per
// range, space 0, as the DXBC translator and the built-in shaders emit them
// without bindless) to Shader Model 5.0 registers, rebuilding the resource
// definitions, the feature flags and the checksum. Limits of Direct3D 11
// (16 samplers, 128 SRVs, 14 constant buffers, the UAV slot count), bindless
// ranges and features the device lacks fail with an explanation in `error`.
// Input and output may not alias. Failure clears the output.
bool ConvertShaderBytecode(std::span<const uint8_t> source, const ShaderBytecodeOptions& options,
                           ShaderBytecode& output, std::string& error);

}  // namespace rex::graphics::d3d11
