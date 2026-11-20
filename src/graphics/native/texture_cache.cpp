/**
 * @file        graphics/native/texture_cache.cpp
 * @brief       Native renderer: guest textures on the host
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Ported from the SDK's VulkanTextureCache (Xenia, Copyright 2022 Ben Vanik, BSD
 *              license): its host format table, image creation, views, loading through the
 *              SDK's texture load shaders, bindings and samplers, without its resolution
 *              scaling, direct loads, replacement textures and resolve copies.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/texture_cache.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <cstring>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/native/image_barrier.h>
#include <rex/graphics/native/shared_memory.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/ui_samplers.h>
#include <rex/ui/vulkan/util.h>

// Defined by the SDK's Vulkan texture cache and texture cache (in rexgpu-core).
REXCVAR_DECLARE(bool, non_seamless_cube_map);
REXCVAR_DECLARE(bool, gpu_3d_to_2d_texture);

REXCVAR_DEFINE_BOOL(native_hoist_texture_loads, true, "GPU",
                    "Native renderer, with native_hoist_uploads: a texture load (or layout "
                    "change) in the middle of a render pass goes before that render pass when "
                    "its earlier draws didn't sample the texture, so it stays open; off: each "
                    "such load ends the render pass")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(native_profile_transfers, false, "GPU",
                    "Native renderer: count resident texture reloads and EDRAM claim operations")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::native {

// The SDK's precompiled texture load shaders (unscaled ones).
namespace shaders {
#include "../shaders/vulkan_spirv/texture_load_128bpb_cs.h"
#include "../shaders/vulkan_spirv/texture_load_16bpb_cs.h"
#include "../shaders/vulkan_spirv/texture_load_32bpb_cs.h"
#include "../shaders/vulkan_spirv/texture_load_64bpb_cs.h"
#include "../shaders/vulkan_spirv/texture_load_8bpb_cs.h"
#include "../shaders/vulkan_spirv/texture_load_bgrg8_rgb8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_ctx1_cs.h"
#include "../shaders/vulkan_spirv/texture_load_depth_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_depth_unorm_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxn_rg8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt1_rgba8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt3_rgba8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt3a_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt3aas1111_argb4_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt5_rgba8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt5a_r8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_gbgr8_rgb8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r10g11b11_rgba16_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r11g11b10_rgba16_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r16_snorm_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r16_unorm_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r4g4b4a4_a4r4g4b4_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r4g4b4a4_rgba8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r5g6b5_b5g6r5_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r5g6b5_rgba8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_rg16_snorm_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_rg16_unorm_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_rgba16_snorm_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_rgba16_unorm_float_cs.h"
}  // namespace shaders

namespace util = ui::vulkan::util;

namespace {

// What is logged once.
enum : uint32_t {
  kLogSamplerCount = 1u << 0,
  kLog3DAs2D = 1u << 1,
  kLogFloatConvert = 1u << 2,
  kLogResolveIntoTextures = 1u << 3,
};

constexpr uint32_t kLoadDescriptorSetIndexDestination = 0;
constexpr uint32_t kLoadDescriptorSetIndexSource = 1;
// The persistent shared memory set, the scratch buffer set, and the
// per-load ones in flight.
constexpr uint32_t kLoadSetPoolSize = 1024;

// Everything before against everything after (milestone N7 narrows it).
void FullMemoryBarrier(const ui::vulkan::VulkanDevice::Functions& dfn,
                       CommandRecorder* command_buffer) {
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  command_buffer->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0,
                                       nullptr, 0, nullptr);
}

// A host format's texel block (BC 4x4, 4:2:2 2x1, else 1x1) and its bytes, and
// one block of opaque magenta in it (native_poison_new_images): one channel
// 1, two channels 1 and 0, BC endpoints both magenta with every index 0.
// False for a format not in the texture cache's host formats.
bool GetHostBlock(VkFormat format, uint32_t& block_width, uint32_t& block_height,
                  uint32_t& block_bytes, uint8_t (&magenta)[16]) {
  std::memset(magenta, 0, sizeof(magenta));
  block_width = 1;
  block_height = 1;
  auto put = [&](std::initializer_list<uint8_t> bytes) {
    uint32_t i = 0;
    for (uint8_t byte : bytes) {
      magenta[i++] = byte;
    }
    block_bytes = i;
  };
  // Little-endian: half 1.0 0x3C00, float 1.0 0x3F800000, 5:6:5 magenta
  // 0xF81F.
  switch (format) {
    case VK_FORMAT_R8_UNORM:
      put({0xFF});
      return true;
    case VK_FORMAT_R8_SNORM:
      put({0x7F});
      return true;
    case VK_FORMAT_R8G8_UNORM:
      put({0xFF, 0x00});
      return true;
    case VK_FORMAT_R8G8_SNORM:
      put({0x7F, 0x00});
      return true;
    case VK_FORMAT_R8G8B8A8_UNORM:
      put({0xFF, 0x00, 0xFF, 0xFF});
      return true;
    case VK_FORMAT_R8G8B8A8_SNORM:
      put({0x7F, 0x00, 0x7F, 0x7F});
      return true;
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
      put({0x1F, 0xF8});
      return true;
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
      // B 15:12, G 11:8, R 7:4, A 3:0.
      put({0xFF, 0xF0});
      return true;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
      put({0x1F, 0xFC});
      return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      put({0xFF, 0x03, 0xF0, 0xFF});
      return true;
    case VK_FORMAT_R16_UNORM:
      put({0xFF, 0xFF});
      return true;
    case VK_FORMAT_R16_SNORM:
      put({0xFF, 0x7F});
      return true;
    case VK_FORMAT_R16_SFLOAT:
      put({0x00, 0x3C});
      return true;
    case VK_FORMAT_R16G16_UNORM:
      put({0xFF, 0xFF, 0x00, 0x00});
      return true;
    case VK_FORMAT_R16G16_SNORM:
      put({0xFF, 0x7F, 0x00, 0x00});
      return true;
    case VK_FORMAT_R16G16_SFLOAT:
      put({0x00, 0x3C, 0x00, 0x00});
      return true;
    case VK_FORMAT_R16G16B16A16_UNORM:
      put({0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF});
      return true;
    case VK_FORMAT_R16G16B16A16_SNORM:
      put({0xFF, 0x7F, 0x00, 0x00, 0xFF, 0x7F, 0xFF, 0x7F});
      return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      put({0x00, 0x3C, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x3C});
      return true;
    case VK_FORMAT_R32_SFLOAT:
      put({0x00, 0x00, 0x80, 0x3F});
      return true;
    case VK_FORMAT_R32G32_SFLOAT:
      put({0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00});
      return true;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      put({0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00,
           0x80, 0x3F});
      return true;
    case VK_FORMAT_G8B8G8R8_422_UNORM:
      block_width = 2;
      put({0x00, 0xFF, 0x00, 0xFF});
      return true;
    case VK_FORMAT_B8G8R8G8_422_UNORM:
      block_width = 2;
      put({0xFF, 0x00, 0xFF, 0x00});
      return true;
    default:
      break;
  }
  block_width = 4;
  block_height = 4;
  switch (format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      put({0x1F, 0xF8, 0x1F, 0xF8, 0, 0, 0, 0});
      return true;
    case VK_FORMAT_BC2_UNORM_BLOCK:
      // Explicit alpha all 15, then the colour block.
      put({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0xF8, 0x1F, 0xF8, 0, 0, 0, 0});
      return true;
    case VK_FORMAT_BC3_UNORM_BLOCK:
      // Alpha endpoints 255, then the colour block.
      put({0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0x1F, 0xF8, 0x1F, 0xF8, 0, 0, 0, 0});
      return true;
    case VK_FORMAT_BC4_UNORM_BLOCK:
      put({0xFF, 0xFF, 0, 0, 0, 0, 0, 0});
      return true;
    case VK_FORMAT_BC5_UNORM_BLOCK:
      put({0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0, 0, 0, 0, 0, 0});
      return true;
    default:
      return false;
  }
}

}  // namespace

// The SDK's host formats (VulkanTextureCache::kBestHostFormats and the
// unaligned fallbacks), copied as they are.
// The fields left out are zero (VK_FORMAT_UNDEFINED, false).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static_assert(VK_FORMAT_UNDEFINED == VkFormat(0),
              "Assuming that skipping a VkFormat in an initializer results in "
              "VK_FORMAT_UNDEFINED");
const NativeTextureCache::HostFormatPair NativeTextureCache::kBestHostFormats[64] = {
    // k_1_REVERSE
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8
    {{kLoadShaderIndex8bpb, VK_FORMAT_R8_UNORM},
     {kLoadShaderIndex8bpb, VK_FORMAT_R8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_1_5_5_5
    // Red and blue swapped in the load shader for simplicity.
    {{kLoadShaderIndexR5G5B5A1ToB5G5R5A1, VK_FORMAT_A1R5G5B5_UNORM_PACK16},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_5_6_5
    // Red and blue swapped in the load shader for simplicity.
    {{kLoadShaderIndexR5G6B5ToB5G6R5, VK_FORMAT_R5G6B5_UNORM_PACK16},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_6_5_5
    // On the host, green bits in blue, blue bits in green.
    {{kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, VK_FORMAT_R5G6B5_UNORM_PACK16},
     {kLoadShaderIndexUnknown},
     XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G)},
    // k_8_8_8_8
    {{kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_UNORM},
     {kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_2_10_10_10
    // Signed 2_10_10_10 is unsupported on D3D12, keep Vulkan behavior aligned.
    {{kLoadShaderIndex32bpb, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_A
    {{kLoadShaderIndex8bpb, VK_FORMAT_R8_UNORM},
     {kLoadShaderIndex8bpb, VK_FORMAT_R8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_8_B
    {{kLoadShaderIndex8bpb, VK_FORMAT_R8_UNORM},
     {kLoadShaderIndex8bpb, VK_FORMAT_R8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_8_8
    {{kLoadShaderIndex16bpb, VK_FORMAT_R8G8_UNORM},
     {kLoadShaderIndex16bpb, VK_FORMAT_R8G8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_Cr_Y1_Cb_Y0_REP
    // VK_FORMAT_G8B8G8R8_422_UNORM (added in
    // VK_KHR_sampler_ycbcr_conversion and promoted to Vulkan 1.1) is
    // optional.
    // Signed variant is unsupported on D3D12.
    {{kLoadShaderIndex32bpb, VK_FORMAT_G8B8G8R8_422_UNORM, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_Y1_Cr_Y0_Cb_REP
    // VK_FORMAT_B8G8R8G8_422_UNORM (added in
    // VK_KHR_sampler_ycbcr_conversion and promoted to Vulkan 1.1) is
    // optional.
    // Signed variant is unsupported on D3D12.
    {{kLoadShaderIndex32bpb, VK_FORMAT_B8G8R8G8_422_UNORM, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_A
    {{kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_UNORM},
     {kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_4_4_4_4
    // Components swapped in the load shader for simplicity.
    {{kLoadShaderIndexRGBA4ToARGB4, VK_FORMAT_B4G4R4A4_UNORM_PACK16},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11
    // Runtime fallback to float16 may be enabled if normalized RGBA16 host
    // formats are not linearly filterable.
    {{kLoadShaderIndexR11G11B10ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
     {kLoadShaderIndexR11G11B10ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10
    // Runtime fallback to float16 may be enabled if normalized RGBA16 host
    // formats are not linearly filterable.
    {{kLoadShaderIndexR10G11B11ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
     {kLoadShaderIndexR10G11B11ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT1
    // VK_FORMAT_BC1_RGBA_UNORM_BLOCK is optional.
    {{kLoadShaderIndex64bpb, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3
    // VK_FORMAT_BC2_UNORM_BLOCK is optional.
    {{kLoadShaderIndex128bpb, VK_FORMAT_BC2_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5
    // VK_FORMAT_BC3_UNORM_BLOCK is optional.
    {{kLoadShaderIndex128bpb, VK_FORMAT_BC3_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_16_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_24_8
    // Keep signed and unsigned sampling on the same R32 float descriptor path
    // for parity with D3D12.
    {{kLoadShaderIndexDepthUnorm, VK_FORMAT_R32_SFLOAT},
     {kLoadShaderIndexDepthUnorm, VK_FORMAT_R32_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_24_8_FLOAT
    // Keep signed and unsigned sampling on the same R32 float descriptor path
    // for parity with D3D12.
    {{kLoadShaderIndexDepthFloat, VK_FORMAT_R32_SFLOAT},
     {kLoadShaderIndexDepthFloat, VK_FORMAT_R32_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16
    // VK_FORMAT_R16_UNORM and VK_FORMAT_R16_SNORM are optional.
    {{kLoadShaderIndex16bpb, VK_FORMAT_R16_UNORM},
     {kLoadShaderIndex16bpb, VK_FORMAT_R16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16_16
    // VK_FORMAT_R16G16_UNORM and VK_FORMAT_R16G16_SNORM are optional.
    {{kLoadShaderIndex32bpb, VK_FORMAT_R16G16_UNORM},
     {kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_16_16_16_16
    // VK_FORMAT_R16G16B16A16_UNORM and VK_FORMAT_R16G16B16A16_SNORM are
    // optional.
    {{kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_UNORM},
     {kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_16_EXPAND
    {{kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
     {kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16_16_EXPAND
    {{kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
     {kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_16_16_16_16_EXPAND
    {{kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
     {kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_16_FLOAT
    {{kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
     {kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16_16_FLOAT
    {{kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
     {kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_16_16_16_16_FLOAT
    {{kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
     {kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_32
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_FLOAT
    {{kLoadShaderIndex32bpb, VK_FORMAT_R32_SFLOAT},
     {kLoadShaderIndex32bpb, VK_FORMAT_R32_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_32_32_FLOAT
    {{kLoadShaderIndex64bpb, VK_FORMAT_R32G32_SFLOAT},
     {kLoadShaderIndex64bpb, VK_FORMAT_R32G32_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_32_32_32_32_FLOAT
    {{kLoadShaderIndex128bpb, VK_FORMAT_R32G32B32A32_SFLOAT},
     {kLoadShaderIndex128bpb, VK_FORMAT_R32G32B32A32_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_32_AS_8
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_MPEG
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_MPEG_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXN
    // VK_FORMAT_BC5_UNORM_BLOCK is optional.
    {{kLoadShaderIndex128bpb, VK_FORMAT_BC5_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_AS_16_16_16_16
    {{kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_UNORM},
     {kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_DXT1_AS_16_16_16_16
    // VK_FORMAT_BC1_RGBA_UNORM_BLOCK is optional.
    {{kLoadShaderIndex64bpb, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3_AS_16_16_16_16
    // VK_FORMAT_BC2_UNORM_BLOCK is optional.
    {{kLoadShaderIndex128bpb, VK_FORMAT_BC2_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5_AS_16_16_16_16
    // VK_FORMAT_BC3_UNORM_BLOCK is optional.
    {{kLoadShaderIndex128bpb, VK_FORMAT_BC3_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_AS_16_16_16_16
    // Signed 2_10_10_10 is unsupported on D3D12, keep Vulkan behavior aligned.
    {{kLoadShaderIndex32bpb, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11_AS_16_16_16_16
    // Runtime fallback to float16 may be enabled if normalized RGBA16 host
    // formats are not linearly filterable.
    {{kLoadShaderIndexR11G11B10ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
     {kLoadShaderIndexR11G11B10ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10_AS_16_16_16_16
    // Runtime fallback to float16 may be enabled if normalized RGBA16 host
    // formats are not linearly filterable.
    {{kLoadShaderIndexR10G11B11ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
     {kLoadShaderIndexR10G11B11ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_32_32_32_FLOAT
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT3A
    {{kLoadShaderIndexDXT3A, VK_FORMAT_R8_UNORM},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_DXT5A
    // VK_FORMAT_BC4_UNORM_BLOCK is optional.
    {{kLoadShaderIndex64bpb, VK_FORMAT_BC4_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_CTX1
    {{kLoadShaderIndexCTX1, VK_FORMAT_R8G8_UNORM},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXT3A_AS_1_1_1_1
    {{kLoadShaderIndexDXT3AAs1111ToARGB4, VK_FORMAT_B4G4R4A4_UNORM_PACK16},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_8_8_8_GAMMA_EDRAM
    // Not usable as a texture.
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_FLOAT_EDRAM
    // Not usable as a texture.
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
};

// Vulkan requires 2x1 (4:2:2) subsampled images to have an even width.
// Always decompressing them to RGBA8 UNORM for D3D12 parity.

const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatGBGRUnaligned = {
    {kLoadShaderIndexGBGR8ToRGB8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB,
    false};

const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatBGRGUnaligned = {
    {kLoadShaderIndexBGRG8ToRGB8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB,
    false};

// Keep BC unaligned fallback mapping compatible with the D3D12 texture cache
// decompression mapping.
const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatDXT1Unaligned = {
    {kLoadShaderIndexDXT1ToRGBA8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
    false};

const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatDXT2_3Unaligned = {
    {kLoadShaderIndexDXT3ToRGBA8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
    false};

const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatDXT4_5Unaligned = {
    {kLoadShaderIndexDXT5ToRGBA8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
    false};

const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatDXNUnaligned = {
    {kLoadShaderIndexDXNToRG8, VK_FORMAT_R8G8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
    false};

const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatDXT5AUnaligned = {
    {kLoadShaderIndexDXT5AToR8, VK_FORMAT_R8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
    false};
#pragma GCC diagnostic pop

NativeTextureCache::NativeTexture::NativeTexture(NativeTextureCache& texture_cache,
                                                 const TextureKey& key, VkImage image,
                                                 VmaAllocation allocation, bool track_usage)
    : Texture(texture_cache, key, track_usage), image_(image), allocation_(allocation) {
  if (texture_cache.profile_transfers()) EnableWatchStats();
  VmaAllocationInfo allocation_info;
  vmaGetAllocationInfo(texture_cache.vma_allocator_, allocation_, &allocation_info);
  SetHostMemoryUsage(uint64_t(allocation_info.size));
}

NativeTextureCache::NativeTexture::~NativeTexture() {
  const NativeTextureCache& native_texture_cache =
      static_cast<const NativeTextureCache&>(texture_cache());
  const ui::vulkan::VulkanDevice* vulkan_device = native_texture_cache.submitter_.vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // The base destroys a texture only after the submissions using it.
  for (const auto& [key, view] : views_) {
    dfn.vkDestroyImageView(device, view, nullptr);
  }
  vmaDestroyImage(native_texture_cache.vma_allocator_, image_, allocation_);
}

VkImageView NativeTextureCache::NativeTexture::GetView(bool is_signed, uint32_t host_swizzle,
                                                       bool is_array) {
  // Red and blue as the guest memory has them (a resolve copied in swapped).
  if (red_blue_swapped_) {
    host_swizzle = TextureCache::SwapRedAndBlueSources(host_swizzle);
  }
  // The answer only depends on the arguments (the key, its formats and the
  // device stay the same; set_red_blue_swapped forgets it).
  const uint32_t arguments =
      (uint32_t(is_signed) << 13) | (uint32_t(is_array) << 12) | (host_swizzle & 0xFFF);
  if (arguments == last_view_arguments_) {
    return last_view_;
  }
  VkImageView view = GetViewUncached(is_signed, host_swizzle, is_array);
  if (view != VK_NULL_HANDLE) {
    last_view_arguments_ = arguments;
    last_view_ = view;
  }
  return view;
}

VkImageView NativeTextureCache::NativeTexture::GetViewUncached(bool is_signed,
                                                               uint32_t host_swizzle,
                                                               bool is_array) {
  const NativeTextureCache& native_texture_cache =
      static_cast<const NativeTextureCache&>(texture_cache());
  const HostFormatPair& host_format_pair = native_texture_cache.GetHostFormatPair(key());
  VkFormat format =
      (is_signed ? host_format_pair.format_signed : host_format_pair.format_unsigned).format;
  if (format == VK_FORMAT_UNDEFINED) {
    return VK_NULL_HANDLE;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = native_texture_cache.submitter_.vulkan_device();
  // Without imageViewFormatSwizzle, the shader swizzles (system constants).
  if (!vulkan_device->properties().imageViewFormatSwizzle) {
    host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
  // One view for both signednesses when the formats are the same.
  bool signed_separate_view =
      is_signed && host_format_pair.format_signed.format != host_format_pair.format_unsigned.format;
  // A 3D-as-2D copy has a 3D key and a 2D image. 1D and 2D are 2D arrays in
  // the translated shaders.
  VkImageViewType view_type;
  if (force_load_3d_tiling()) {
    view_type = is_array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  } else {
    switch (key().dimension) {
      case xenos::DataDimension::k3D:
        view_type = VK_IMAGE_VIEW_TYPE_3D;
        break;
      case xenos::DataDimension::kCube:
        view_type = VK_IMAGE_VIEW_TYPE_CUBE;
        break;
      default:
        view_type = is_array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        break;
    }
  }
  uint32_t view_key = (uint32_t(view_type == VK_IMAGE_VIEW_TYPE_2D) << 13) |
                      (uint32_t(signed_separate_view) << 12) | (host_swizzle & 0xFFF);
  auto it = views_.find(view_key);
  if (it != views_.end()) {
    return it->second;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image_;
  view_info.viewType = view_type;
  view_info.format = format;
  view_info.components.r = GuestComponentSwizzle(host_swizzle, 0);
  view_info.components.g = GuestComponentSwizzle(host_swizzle, 1);
  view_info.components.b = GuestComponentSwizzle(host_swizzle, 2);
  view_info.components.a = GuestComponentSwizzle(host_swizzle, 3);
  view_info.subresourceRange = util::InitializeSubresourceRange();
  if (view_type == VK_IMAGE_VIEW_TYPE_2D) {
    // The first layer of a stacked texture.
    view_info.subresourceRange.layerCount = 1;
  }
  VkImageView view;
  if (vulkan_device->functions().vkCreateImageView(vulkan_device->device(), &view_info, nullptr,
                                                   &view) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a texture view (Vulkan format {})",
                 uint32_t(format));
    return VK_NULL_HANDLE;
  }
  views_.emplace(view_key, view);
  return view;
}

NativeTextureCache::NativeTexture* NativeTextureCache::NativeTexture::Get3DAs2D() {
  NativeTextureCache& native_texture_cache = static_cast<NativeTextureCache&>(texture_cache());
  if (!texture_3d_as_2d_) {
    // The first slice, the base level only, tiled as 3D (the SDK's wrapper).
    TextureKey key_2d = key();
    key_2d.depth_or_array_size_minus_1 = 0;
    key_2d.mip_max_level = 0;
    texture_3d_as_2d_ = native_texture_cache.CreateNativeTexture(key_2d, true, false);
    if (!texture_3d_as_2d_) {
      return nullptr;
    }
    texture_3d_as_2d_->SetForceLoad3DTiling(true);
    if (!(native_texture_cache.logged_ & kLog3DAs2D)) {
      native_texture_cache.logged_ |= kLog3DAs2D;
      REXGPU_INFO("Native renderer: a 3D texture fetched as 2D is sampled from its first slice");
    }
  }
  // Its own memory watch: loaded again when the guest rewrites the texture.
  if (texture_3d_as_2d_->outdated_mask() &&
      !native_texture_cache.LoadTextureData(*texture_3d_as_2d_)) {
    return nullptr;
  }
  return texture_3d_as_2d_.get();
}

NativeTextureCache::NativeTextureCache(const RegisterFile& register_file,
                                       NativeSharedMemory& shared_memory, Submitter& submitter)
    : TextureCache(register_file, shared_memory, 1, 1),
      native_shared_memory_(shared_memory),
      submitter_(submitter) {}

NativeTextureCache::~NativeTextureCache() {
  // The caller has waited for the GPU.
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_.vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (const auto& [key, sampler] : samplers_) {
    dfn.vkDestroySampler(device, sampler, nullptr);
  }
  samplers_.clear();
  // Textures before the allocator their memory is in.
  DestroyAllTextures(true);
  for (VkPipeline pipeline : load_pipelines_) {
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
  util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, load_pipeline_layout_);
  // Destroying the pool frees its sets.
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, load_set_pool_);
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, load_set_layout_);
  if (scratch_buffer_ != VK_NULL_HANDLE) {
    vmaDestroyBuffer(vma_allocator_, scratch_buffer_, scratch_allocation_);
  }
  if (vma_allocator_ != VK_NULL_HANDLE) {
    vmaDestroyAllocator(vma_allocator_);
  }
}

bool NativeTextureCache::Initialize() {
  profile_transfers_ = REXCVAR_GET(native_profile_transfers);
  resident_load_stats_ = {};
  resident_detail_total_ = {};
  resident_details_.clear();
  resident_detail_overflow_ = 0;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_.vulkan_device();
  const ui::vulkan::VulkanInstance::Functions& ifn = vulkan_device->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device->physical_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  // Externally synchronized: the command processor thread only.
  vma_allocator_ = ui::vulkan::CreateVmaAllocator(vulkan_device, true);
  if (vma_allocator_ == VK_NULL_HANDLE) {
    return false;
  }

  // Host formats: the best ones, then the SDK's fallbacks for what the device
  // can't sample (as VulkanTextureCache::Initialize).
  for (size_t i = 0; i < rex::countof(host_formats_); ++i) {
    host_formats_[i] = kBestHostFormats[i];
  }
  constexpr VkFormatFeatureFlags kLinearFilterFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  auto features = [&](VkFormat format) {
    VkFormatProperties properties;
    ifn.vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
    return properties.optimalTilingFeatures;
  };
  auto filterable = [&](VkFormat format) {
    return (features(format) & kLinearFilterFeatures) == kLinearFilterFeatures;
  };
  // Decompressed or expanded to RGBA8 / RG8 / R8 when not sampleable.
  auto fall_back = [&](xenos::TextureFormat guest_format, LoadShaderIndex load_shader,
                       VkFormat format) {
    HostFormatPair& host_format = host_formats_[uint32_t(guest_format)];
    host_format.format_unsigned.load_shader = load_shader;
    host_format.format_unsigned.format = format;
    host_format.format_unsigned.block_compressed = false;
    host_format.unsigned_signed_compatible = false;
  };
  if (!filterable(VK_FORMAT_G8B8G8R8_422_UNORM)) {
    fall_back(xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP, kLoadShaderIndexGBGR8ToRGB8,
              VK_FORMAT_R8G8B8A8_UNORM);
  }
  if (!filterable(VK_FORMAT_B8G8R8G8_422_UNORM)) {
    fall_back(xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP, kLoadShaderIndexBGRG8ToRGB8,
              VK_FORMAT_R8G8B8A8_UNORM);
  }
  if (!filterable(VK_FORMAT_R5G6B5_UNORM_PACK16)) {
    fall_back(xenos::TextureFormat::k_5_6_5, kLoadShaderIndexR5G6B5ToRGBA8,
              VK_FORMAT_R8G8B8A8_UNORM);
  }
  if (!filterable(VK_FORMAT_B4G4R4A4_UNORM_PACK16)) {
    // MoltenVK on some Metal devices.
    fall_back(xenos::TextureFormat::k_4_4_4_4, kLoadShaderIndexRGBA4ToRGBA8,
              VK_FORMAT_R8G8B8A8_UNORM);
  }
  // 10_11_11 / 11_11_10: decoded to normalized RGBA16, converted to float16 in
  // a second pass if normalized RGBA16 can't be filtered.
  if (!filterable(VK_FORMAT_R16G16B16A16_UNORM) || !filterable(VK_FORMAT_R16G16B16A16_SNORM)) {
    if (!filterable(VK_FORMAT_R16G16B16A16_SFLOAT)) {
      REXGPU_ERROR("Native renderer: neither normalized nor float16 RGBA16 textures can be "
                   "filtered");
      return false;
    }
    for (xenos::TextureFormat format :
         {xenos::TextureFormat::k_10_11_11, xenos::TextureFormat::k_11_11_10,
          xenos::TextureFormat::k_10_11_11_AS_16_16_16_16,
          xenos::TextureFormat::k_11_11_10_AS_16_16_16_16}) {
      HostFormatPair& host_format = host_formats_[uint32_t(format)];
      host_format.format_unsigned.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      host_format.format_signed.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      host_format.unsigned_signed_compatible = false;
    }
  }
  // S3TC, decompressed when the BC format can't be sampled.
  if (!filterable(VK_FORMAT_BC1_RGBA_UNORM_BLOCK)) {
    fall_back(xenos::TextureFormat::k_DXT1, kLoadShaderIndexDXT1ToRGBA8,
              VK_FORMAT_R8G8B8A8_UNORM);
    host_formats_[uint32_t(xenos::TextureFormat::k_DXT1_AS_16_16_16_16)] =
        host_formats_[uint32_t(xenos::TextureFormat::k_DXT1)];
  }
  if (!filterable(VK_FORMAT_BC2_UNORM_BLOCK)) {
    fall_back(xenos::TextureFormat::k_DXT2_3, kLoadShaderIndexDXT3ToRGBA8,
              VK_FORMAT_R8G8B8A8_UNORM);
    host_formats_[uint32_t(xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16)] =
        host_formats_[uint32_t(xenos::TextureFormat::k_DXT2_3)];
  }
  if (!filterable(VK_FORMAT_BC3_UNORM_BLOCK)) {
    fall_back(xenos::TextureFormat::k_DXT4_5, kLoadShaderIndexDXT5ToRGBA8,
              VK_FORMAT_R8G8B8A8_UNORM);
    host_formats_[uint32_t(xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16)] =
        host_formats_[uint32_t(xenos::TextureFormat::k_DXT4_5)];
  }
  if (!filterable(VK_FORMAT_BC5_UNORM_BLOCK)) {
    fall_back(xenos::TextureFormat::k_DXN, kLoadShaderIndexDXNToRG8, VK_FORMAT_R8G8_UNORM);
  }
  if (!filterable(VK_FORMAT_BC4_UNORM_BLOCK)) {
    fall_back(xenos::TextureFormat::k_DXT5A, kLoadShaderIndexDXT5AToR8, VK_FORMAT_R8_UNORM);
  }
  // Signed versions of the formats Direct3D 12 can't sample signed either.
  for (xenos::TextureFormat format :
       {xenos::TextureFormat::k_1_5_5_5, xenos::TextureFormat::k_5_6_5,
        xenos::TextureFormat::k_6_5_5, xenos::TextureFormat::k_2_10_10_10,
        xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP, xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP,
        xenos::TextureFormat::k_4_4_4_4, xenos::TextureFormat::k_DXT1,
        xenos::TextureFormat::k_DXT2_3, xenos::TextureFormat::k_DXT4_5,
        xenos::TextureFormat::k_DXN, xenos::TextureFormat::k_DXT5A, xenos::TextureFormat::k_CTX1,
        xenos::TextureFormat::k_DXT1_AS_16_16_16_16, xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16,
        xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16,
        xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16, xenos::TextureFormat::k_DXT3A,
        xenos::TextureFormat::k_DXT3A_AS_1_1_1_1}) {
    HostFormatPair& host_format = host_formats_[uint32_t(format)];
    host_format.format_signed.load_shader = kLoadShaderIndexUnknown;
    host_format.format_signed.format = VK_FORMAT_UNDEFINED;
    host_format.format_signed.linear_filterable = false;
    host_format.unsigned_signed_compatible = false;
  }
  // 16-bit normalized formats are optional: float16 instead.
  auto norm16 = [&](xenos::TextureFormat guest_format, VkFormat unorm, VkFormat snorm,
                    VkFormat sfloat, LoadShaderIndex unorm_to_float,
                    LoadShaderIndex snorm_to_float) {
    HostFormatPair& host_format = host_formats_[uint32_t(guest_format)];
    if (!filterable(unorm)) {
      host_format.format_unsigned.load_shader = unorm_to_float;
      host_format.format_unsigned.format = sfloat;
    }
    if (!filterable(snorm)) {
      host_format.format_signed.load_shader = snorm_to_float;
      host_format.format_signed.format = sfloat;
    }
    host_format.unsigned_signed_compatible =
        (host_format.format_unsigned.format == unorm && host_format.format_signed.format == snorm) ||
        (host_format.format_unsigned.format == sfloat && host_format.format_signed.format == sfloat);
  };
  norm16(xenos::TextureFormat::k_16, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM,
         VK_FORMAT_R16_SFLOAT, kLoadShaderIndexR16UNormToFloat, kLoadShaderIndexR16SNormToFloat);
  norm16(xenos::TextureFormat::k_16_16, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM,
         VK_FORMAT_R16G16_SFLOAT, kLoadShaderIndexRG16UNormToFloat,
         kLoadShaderIndexRG16SNormToFloat);
  norm16(xenos::TextureFormat::k_16_16_16_16, VK_FORMAT_R16G16B16A16_UNORM,
         VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
         kLoadShaderIndexRGBA16UNormToFloat, kLoadShaderIndexRGBA16SNormToFloat);
  // Normalized: undefined formats have no load shader and the reverse; what
  // the device can't sample at all is undefined; linear filtering as it can.
  for (size_t i = 0; i < rex::countof(host_formats_); ++i) {
    HostFormatPair& host_format = host_formats_[i];
    for (HostFormat* format : {&host_format.format_unsigned, &host_format.format_signed}) {
      if (format->format == VK_FORMAT_UNDEFINED ||
          format->load_shader == kLoadShaderIndexUnknown) {
        format->format = VK_FORMAT_UNDEFINED;
        format->load_shader = kLoadShaderIndexUnknown;
        format->linear_filterable = false;
        continue;
      }
      VkFormatFeatureFlags format_features = features(format->format);
      if (format_features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
        format->linear_filterable =
            (format_features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
      } else {
        REXGPU_INFO("Native renderer: texture format {} can't be sampled as Vulkan format {}",
                    FormatInfo::Get(xenos::TextureFormat(i))->name, uint32_t(format->format));
        format->format = VK_FORMAT_UNDEFINED;
        format->load_shader = kLoadShaderIndexUnknown;
        format->linear_filterable = false;
      }
    }
    // One image for both signednesses only with the same load shader and a
    // compatible format (the same format after the fallbacks always is).
    if (host_format.format_unsigned.format != VK_FORMAT_UNDEFINED &&
        host_format.format_signed.format != VK_FORMAT_UNDEFINED) {
      if (host_format.format_unsigned.load_shader == host_format.format_signed.load_shader) {
        if (host_format.format_unsigned.format == host_format.format_signed.format) {
          host_format.unsigned_signed_compatible = true;
        }
      } else {
        host_format.unsigned_signed_compatible = false;
      }
      if (host_format.format_unsigned.block_compressed !=
          host_format.format_signed.block_compressed) {
        host_format.unsigned_signed_compatible = false;
      }
    } else {
      host_format.unsigned_signed_compatible = false;
    }
  }

  // Load pipeline layout: two compute storage buffers and the constants.
  VkDescriptorSetLayoutBinding storage_binding = {};
  storage_binding.binding = 0;
  storage_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  storage_binding.descriptorCount = 1;
  storage_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &storage_binding;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &load_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayout load_set_layouts[2] = {load_set_layout_, load_set_layout_};
  VkPushConstantRange push_constant_range = {};
  push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = sizeof(LoadConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 2;
  pipeline_layout_info.pSetLayouts = load_set_layouts;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                 &load_pipeline_layout_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorPoolSize pool_size = {};
  pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_size.descriptorCount = kLoadSetPoolSize;
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.maxSets = kLoadSetPoolSize;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &load_set_pool_) != VK_SUCCESS) {
    return false;
  }
  // The whole shared memory as the source when it fits (the offset pushed).
  if (device_properties.maxStorageBufferRange >= SharedMemory::kBufferSize) {
    shared_memory_set_ =
        AllocateStorageBufferSet(native_shared_memory_.buffer(), 0, SharedMemory::kBufferSize);
  }

  // Load pipelines for the shaders the formats use.
  std::pair<const uint32_t*, size_t> load_shader_code[kLoadShaderCount] = {};
  auto code = [&](LoadShaderIndex index, const uint32_t* words, size_t size) {
    load_shader_code[index] = std::make_pair(words, size);
  };
  code(kLoadShaderIndex8bpb, shaders::texture_load_8bpb_cs, sizeof(shaders::texture_load_8bpb_cs));
  code(kLoadShaderIndex16bpb, shaders::texture_load_16bpb_cs,
       sizeof(shaders::texture_load_16bpb_cs));
  code(kLoadShaderIndex32bpb, shaders::texture_load_32bpb_cs,
       sizeof(shaders::texture_load_32bpb_cs));
  code(kLoadShaderIndex64bpb, shaders::texture_load_64bpb_cs,
       sizeof(shaders::texture_load_64bpb_cs));
  code(kLoadShaderIndex128bpb, shaders::texture_load_128bpb_cs,
       sizeof(shaders::texture_load_128bpb_cs));
  code(kLoadShaderIndexR5G5B5A1ToB5G5R5A1, shaders::texture_load_r5g5b5a1_b5g5r5a1_cs,
       sizeof(shaders::texture_load_r5g5b5a1_b5g5r5a1_cs));
  code(kLoadShaderIndexR5G6B5ToB5G6R5, shaders::texture_load_r5g6b5_b5g6r5_cs,
       sizeof(shaders::texture_load_r5g6b5_b5g6r5_cs));
  code(kLoadShaderIndexR5G6B5ToRGBA8, shaders::texture_load_r5g6b5_rgba8_cs,
       sizeof(shaders::texture_load_r5g6b5_rgba8_cs));
  code(kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle,
       shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs,
       sizeof(shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs));
  code(kLoadShaderIndexRGBA4ToARGB4, shaders::texture_load_r4g4b4a4_a4r4g4b4_cs,
       sizeof(shaders::texture_load_r4g4b4a4_a4r4g4b4_cs));
  code(kLoadShaderIndexRGBA4ToRGBA8, shaders::texture_load_r4g4b4a4_rgba8_cs,
       sizeof(shaders::texture_load_r4g4b4a4_rgba8_cs));
  code(kLoadShaderIndexGBGR8ToRGB8, shaders::texture_load_gbgr8_rgb8_cs,
       sizeof(shaders::texture_load_gbgr8_rgb8_cs));
  code(kLoadShaderIndexBGRG8ToRGB8, shaders::texture_load_bgrg8_rgb8_cs,
       sizeof(shaders::texture_load_bgrg8_rgb8_cs));
  code(kLoadShaderIndexR10G11B11ToRGBA16, shaders::texture_load_r10g11b11_rgba16_cs,
       sizeof(shaders::texture_load_r10g11b11_rgba16_cs));
  code(kLoadShaderIndexR10G11B11ToRGBA16SNorm, shaders::texture_load_r10g11b11_rgba16_snorm_cs,
       sizeof(shaders::texture_load_r10g11b11_rgba16_snorm_cs));
  code(kLoadShaderIndexR11G11B10ToRGBA16, shaders::texture_load_r11g11b10_rgba16_cs,
       sizeof(shaders::texture_load_r11g11b10_rgba16_cs));
  code(kLoadShaderIndexR11G11B10ToRGBA16SNorm, shaders::texture_load_r11g11b10_rgba16_snorm_cs,
       sizeof(shaders::texture_load_r11g11b10_rgba16_snorm_cs));
  code(kLoadShaderIndexR16UNormToFloat, shaders::texture_load_r16_unorm_float_cs,
       sizeof(shaders::texture_load_r16_unorm_float_cs));
  code(kLoadShaderIndexR16SNormToFloat, shaders::texture_load_r16_snorm_float_cs,
       sizeof(shaders::texture_load_r16_snorm_float_cs));
  code(kLoadShaderIndexRG16UNormToFloat, shaders::texture_load_rg16_unorm_float_cs,
       sizeof(shaders::texture_load_rg16_unorm_float_cs));
  code(kLoadShaderIndexRG16SNormToFloat, shaders::texture_load_rg16_snorm_float_cs,
       sizeof(shaders::texture_load_rg16_snorm_float_cs));
  code(kLoadShaderIndexRGBA16UNormToFloat, shaders::texture_load_rgba16_unorm_float_cs,
       sizeof(shaders::texture_load_rgba16_unorm_float_cs));
  code(kLoadShaderIndexRGBA16SNormToFloat, shaders::texture_load_rgba16_snorm_float_cs,
       sizeof(shaders::texture_load_rgba16_snorm_float_cs));
  code(kLoadShaderIndexDXT1ToRGBA8, shaders::texture_load_dxt1_rgba8_cs,
       sizeof(shaders::texture_load_dxt1_rgba8_cs));
  code(kLoadShaderIndexDXT3ToRGBA8, shaders::texture_load_dxt3_rgba8_cs,
       sizeof(shaders::texture_load_dxt3_rgba8_cs));
  code(kLoadShaderIndexDXT5ToRGBA8, shaders::texture_load_dxt5_rgba8_cs,
       sizeof(shaders::texture_load_dxt5_rgba8_cs));
  code(kLoadShaderIndexDXNToRG8, shaders::texture_load_dxn_rg8_cs,
       sizeof(shaders::texture_load_dxn_rg8_cs));
  code(kLoadShaderIndexDXT3A, shaders::texture_load_dxt3a_cs,
       sizeof(shaders::texture_load_dxt3a_cs));
  code(kLoadShaderIndexDXT3AAs1111ToARGB4, shaders::texture_load_dxt3aas1111_argb4_cs,
       sizeof(shaders::texture_load_dxt3aas1111_argb4_cs));
  code(kLoadShaderIndexDXT5AToR8, shaders::texture_load_dxt5a_r8_cs,
       sizeof(shaders::texture_load_dxt5a_r8_cs));
  code(kLoadShaderIndexCTX1, shaders::texture_load_ctx1_cs, sizeof(shaders::texture_load_ctx1_cs));
  code(kLoadShaderIndexDepthUnorm, shaders::texture_load_depth_unorm_cs,
       sizeof(shaders::texture_load_depth_unorm_cs));
  code(kLoadShaderIndexDepthFloat, shaders::texture_load_depth_float_cs,
       sizeof(shaders::texture_load_depth_float_cs));
  bool load_shaders_needed[kLoadShaderCount] = {};
  auto mark_needed = [&](const HostFormatPair& host_format_pair) {
    for (const HostFormat* format :
         {&host_format_pair.format_unsigned, &host_format_pair.format_signed}) {
      if (format->load_shader != kLoadShaderIndexUnknown) {
        load_shaders_needed[format->load_shader] = true;
      }
    }
  };
  for (const HostFormatPair& host_format_pair : host_formats_) {
    mark_needed(host_format_pair);
  }
  for (const HostFormatPair* host_format_pair :
       {&kHostFormatGBGRUnaligned, &kHostFormatBGRGUnaligned, &kHostFormatDXT1Unaligned,
        &kHostFormatDXT2_3Unaligned, &kHostFormatDXT4_5Unaligned, &kHostFormatDXNUnaligned,
        &kHostFormatDXT5AUnaligned}) {
    mark_needed(*host_format_pair);
  }
  // The float16 conversion pass of 10_11_11 / 11_11_10.
  load_shaders_needed[kLoadShaderIndexRGBA16UNormToFloat] = true;
  load_shaders_needed[kLoadShaderIndexRGBA16SNormToFloat] = true;
  for (uint32_t i = 0; i < uint32_t(kLoadShaderCount); ++i) {
    if (!load_shaders_needed[i] || !load_shader_code[i].first) {
      continue;
    }
    load_pipelines_[i] = util::CreateComputePipeline(
        vulkan_device, load_pipeline_layout_, load_shader_code[i].first,
        load_shader_code[i].second);
    if (load_pipelines_[i] == VK_NULL_HANDLE) {
      REXGPU_ERROR("Native renderer: failed to create texture load pipeline {}", i);
      return false;
    }
  }

  // Samplers: the device's count, less the UI's and a few for overlays.
  sampler_max_count_ = device_properties.maxSamplerAllocationCount -
                       std::min(device_properties.maxSamplerAllocationCount,
                                uint32_t(ui::vulkan::UISamplers::kSamplerCount) + 16);
  if (device_properties.samplerAnisotropy) {
    max_anisotropy_ = xenos::AnisoFilter(
        uint32_t(xenos::AnisoFilter::kMax_1_1) +
        (31 - rex::lzcnt(uint32_t(
                  std::min(16.0f, std::max(1.0f, device_properties.maxSamplerAnisotropy))))));
  } else {
    max_anisotropy_ = xenos::AnisoFilter::kDisabled;
  }
  return true;
}

void NativeTextureCache::UpdateSubmissions(bool poll_completed) {
  uint64_t current = submitter_.current();
  if (current > submission_begun_) {
    submission_begun_ = current;
    BeginSubmission(current);
  }
  if (!poll_completed) {
    return;
  }
  uint64_t completed = submitter_.completed();
  if (completed > completed_reported_) {
    completed_reported_ = completed;
    CompletedSubmissionUpdated(completed);
  }
}

VkDescriptorSet NativeTextureCache::AllocateStorageBufferSet(VkBuffer buffer, VkDeviceSize offset,
                                                             VkDeviceSize range) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_.vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkDescriptorSetAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorPool = load_set_pool_;
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &load_set_layout_;
  VkDescriptorSet set;
  if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &set) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  VkDescriptorBufferInfo buffer_info = {};
  buffer_info.buffer = buffer;
  buffer_info.offset = offset;
  buffer_info.range = range;
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &buffer_info;
  dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  return set;
}

void NativeTextureCache::FreeStorageBufferSetLater(VkDescriptorSet set) {
  if (set == VK_NULL_HANDLE) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_.vulkan_device();
  VkDescriptorPool pool = load_set_pool_;
  submitter_.DestroyAfter([vulkan_device, pool, set]() {
    vulkan_device->functions().vkFreeDescriptorSets(vulkan_device->device(), pool, 1, &set);
  });
}

bool NativeTextureCache::EnsureScratchBuffer(VkDeviceSize size) {
  if (scratch_buffer_ != VK_NULL_HANDLE && scratch_size_ >= size) {
    return true;
  }
  // The old one may still be read by copies in flight.
  if (scratch_buffer_ != VK_NULL_HANDLE) {
    VmaAllocator allocator = vma_allocator_;
    VkBuffer buffer = scratch_buffer_;
    VmaAllocation allocation = scratch_allocation_;
    submitter_.DestroyAfter(
        [allocator, buffer, allocation]() { vmaDestroyBuffer(allocator, buffer, allocation); });
    FreeStorageBufferSetLater(scratch_set_);
    scratch_buffer_ = VK_NULL_HANDLE;
    scratch_allocation_ = VK_NULL_HANDLE;
    scratch_set_ = VK_NULL_HANDLE;
    scratch_size_ = 0;
  }
  // Grown in steps of 4 MB, not one load at a time.
  VkDeviceSize new_size = rex::align(std::max(size, VkDeviceSize(4) << 20), VkDeviceSize(4) << 20);
  VkBufferCreateInfo buffer_info = {};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = new_size;
  // Also filled and copied within itself (InitializeTexture).
  buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo allocation_info = {};
  allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateBuffer(vma_allocator_, &buffer_info, &allocation_info, &scratch_buffer_,
                      &scratch_allocation_, nullptr) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a {} MB texture load buffer",
                 new_size >> 20);
    scratch_buffer_ = VK_NULL_HANDLE;
    scratch_allocation_ = VK_NULL_HANDLE;
    return false;
  }
  scratch_size_ = new_size;
  scratch_set_ = AllocateStorageBufferSet(scratch_buffer_, 0, new_size);
  return scratch_set_ != VK_NULL_HANDLE;
}

const NativeTextureCache::HostFormatPair& NativeTextureCache::GetHostFormatPair(
    TextureKey key) const {
  const HostFormatPair& host_format_pair = host_formats_[uint32_t(key.format)];
  if (!host_format_pair.format_unsigned.block_compressed &&
      !host_format_pair.format_signed.block_compressed) {
    return host_format_pair;
  }
  // Decompressed when the size isn't a whole number of blocks.
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  if (!(key.GetWidth() & (format_info->block_width - 1)) &&
      !(key.GetHeight() & (format_info->block_height - 1))) {
    return host_format_pair;
  }
  switch (key.format) {
    case xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP:
      return kHostFormatGBGRUnaligned;
    case xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP:
      return kHostFormatBGRGUnaligned;
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      return kHostFormatDXT1Unaligned;
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return kHostFormatDXT2_3Unaligned;
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return kHostFormatDXT4_5Unaligned;
    case xenos::TextureFormat::k_DXN:
      return kHostFormatDXNUnaligned;
    case xenos::TextureFormat::k_DXT5A:
      return kHostFormatDXT5AUnaligned;
    default:
      return host_format_pair;
  }
}

bool NativeTextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  const HostFormatPair& host_format_pair = GetHostFormatPair(key);
  if (host_format_pair.format_unsigned.format == VK_FORMAT_UNDEFINED ||
      host_format_pair.format_signed.format == VK_FORMAT_UNDEFINED) {
    return false;
  }
  return !host_format_pair.unsigned_signed_compatible;
}

uint32_t NativeTextureCache::GetHostFormatSwizzle(TextureKey key) const {
  return GetHostFormatPair(key).swizzle;
}

uint32_t NativeTextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  const ui::vulkan::VulkanDevice::Properties& properties = submitter_.vulkan_device()->properties();
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      return properties.maxImageDimension2D;
    case xenos::DataDimension::k3D:
      return properties.maxImageDimension3D;
    case xenos::DataDimension::kCube:
      return properties.maxImageDimensionCube;
    default:
      return 0;
  }
}

uint32_t NativeTextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  const ui::vulkan::VulkanDevice::Properties& properties = submitter_.vulkan_device()->properties();
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      return properties.maxImageArrayLayers;
    case xenos::DataDimension::k3D:
      return properties.maxImageDimension3D;
    case xenos::DataDimension::kCube:
      // No cube arrays on the Xenos.
      return 6;
    default:
      return 0;
  }
}

void NativeTextureCache::GetTextureImageFormats(TextureKey key, VkFormat (&formats)[2]) const {
  formats[0] = VK_FORMAT_UNDEFINED;
  formats[1] = VK_FORMAT_UNDEFINED;
  const HostFormatPair& host_format = GetHostFormatPair(key);
  if (host_format.format_signed.format == VK_FORMAT_UNDEFINED) {
    formats[0] = host_format.format_unsigned.format;
  } else if (host_format.format_unsigned.format == VK_FORMAT_UNDEFINED) {
    formats[0] = host_format.format_signed.format;
  } else if (IsSignedVersionSeparateForFormat(key)) {
    formats[0] =
        key.signed_separate ? host_format.format_signed.format : host_format.format_unsigned.format;
  } else {
    // One image, with a second view format if they differ.
    formats[0] = host_format.format_unsigned.format;
    if (host_format.format_signed.format != host_format.format_unsigned.format) {
      formats[1] = host_format.format_signed.format;
    }
  }
}

std::unique_ptr<TextureCache::Texture> NativeTextureCache::CreateTexture(TextureKey key) {
  return CreateNativeTexture(key, false, true);
}

std::unique_ptr<NativeTextureCache::NativeTexture> NativeTextureCache::CreateNativeTexture(
    TextureKey key, bool image_2d, bool track_usage) {
  VkFormat formats[2];
  GetTextureImageFormats(key, formats);
  if (formats[0] == VK_FORMAT_UNDEFINED) {
    return nullptr;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_.vulkan_device();
  bool is_3d = !image_2d && key.dimension == xenos::DataDimension::k3D;
  uint32_t depth_or_array_size = key.GetDepthOrArraySize();
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  if (formats[1] != VK_FORMAT_UNDEFINED) {
    image_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  }
  if (!image_2d && key.dimension == xenos::DataDimension::kCube) {
    image_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  }
  image_info.imageType = is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  image_info.format = formats[0];
  image_info.extent.width = key.GetWidth();
  image_info.extent.height = key.GetHeight();
  image_info.extent.depth = is_3d ? depth_or_array_size : 1;
  image_info.mipLevels = key.mip_max_level + 1;
  image_info.arrayLayers = is_3d ? 1 : depth_or_array_size;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageFormatListCreateInfo format_list_info = {};
  if (formats[1] != VK_FORMAT_UNDEFINED &&
      vulkan_device->extensions().ext_1_2_KHR_image_format_list) {
    format_list_info.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    format_list_info.viewFormatCount = 2;
    format_list_info.pViewFormats = formats;
    image_info.pNext = &format_list_info;
  }
  VmaAllocationCreateInfo allocation_info = {};
  allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  VkImage image;
  VmaAllocation allocation;
  if (vmaCreateImage(vma_allocator_, &image_info, &allocation_info, &image, &allocation,
                     nullptr) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a {}x{}x{} texture (Vulkan format {})",
                 image_info.extent.width, image_info.extent.height, depth_or_array_size,
                 uint32_t(formats[0]));
    return nullptr;
  }
  return std::make_unique<NativeTexture>(*this, key, image, allocation, track_usage);
}

void NativeTextureCache::TransitionTexture(CommandRecorder* command_buffer, NativeTexture& texture,
                                           NativeTexture::Usage usage) {
  auto layout = [](NativeTexture::Usage texture_usage) {
    switch (texture_usage) {
      case NativeTexture::Usage::kTransferDestination:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      case NativeTexture::Usage::kSampled:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      default:
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }
  };
  if (texture.usage() == NativeTexture::Usage::kUndefined &&
      usage == NativeTexture::Usage::kSampled) {
    // No load wrote it.
    InitializeTexture(command_buffer, texture, 1, 0);
  }
  ImageBarrier(submitter_.vulkan_device()->functions(), command_buffer, texture.image(),
               VK_IMAGE_ASPECT_COLOR_BIT, layout(texture.usage()), layout(usage));
  texture.set_usage(usage);
}

bool NativeTextureCache::InitializeTexture(CommandRecorder* command_buffer, NativeTexture& texture,
                                           uint32_t written_first, uint32_t written_last) {
  if (texture.usage() != NativeTexture::Usage::kUndefined) {
    return false;
  }
  const TextureKey key = texture.key();
  const bool poison = REXCVAR_GET(native_poison_new_images);
  // The levels to fill.
  const uint32_t level_last = key.mip_max_level;
  uint32_t fill_levels = 0;
  for (uint32_t level = 0; level <= level_last; ++level) {
    if (poison || level < written_first || level > written_last) {
      fill_levels |= uint32_t(1) << level;
    }
  }
  if (!fill_levels) {
    return false;
  }
  VkFormat formats[2];
  GetTextureImageFormats(key, formats);
  uint32_t block_width, block_height, block_bytes;
  uint8_t magenta[16];
  if (!GetHostBlock(formats[0], block_width, block_height, block_bytes, magenta)) {
    return false;
  }
  // As CreateNativeTexture made it (a 3D-as-2D copy: one slice, one level).
  const bool is_3d = key.dimension == xenos::DataDimension::k3D;
  const uint32_t width = key.GetWidth();
  const uint32_t height = key.GetHeight();
  const uint32_t depth_or_array_size = key.GetDepthOrArraySize();
  const uint32_t depth = is_3d ? depth_or_array_size : 1;
  const uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  // Every region reads the buffer from its start: the largest level's bytes.
  std::array<VkBufferImageCopy, xenos::kTextureMaxMips> regions = {};
  uint32_t region_count = 0;
  VkDeviceSize fill_size = 0;
  for (uint32_t level = 0; level <= level_last; ++level) {
    if (!(fill_levels & (uint32_t(1) << level))) {
      continue;
    }
    VkBufferImageCopy& region = regions[region_count++];
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = level;
    region.imageSubresource.layerCount = array_size;
    region.imageExtent.width = std::max(width >> level, uint32_t(1));
    region.imageExtent.height = std::max(height >> level, uint32_t(1));
    region.imageExtent.depth = std::max(depth >> level, uint32_t(1));
    fill_size = std::max(
        fill_size, VkDeviceSize(block_bytes) *
                       ((region.imageExtent.width + (block_width - 1)) / block_width) *
                       ((region.imageExtent.height + (block_height - 1)) / block_height) *
                       region.imageExtent.depth * array_size);
  }
  fill_size = rex::align(fill_size, VkDeviceSize(16));
  if (!EnsureScratchBuffer(fill_size)) {
    return false;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = submitter_.vulkan_device()->functions();
  // After the scratch buffer's earlier reads.
  FullMemoryBarrier(dfn, command_buffer);
  // Zero, or the magenta block repeated: a 32-bit word filled, a longer block
  // written once and doubled.
  uint8_t pattern[16] = {};
  if (poison) {
    for (uint32_t i = 0; i < 16; ++i) {
      pattern[i] = magenta[i % block_bytes];
    }
  }
  if (!std::memcmp(pattern, pattern + 4, 12)) {
    uint32_t word;
    std::memcpy(&word, pattern, sizeof(word));
    command_buffer->CmdVkFillBuffer(scratch_buffer_, 0, fill_size, word);
  } else {
    constexpr VkDeviceSize kWrittenSize = 4096;
    uint8_t written[kWrittenSize];
    for (VkDeviceSize i = 0; i < kWrittenSize; ++i) {
      written[i] = pattern[i & 15];
    }
    VkDeviceSize filled = std::min(fill_size, kWrittenSize);
    command_buffer->CmdVkUpdateBuffer(scratch_buffer_, 0, filled, written);
    while (filled < fill_size) {
      FullMemoryBarrier(dfn, command_buffer);
      VkBufferCopy copy = {};
      copy.dstOffset = filled;
      copy.size = std::min(filled, fill_size - filled);
      command_buffer->CmdVkCopyBuffer(scratch_buffer_, scratch_buffer_, 1, &copy);
      filled += copy.size;
    }
  }
  FullMemoryBarrier(dfn, command_buffer);
  TransitionTexture(command_buffer, texture, NativeTexture::Usage::kTransferDestination);
  command_buffer->CmdVkCopyBufferToImage(scratch_buffer_, texture.image(),
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region_count,
                                         regions.data());
  return true;
}

bool NativeTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                               bool load_mips) {
  NativeTexture& native_texture = static_cast<NativeTexture&>(texture);
  // Loaded as the guest memory has it (CopyResolveIntoTexture may have stored
  // red and blue the other way before).
  if (load_base && native_texture.red_blue_swapped()) {
    native_texture.set_red_blue_swapped(false);
    RebindTexture(native_texture);
  }
  if (profile_transfers_) ++resident_load_stats_.attempts;
  // Snapshot before recording, under the watch lock. Commit this baseline only
  // on success, so a failed recording does not consume diagnostic evidence.
  const auto watch_snapshot = profile_transfers_ ? texture.GetWatchStats() : Texture::WatchStats{};
  TextureKey texture_key = native_texture.key();

  // The load shader of the host format (the signed one if that's the data).
  const HostFormatPair& host_format_pair = GetHostFormatPair(texture_key);
  bool host_format_is_signed;
  if (IsSignedVersionSeparateForFormat(texture_key)) {
    host_format_is_signed = bool(texture_key.signed_separate);
  } else {
    host_format_is_signed = host_format_pair.format_unsigned.load_shader == kLoadShaderIndexUnknown;
  }
  const HostFormat& host_format =
      host_format_is_signed ? host_format_pair.format_signed : host_format_pair.format_unsigned;
  LoadShaderIndex load_shader = host_format.load_shader;
  if (load_shader == kLoadShaderIndexUnknown || load_pipelines_[load_shader] == VK_NULL_HANDLE) {
    return false;
  }
  VkPipeline pipeline = load_pipelines_[load_shader];
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(load_shader);
  // 10_11_11 / 11_11_10 into float16: a second, in-place pass.
  LoadShaderIndex load_shader_float_convert = kLoadShaderIndexUnknown;
  if (host_format.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
    switch (load_shader) {
      case kLoadShaderIndexR10G11B11ToRGBA16:
      case kLoadShaderIndexR11G11B10ToRGBA16:
        load_shader_float_convert = kLoadShaderIndexRGBA16UNormToFloat;
        break;
      case kLoadShaderIndexR10G11B11ToRGBA16SNorm:
      case kLoadShaderIndexR11G11B10ToRGBA16SNorm:
        load_shader_float_convert = kLoadShaderIndexRGBA16SNormToFloat;
        break;
      default:
        break;
    }
  }
  VkPipeline pipeline_float_convert = VK_NULL_HANDLE;
  const LoadShaderInfo* load_shader_info_float_convert = nullptr;
  if (load_shader_float_convert != kLoadShaderIndexUnknown) {
    pipeline_float_convert = load_pipelines_[load_shader_float_convert];
    if (pipeline_float_convert == VK_NULL_HANDLE) {
      return false;
    }
    load_shader_info_float_convert = &GetLoadShaderInfo(load_shader_float_convert);
    if (!(logged_ & kLogFloatConvert)) {
      logged_ |= kLogFloatConvert;
      REXGPU_INFO("Native renderer: 10_11_11 / 11_11_10 textures are converted to float16");
    }
  }

  // The guest layout.
  const texture_util::TextureGuestLayout& guest_layout = native_texture.guest_layout();
  bool is_3d = texture_key.dimension == xenos::DataDimension::k3D;
  bool is_3d_tiling = is_3d || native_texture.force_load_3d_tiling();
  uint32_t width = texture_key.GetWidth();
  uint32_t height = texture_key.GetHeight();
  uint32_t depth_or_array_size = texture_key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  xenos::TextureFormat guest_format = texture_key.format;
  const FormatInfo* guest_format_info = FormatInfo::Get(guest_format);
  uint32_t block_width = guest_format_info->block_width;
  uint32_t block_height = guest_format_info->block_height;
  uint32_t bytes_per_block = guest_format_info->bytes_per_block();
  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? texture_key.mip_max_level : 0;
  if (level_first > level_last) {
    return false;
  }
  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);
  // With the packed mip tail as level 0, the loop index is base / mips, else
  // the level.
  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  // The host layout in the scratch buffer.
  uint32_t host_block_width = host_format.block_compressed ? block_width : 1;
  uint32_t host_block_height = host_format.block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread = uint32_t(1) << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_format.block_compressed) {
    // Decompressing guest blocks.
    host_x_blocks_per_thread *= block_width;
  }
  struct HostLayout {
    VkDeviceSize offset_bytes;
    VkDeviceSize slice_size_bytes;
    uint32_t x_pitch_blocks;
    uint32_t y_pitch_blocks;
  };
  HostLayout host_layout_base = {};
  HostLayout host_layout_mips[xenos::kTextureMaxMips] = {};
  VkDeviceSize host_buffer_size = 0;
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = level_packed == 0 ? 0 : loop_level;
    HostLayout& level_host_layout = is_base ? host_layout_base : host_layout_mips[level];
    level_host_layout.offset_bytes = host_buffer_size;
    uint32_t level_x_extent_texels, level_y_extent_texels, level_z_extent;
    if (level == level_packed) {
      // The whole packed tail, the levels are copied out of it.
      const texture_util::TextureGuestLayout::Level& guest_layout_packed =
          is_base ? guest_layout.base : guest_layout.mips[level];
      level_x_extent_texels = guest_layout_packed.x_extent_blocks * block_width;
      level_y_extent_texels = guest_layout_packed.y_extent_blocks * block_height;
      level_z_extent = guest_layout_packed.z_extent;
    } else {
      level_x_extent_texels = std::max(width >> level, uint32_t(1));
      level_y_extent_texels = std::max(height >> level, uint32_t(1));
      level_z_extent = std::max(depth >> level, uint32_t(1));
    }
    level_host_layout.x_pitch_blocks = rex::round_up(
        (level_x_extent_texels + (host_block_width - 1)) / host_block_width,
        host_x_blocks_per_thread);
    level_host_layout.y_pitch_blocks =
        (level_y_extent_texels + (host_block_height - 1)) / host_block_height;
    level_host_layout.slice_size_bytes = VkDeviceSize(load_shader_info.bytes_per_host_block) *
                                         level_host_layout.x_pitch_blocks *
                                         level_host_layout.y_pitch_blocks * level_z_extent;
    host_buffer_size += level_host_layout.slice_size_bytes * array_size;
  }
  if (!host_buffer_size || !EnsureScratchBuffer(host_buffer_size)) {
    return false;
  }

  // The sources: the whole shared memory (offset pushed) or the base and mip
  // ranges. Shaders read up to 16 bytes at once: lengths aligned to 16.
  constexpr uint32_t kSourceLengthAlignment = 16;
  VkDescriptorSet source_base = VK_NULL_HANDLE;
  VkDescriptorSet source_mips = VK_NULL_HANDLE;
  bool persistent_source = shared_memory_set_ != VK_NULL_HANDLE;
  uint32_t base_start = texture_key.base_page << 12;
  uint32_t mips_start = texture_key.mip_page << 12;
  if (level_first == 0) {
    uint32_t base_range =
        rex::align(native_texture.GetGuestBaseSize(), kSourceLengthAlignment);
    if (base_range > SharedMemory::kBufferSize - base_start) {
      return false;
    }
    source_base = persistent_source ? shared_memory_set_
                                    : AllocateStorageBufferSet(native_shared_memory_.buffer(),
                                                               base_start, base_range);
    if (source_base == VK_NULL_HANDLE) {
      return false;
    }
  }
  if (level_last != 0) {
    uint32_t mips_range =
        rex::align(native_texture.GetGuestMipsSize(), kSourceLengthAlignment);
    if (mips_range > SharedMemory::kBufferSize - mips_start) {
      if (!persistent_source) {
        FreeStorageBufferSetLater(source_base);
      }
      return false;
    }
    source_mips = persistent_source ? shared_memory_set_
                                    : AllocateStorageBufferSet(native_shared_memory_.buffer(),
                                                               mips_start, mips_range);
    if (source_mips == VK_NULL_HANDLE) {
      if (!persistent_source) {
        FreeStorageBufferSetLater(source_base);
      }
      return false;
    }
  }
  if (!persistent_source) {
    // Referenced by this submission only.
    FreeStorageBufferSetLater(source_base);
    FreeStorageBufferSetLater(source_mips);
  }

  // Outside the draws' render pass: before it if it can stay open (recorded
  // at its hoist point until EndBeforeRenderPass below).
  const bool before_draws = CanLoadBeforeDraws(native_texture);
  CommandRecorder* command_buffer;
  if (before_draws) {
    command_buffer = submitter_.BeginBeforeRenderPass(GpuKind::kTextureLoads);
    submitter_.CountTextureLoadBeforeRenderPass();
  } else {
    native_shared_memory_.Use(NativeSharedMemory::Usage::kRead);
    command_buffer = submitter_.OpenOutsideRenderPass(RenderPassEnd::kTextureLoad);
    if (command_buffer == nullptr) {
      return false;
    }
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = submitter_.vulkan_device()->functions();
  // A new texture: the levels this load doesn't write (all, poisoned) first,
  // from the scratch buffer before the load fills it.
  InitializeTexture(command_buffer, native_texture, level_first, level_last);
  // The scratch buffer's last copy (or conversion) is done before it's
  // written again.
  FullMemoryBarrier(dfn, command_buffer);

  command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                              load_pipeline_layout_, kLoadDescriptorSetIndexDestination, 1,
                              &scratch_set_, 0, nullptr);
  VkDescriptorSet source_bound = VK_NULL_HANDLE;
  LoadConstants load_constants = {};
  // Tiled, 3D tiling, endianness, scale 1 x 1.
  load_constants.is_tiled_3d_endian_scale = uint32_t(texture_key.tiled) |
                                            (uint32_t(is_3d_tiling) << 1) |
                                            (uint32_t(texture_key.endianness) << 2) |
                                            (uint32_t(1) << 4) | (uint32_t(1) << 7);
  uint32_t guest_x_blocks_per_group_log2 = load_shader_info.GetGuestXBlocksPerGroupLog2();
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = level_packed == 0 ? 0 : loop_level;
    VkDescriptorSet source = is_base ? source_base : source_mips;
    if (source_bound != source) {
      source_bound = source;
      command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                  load_pipeline_layout_, kLoadDescriptorSetIndexSource, 1, &source,
                                  0, nullptr);
    }
    // With the whole buffer bound, the offset is from its start, else from the
    // start of the range bound.
    load_constants.guest_offset = persistent_source ? (is_base ? base_start : mips_start) : 0;
    if (!is_base) {
      load_constants.guest_offset += guest_layout.mip_offsets_bytes[level];
    }
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    load_constants.guest_pitch_aligned = level_guest_layout.row_pitch_bytes / bytes_per_block;
    load_constants.guest_z_stride_block_rows_aligned = level_guest_layout.z_slice_stride_block_rows;
    uint32_t level_width, level_height, level_depth;
    if (level == level_packed) {
      level_width = level_guest_layout.x_extent_blocks * block_width;
      level_height = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width = std::max(width >> level, uint32_t(1));
      level_height = std::max(height >> level, uint32_t(1));
      level_depth = std::max(depth >> level, uint32_t(1));
    }
    load_constants.size_blocks[0] = (level_width + (block_width - 1)) / block_width;
    load_constants.size_blocks[1] = (level_height + (block_height - 1)) / block_height;
    load_constants.size_blocks[2] = level_depth;
    load_constants.height_texels = level_height;
    uint32_t group_count_x =
        (load_constants.size_blocks[0] + ((uint32_t(1) << guest_x_blocks_per_group_log2) - 1)) >>
        guest_x_blocks_per_group_log2;
    uint32_t group_count_y =
        (load_constants.size_blocks[1] + ((uint32_t(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
        kLoadGuestYBlocksPerGroupLog2;
    const HostLayout& level_host_layout = is_base ? host_layout_base : host_layout_mips[level];
    load_constants.host_offset = uint32_t(level_host_layout.offset_bytes);
    load_constants.host_pitch =
        load_shader_info.bytes_per_host_block * level_host_layout.x_pitch_blocks;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      command_buffer->CmdVkPushConstants(load_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                             0, sizeof(load_constants), &load_constants);
      command_buffer->CmdVkDispatch(group_count_x, group_count_y,
                        load_constants.size_blocks[2]);
      load_constants.guest_offset += level_guest_layout.array_slice_stride_bytes;
      load_constants.host_offset += uint32_t(level_host_layout.slice_size_bytes);
    }
  }

  if (pipeline_float_convert != VK_NULL_HANDLE) {
    // In place, in the scratch buffer.
    FullMemoryBarrier(dfn, command_buffer);
    command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_float_convert);
    command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, load_pipeline_layout_,
                                            kLoadDescriptorSetIndexDestination, 1, &scratch_set_, 0,
                                            nullptr);
    command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                load_pipeline_layout_, kLoadDescriptorSetIndexSource, 1,
                                &scratch_set_, 0, nullptr);
    LoadConstants convert_constants = {};
    // Linear, 1 x 1.
    convert_constants.is_tiled_3d_endian_scale =
        (uint32_t(is_3d) << 1) | (uint32_t(1) << 4) | (uint32_t(1) << 7);
    uint32_t convert_x_blocks_per_group_log2 =
        load_shader_info_float_convert->GetGuestXBlocksPerGroupLog2();
    for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
      bool is_base = loop_level == 0;
      uint32_t level = level_packed == 0 ? 0 : loop_level;
      const texture_util::TextureGuestLayout::Level& level_guest_layout =
          is_base ? guest_layout.base : guest_layout.mips[level];
      uint32_t level_width, level_height, level_depth;
      if (level == level_packed) {
        level_width = level_guest_layout.x_extent_blocks * block_width;
        level_height = level_guest_layout.y_extent_blocks * block_height;
        level_depth = level_guest_layout.z_extent;
      } else {
        level_width = std::max(width >> level, uint32_t(1));
        level_height = std::max(height >> level, uint32_t(1));
        level_depth = std::max(depth >> level, uint32_t(1));
      }
      convert_constants.size_blocks[0] = (level_width + (block_width - 1)) / block_width;
      convert_constants.size_blocks[1] = (level_height + (block_height - 1)) / block_height;
      convert_constants.size_blocks[2] = level_depth;
      convert_constants.height_texels = level_height;
      uint32_t group_count_x = (convert_constants.size_blocks[0] +
                                ((uint32_t(1) << convert_x_blocks_per_group_log2) - 1)) >>
                               convert_x_blocks_per_group_log2;
      uint32_t group_count_y =
          (convert_constants.size_blocks[1] + ((uint32_t(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
          kLoadGuestYBlocksPerGroupLog2;
      const HostLayout& level_host_layout = is_base ? host_layout_base : host_layout_mips[level];
      convert_constants.guest_offset = uint32_t(level_host_layout.offset_bytes);
      // In RGBA16 blocks of the first pass, as the host pitch.
      convert_constants.guest_pitch_aligned = level_host_layout.x_pitch_blocks;
      convert_constants.guest_z_stride_block_rows_aligned = level_host_layout.y_pitch_blocks;
      convert_constants.host_offset = uint32_t(level_host_layout.offset_bytes);
      convert_constants.host_pitch =
          load_shader_info_float_convert->bytes_per_host_block * level_host_layout.x_pitch_blocks;
      for (uint32_t slice = 0; slice < array_size; ++slice) {
        command_buffer->CmdVkPushConstants(load_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(convert_constants), &convert_constants);
        command_buffer->CmdVkDispatch(group_count_x, group_count_y,
                          convert_constants.size_blocks[2]);
        convert_constants.guest_offset += uint32_t(level_host_layout.slice_size_bytes);
        convert_constants.host_offset += uint32_t(level_host_layout.slice_size_bytes);
      }
    }
  }

  // Into the image.
  FullMemoryBarrier(dfn, command_buffer);
  native_texture.MarkAsUsed();
  TransitionTexture(command_buffer, native_texture, NativeTexture::Usage::kTransferDestination);
  std::array<VkBufferImageCopy, xenos::kTextureMaxMips> copy_regions = {};
  uint32_t copy_region_count = level_last - level_first + 1;
  for (uint32_t level = level_first; level <= level_last; ++level) {
    VkBufferImageCopy& copy_region = copy_regions[level - level_first];
    const HostLayout& level_host_layout =
        level != 0 ? host_layout_mips[std::min(level, level_packed)] : host_layout_base;
    copy_region.bufferOffset = level_host_layout.offset_bytes;
    if (level >= level_packed) {
      uint32_t level_offset_blocks_x, level_offset_blocks_y, level_offset_z;
      texture_util::GetPackedMipOffset(width, height, depth, guest_format, level,
                                       level_offset_blocks_x, level_offset_blocks_y,
                                       level_offset_z);
      uint32_t level_offset_host_blocks_x = level_offset_blocks_x;
      uint32_t level_offset_host_blocks_y = level_offset_blocks_y;
      if (!host_format.block_compressed) {
        level_offset_host_blocks_x *= block_width;
        level_offset_host_blocks_y *= block_height;
      }
      copy_region.bufferOffset +=
          load_shader_info.bytes_per_host_block *
          (level_offset_host_blocks_x +
           level_host_layout.x_pitch_blocks *
               (level_offset_host_blocks_y +
                level_host_layout.y_pitch_blocks * VkDeviceSize(level_offset_z)));
    }
    copy_region.bufferRowLength = level_host_layout.x_pitch_blocks * host_block_width;
    copy_region.bufferImageHeight = level_host_layout.y_pitch_blocks * host_block_height;
    copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.imageSubresource.mipLevel = level;
    copy_region.imageSubresource.baseArrayLayer = 0;
    copy_region.imageSubresource.layerCount = array_size;
    copy_region.imageExtent.width =
        std::min(std::max(width >> level, uint32_t(1)), copy_region.bufferRowLength);
    copy_region.imageExtent.height =
        std::min(std::max(height >> level, uint32_t(1)), copy_region.bufferImageHeight);
    copy_region.imageExtent.depth = std::max(depth >> level, uint32_t(1));
  }
  command_buffer->CmdVkCopyBufferToImage(scratch_buffer_, native_texture.image(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy_region_count,
                             copy_regions.data());
  if (before_draws) {
    submitter_.EndBeforeRenderPass();
  }
  if (profile_transfers_) {
    ++resident_load_stats_.recorded;
    const uint32_t parts = uint32_t(load_base) | (uint32_t(load_mips) << 1);
    const auto& previous = native_texture.resident_watch_snapshot;
    const uint64_t base_cpu = load_base ? watch_snapshot.base_cpu - previous.base_cpu : 0;
    const uint64_t base_gpu = load_base ? watch_snapshot.base_gpu - previous.base_gpu : 0;
    const uint64_t mips_cpu = load_mips ? watch_snapshot.mips_cpu - previous.mips_cpu : 0;
    const uint64_t mips_gpu = load_mips ? watch_snapshot.mips_gpu - previous.mips_gpu : 0;
    const bool cpu = (load_base && base_cpu) || (load_mips && mips_cpu);
    const bool gpu = (load_base && base_gpu) || (load_mips && mips_gpu);
    const uint64_t bytes = (load_base ? uint64_t(texture.GetGuestBaseSize()) : 0) +
                           (load_mips ? uint64_t(texture.GetGuestMipsSize()) : 0);
    auto accumulate = [&](ResidentDetail& detail) {
      ++detail.recorded;
      detail.bytes += bytes;
      if (native_texture.resident_load_count) {
        if (parts & native_texture.resident_loaded_parts) ++detail.repeat_parts;
        else ++detail.new_parts_only;
      }
      if (cpu && gpu) ++detail.both;
      else if (cpu) ++detail.cpu;
      else if (gpu) ++detail.gpu;
      else ++detail.no_watch;
      detail.base_cpu += base_cpu;
      detail.base_gpu += base_gpu;
      detail.mips_cpu += mips_cpu;
      detail.mips_gpu += mips_gpu;
    };
    accumulate(resident_detail_total_);
    auto found = resident_details_.find(texture_key);
    if (found == resident_details_.end() && resident_details_.size() < 128) {
      found = resident_details_.try_emplace(texture_key).first;
    }
    if (found != resident_details_.end()) accumulate(found->second);
    else ++resident_detail_overflow_;
    // Do not consume a notification for a part this recording didn't load.
    if (load_base) {
      native_texture.resident_watch_snapshot.base_cpu = watch_snapshot.base_cpu;
      native_texture.resident_watch_snapshot.base_gpu = watch_snapshot.base_gpu;
    }
    if (load_mips) {
      native_texture.resident_watch_snapshot.mips_cpu = watch_snapshot.mips_cpu;
      native_texture.resident_watch_snapshot.mips_gpu = watch_snapshot.mips_gpu;
    }
    native_texture.resident_loaded_parts |= parts;
    if (native_texture.resident_load_count) ++resident_load_stats_.reload;
    else ++resident_load_stats_.initial;
    if (native_texture.resident_load_count &&
        native_texture.resident_load_submission == submitter_.current()) {
      ++resident_load_stats_.same_submission;
    }
    resident_load_stats_.base += load_base;
    resident_load_stats_.mips += load_mips;
    resident_load_stats_.guest_bytes += (load_base ? texture.GetGuestBaseSize() : 0) +
                                      uint64_t(load_mips ? texture.GetGuestMipsSize() : 0);
    ++native_texture.resident_load_count;
    native_texture.resident_load_submission = submitter_.current();
  }
  return true;
}

void NativeTextureCache::LogResidentLoadDetails(bool emit) {
  if (!profile_transfers_) return;
  if (emit) {
    auto log = [](const char* label, const ResidentDetail& d) {
      REXGPU_INFO("Native resident detail {}: recorded {}, guest bytes {}, repeat parts {}, "
                  "new parts only {}; requested-part watch class: CPU {}, GPU {}, both {}, "
                  "none {}; requested-part callbacks: base CPU {}, base GPU {}, mips CPU {}, mips GPU {}",
                  label, d.recorded, d.bytes, d.repeat_parts, d.new_parts_only,
                  d.cpu, d.gpu, d.both, d.no_watch,
                  d.base_cpu, d.base_gpu, d.mips_cpu, d.mips_gpu);
    };
    log("interval total", resident_detail_total_);
    std::array<decltype(resident_details_)::const_iterator, 128> ranked;
    size_t count = 0;
    for (auto it = resident_details_.cbegin(); it != resident_details_.cend(); ++it) {
      ranked[count++] = it;
    }
    std::sort(ranked.begin(), ranked.begin() + count, [](auto a, auto b) {
      if (a->second.bytes != b->second.bytes) return a->second.bytes > b->second.bytes;
      return std::memcmp(&a->first, &b->first, sizeof(TextureKey)) < 0;
    });
    REXGPU_INFO("Native resident detail coverage: tracked keys {}, overflow loads {}; "
                "top 8 by guest bytes within admitted keys (interval counts, not per frame)",
                count, resident_detail_overflow_);
    for (size_t i = 0; i < std::min(count, size_t(8)); ++i) {
      const auto& key = ranked[i]->first;
      const auto label = fmt::format(
          "base {:08X} mips {:08X} {}x{}x{} format {} dim {} pitch {} mipmax {} "
          "tiled {} packed {} signed {} scaled {} endian {}",
          uint32_t(key.base_page) << 12, uint32_t(key.mip_page) << 12,
          key.GetWidth(), key.GetHeight(), key.GetDepthOrArraySize(), uint32_t(key.format),
          uint32_t(key.dimension), uint32_t(key.pitch), uint32_t(key.mip_max_level),
          uint32_t(key.tiled), uint32_t(key.packed_mips), uint32_t(key.signed_separate),
          uint32_t(key.scaled_resolve), uint32_t(key.endianness));
      log(label.c_str(), ranked[i]->second);
    }
  }
  resident_detail_total_ = {};
  resident_details_.clear();
  resident_detail_overflow_ = 0;
}

void NativeTextureCache::RequestTextures(uint32_t used_texture_mask) {
  TextureCache::RequestTextures(used_texture_mask);
  // The used textures sampled: a barrier only for those that aren't yet.
  CommandRecorder* command_buffer = nullptr;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    textures_remaining &= ~(uint32_t(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding) {
      continue;
    }
    for (Texture* texture : {binding->texture, binding->texture_signed}) {
      if (!texture) {
        continue;
      }
      auto* native_texture = static_cast<NativeTexture*>(texture);
      // Referenced by the coming draw.
      native_texture->MarkAsUsed();
      if (native_texture->usage() == NativeTexture::Usage::kSampled) {
        continue;
      }
      if (CanLoadBeforeDraws(*native_texture)) {
        TransitionTexture(submitter_.BeginBeforeRenderPass(GpuKind::kTextureLoads),
                          *native_texture, NativeTexture::Usage::kSampled);
        submitter_.EndBeforeRenderPass();
        continue;
      }
      if (command_buffer == nullptr) {
        command_buffer = submitter_.OpenOutsideRenderPass(RenderPassEnd::kTextureLoad);
        if (command_buffer == nullptr) {
          return;
        }
      }
      TransitionTexture(command_buffer, *native_texture, NativeTexture::Usage::kSampled);
    }
  }
}

bool NativeTextureCache::CanLoadBeforeDraws(const NativeTexture& texture) const {
  return REXCVAR_GET(native_hoist_texture_loads) &&
         texture.render_pass_sampled_serial != submitter_.render_pass_serial() &&
         native_shared_memory_.IsReadOnly() && submitter_.can_record_before_render_pass();
}

void NativeTextureCache::MarkSampledInRenderPass(uint32_t used_texture_mask) {
  const uint64_t serial = submitter_.render_pass_serial();
  uint32_t index;
  while (rex::bit_scan_forward(used_texture_mask, &index)) {
    used_texture_mask &= ~(uint32_t(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding) {
      continue;
    }
    for (Texture* texture : {binding->texture, binding->texture_signed}) {
      if (!texture) {
        continue;
      }
      auto* native_texture = static_cast<NativeTexture*>(texture);
      native_texture->render_pass_sampled_serial = serial;
      if (NativeTexture* texture_2d = native_texture->texture_3d_as_2d()) {
        texture_2d->render_pass_sampled_serial = serial;
      }
    }
  }
}

void NativeTextureCache::Request3DAs2DTextures(uint32_t fetched_as_2d_mask) {
  if (!REXCVAR_GET(gpu_3d_to_2d_texture)) {
    return;
  }
  uint32_t textures_remaining = fetched_as_2d_mask;
  uint32_t index;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    textures_remaining &= ~(uint32_t(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding || binding->key.dimension != xenos::DataDimension::k3D) {
      continue;
    }
    for (Texture* texture : {binding->texture, binding->texture_signed}) {
      if (!texture) {
        continue;
      }
      // The copy is destroyed with its 3D texture, which RequestTextures has
      // marked as used.
      NativeTexture* texture_2d = static_cast<NativeTexture*>(texture)->Get3DAs2D();
      if (!texture_2d || texture_2d->usage() == NativeTexture::Usage::kSampled) {
        continue;
      }
      if (CanLoadBeforeDraws(*texture_2d)) {
        TransitionTexture(submitter_.BeginBeforeRenderPass(GpuKind::kTextureLoads),
                          *texture_2d, NativeTexture::Usage::kSampled);
        submitter_.EndBeforeRenderPass();
        continue;
      }
      CommandRecorder* command_buffer =
          submitter_.OpenOutsideRenderPass(RenderPassEnd::kTextureLoad);
      if (command_buffer == nullptr) {
        return;
      }
      TransitionTexture(command_buffer, *texture_2d, NativeTexture::Usage::kSampled);
    }
  }
}

VkImageView NativeTextureCache::RequestSwapTexture(uint32_t& width_out, uint32_t& height_out,
                                                   xenos::TextureFormat& format_out) {
  xenos::xe_gpu_texture_fetch_t fetch = register_file().GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 || key.dimension != xenos::DataDimension::k2DOrStacked) {
    return VK_NULL_HANDLE;
  }
  auto* texture = static_cast<NativeTexture*>(FindOrCreateTexture(key));
  if (!texture || !LoadTextureData(*texture)) {
    return VK_NULL_HANDLE;
  }
  VkImageView view =
      texture->GetView(false, GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(key)), false);
  if (view == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  // Referenced by the present pass.
  texture->MarkAsUsed();
  if (texture->usage() != NativeTexture::Usage::kSampled) {
    CommandRecorder* command_buffer = submitter_.OpenOutsideRenderPass(RenderPassEnd::kTextureLoad);
    if (command_buffer == nullptr) {
      return VK_NULL_HANDLE;
    }
    TransitionTexture(command_buffer, *texture, NativeTexture::Usage::kSampled);
  }
  width_out = key.GetWidth();
  height_out = key.GetHeight();
  format_out = key.format;
  return view;
}

TextureCache::Texture* NativeTextureCache::FindResolveDestinationTexture(
    const ResolveDestination& destination) {
  const uint32_t width = destination.width;
  const uint32_t height = destination.height;
  if (!width || !height || (destination.base & 0xFFF) || !destination.pitch_texels ||
      (destination.pitch_texels & 31) || width > destination.pitch_texels ||
      width > (UINT32_C(1) << 13) || height > (UINT32_C(1) << 13)) {
    return nullptr;
  }
  // The key a fetch constant gives for the surface (BindingInfoFromFetchConstant
  // with one level: no mip page); packed mips may be either.
  TextureKey key;
  key.base_page = (destination.base & 0x1FFFFFFF) >> 12;
  key.dimension = xenos::DataDimension::k2DOrStacked;
  key.tiled = 1;
  key.pitch = destination.pitch_texels >> 5;
  key.format = destination.format;
  key.endianness = destination.endianness;
  key.is_valid = 1;
  auto find_at_size = [&](uint32_t texture_width, uint32_t texture_height) -> Texture* {
    key.width_minus_1 = texture_width - 1;
    key.height_minus_1 = texture_height - 1;
    for (uint32_t packed_mips = 0; packed_mips <= 1; ++packed_mips) {
      key.packed_mips = packed_mips;
      if (Texture* texture = FindExistingTexture(key)) {
        return texture;
      }
    }
    return nullptr;
  };
  Texture* found = find_at_size(width, height);
  if (found) {
    return found;
  }
  // The surface's own size, which the resolve rounded up to whole 8x8 blocks,
  // can be up to 7 texels smaller each way: the size found last time first,
  // else each of them (not again while no texture has been created since none
  // was found).
  constexpr uint32_t kBlock = xenos::kResolveAlignmentPixels;
  // Every field whole: 17-bit page, 11-bit block counts, 9-bit pitch, 6-bit
  // format, 2-bit endianness, and a bit for "used".
  const uint64_t resolve_id = (uint64_t(1) << 63) | (uint64_t(key.base_page) << 39) |
                              (uint64_t((width + kBlock - 1) / kBlock) << 28) |
                              (uint64_t((height + kBlock - 1) / kBlock) << 17) |
                              (uint64_t(key.pitch) << 8) | (uint64_t(key.format) << 2) |
                              uint64_t(key.endianness);
  ResolvedSurfaceSize& last =
      resolved_surface_sizes_[(resolve_id * UINT64_C(0x9E3779B97F4A7C15)) >> 58];
  const bool same_resolve = last.resolve == resolve_id;
  if (same_resolve && last.width) {
    found = find_at_size(last.width, last.height);
  }
  if (!found && !(same_resolve && !last.width && last.textures_created == textures_created())) {
    for (uint32_t narrower = 0; narrower < kBlock && narrower < width && !found; ++narrower) {
      for (uint32_t shorter = narrower ? 0 : 1; shorter < kBlock && shorter < height && !found;
           ++shorter) {
        found = find_at_size(width - narrower, height - shorter);
      }
    }
    last.resolve = resolve_id;
    last.width = found ? key.GetWidth() : 0;
    last.height = found ? key.GetHeight() : 0;
    last.textures_created = textures_created();
  }
  return found;
}

void NativeTextureCache::RebindTexture(const Texture& texture) {
  uint32_t fetch_constant_mask = 0;
  for (uint32_t i = 0; i < xenos::kTextureFetchConstantCount; ++i) {
    const TextureBinding* binding = GetValidTextureBinding(i);
    if (binding && (binding->texture == &texture || binding->texture_signed == &texture)) {
      fetch_constant_mask |= uint32_t(1) << i;
    }
  }
  if (fetch_constant_mask) {
    UpdateTextureBindingsImpl(fetch_constant_mask);
  }
}

bool NativeTextureCache::HasResolveDestinationTexture(const ResolveDestination& destination) {
  return FindResolveDestinationTexture(destination) != nullptr;
}

NativeTextureCache::ResolveCopy NativeTextureCache::CopyResolveIntoTexture(
    CommandRecorder* command_buffer, const ResolveDestination& destination, VkImage source_image,
    VkFormat source_format, bool source_is_depth, bool red_blue_swapped) {
  Texture* found = FindResolveDestinationTexture(destination);
  if (!found) {
    return ResolveCopy::kNoTexture;
  }
  if (found->is_replacement() || found->GetGuestMipsSize() || !found->GetGuestBaseSize()) {
    return ResolveCopy::kFormat;
  }
  // Only a format loaded as a plain copy of its texels holds what the image
  // does, bit for bit; depth as the floats the depth loads make of it.
  const TextureKey found_key = found->key();
  const HostFormat& host_format = GetHostFormatPair(found_key).format_unsigned;
  bool plain;
  if (source_is_depth) {
    plain = (found_key.format == xenos::TextureFormat::k_24_8 &&
             host_format.load_shader == kLoadShaderIndexDepthUnorm) ||
            (found_key.format == xenos::TextureFormat::k_24_8_FLOAT &&
             host_format.load_shader == kLoadShaderIndexDepthFloat);
  } else {
    switch (host_format.load_shader) {
      case kLoadShaderIndex8bpb:
      case kLoadShaderIndex16bpb:
      case kLoadShaderIndex32bpb:
      case kLoadShaderIndex64bpb:
      case kLoadShaderIndex128bpb:
        plain = true;
        break;
      default:
        plain = false;
        break;
    }
  }
  if (!plain || host_format.block_compressed || host_format.format != source_format) {
    return ResolveCopy::kFormat;
  }
  // Red and blue are swapped back by the views, which some devices can't do
  // (portability subset; the shader swizzles there, without the swap).
  if (red_blue_swapped && !submitter_.vulkan_device()->properties().imageViewFormatSwizzle) {
    return ResolveCopy::kSwap;
  }
  auto& texture = *static_cast<NativeTexture*>(found);
  // Referenced by this submission (not destroyed before the copy is done).
  texture.MarkAsUsed();
  // After its earlier uses (sampling, or a load into it).
  TransitionTexture(command_buffer, texture, NativeTexture::Usage::kTransferDestination);
  VkImageCopy region = {};
  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource = region.srcSubresource;
  region.extent = {found_key.GetWidth(), found_key.GetHeight(), 1};
  command_buffer->CmdVkCopyImage(source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 texture.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  // The resolve has written the same texels to the guest memory (with red and
  // blue swapped if it swapped them), which outdated the texture; it now holds
  // them. Bindings with views made for the other order are looked up again.
  if (texture.red_blue_swapped() != red_blue_swapped) {
    texture.set_red_blue_swapped(red_blue_swapped);
    RebindTexture(texture);
  }
  if (!MarkTextureUpToDate(texture)) {
    return ResolveCopy::kNotMarked;
  }
  if (!(logged_ & kLogResolveIntoTextures)) {
    logged_ |= kLogResolveIntoTextures;
    REXGPU_INFO(
        "Native renderer: resolves copied straight into their textures "
        "(native_resolve_into_textures), the first {}x{} {} at {:08X}",
        found_key.GetWidth(), found_key.GetHeight(), FormatInfo::Get(found_key.format)->name,
        destination.base);
  }
  return ResolveCopy::kCopied;
}

void NativeTextureCache::UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) {
  ++binding_generation_;
  uint32_t bindings_remaining = fetch_constant_mask;
  uint32_t binding_index;
  while (rex::bit_scan_forward(bindings_remaining, &binding_index)) {
    bindings_remaining &= ~(uint32_t(1) << binding_index);
    ++binding_generations_[binding_index];
    BindingViews& views = binding_views_[binding_index];
    views = BindingViews();
    const TextureBinding* binding = GetValidTextureBinding(binding_index);
    if (!binding) {
      continue;
    }
    const HostFormatPair& host_format_pair = GetHostFormatPair(binding->key);
    bool uses_unsigned = texture_util::IsAnySignNotSigned(binding->swizzled_signs);
    bool uses_signed = texture_util::IsAnySignSigned(binding->swizzled_signs);
    // The signed data in its own image, or both signednesses in one.
    auto* texture_unsigned = static_cast<NativeTexture*>(binding->texture);
    auto* texture_signed = IsSignedVersionSeparateForFormat(binding->key)
                               ? static_cast<NativeTexture*>(binding->texture_signed)
                               : texture_unsigned;
    if (texture_unsigned && uses_unsigned &&
        host_format_pair.format_unsigned.format != VK_FORMAT_UNDEFINED) {
      views.view_unsigned = texture_unsigned->GetView(false, binding->host_swizzle);
    }
    if (texture_signed && uses_signed &&
        host_format_pair.format_signed.format != VK_FORMAT_UNDEFINED) {
      views.view_signed = texture_signed->GetView(true, binding->host_swizzle);
    }
  }
}

VkImageView NativeTextureCache::GetActiveBindingImageView(uint32_t fetch_constant_index,
                                                          xenos::FetchOpDimension dimension,
                                                          bool is_signed) {
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (!binding) {
    return VK_NULL_HANDLE;
  }
  xenos::DataDimension resource_dimension = binding->key.dimension;
  bool compatible;
  switch (dimension) {
    case xenos::FetchOpDimension::k1D:
    case xenos::FetchOpDimension::k2D:
      if (resource_dimension == xenos::DataDimension::k3D) {
        // Its 2D copy (Request3DAs2DTextures), signed as the SDK's backend
        // chooses it.
        bool use_signed = texture_util::IsSignedViewBound(binding->swizzled_signs, is_signed) &&
                          texture_util::IsAnySignSigned(binding->swizzled_signs);
        Texture* texture = use_signed && IsSignedVersionSeparateForFormat(binding->key)
                               ? binding->texture_signed
                               : binding->texture;
        NativeTexture* texture_2d =
            texture ? static_cast<NativeTexture*>(texture)->texture_3d_as_2d() : nullptr;
        if (!texture_2d || texture_2d->usage() != NativeTexture::Usage::kSampled) {
          return VK_NULL_HANDLE;
        }
        return texture_2d->GetView(use_signed, binding->host_swizzle);
      }
      compatible = resource_dimension == xenos::DataDimension::k1D ||
                   resource_dimension == xenos::DataDimension::k2DOrStacked;
      break;
    case xenos::FetchOpDimension::k3DOrStacked:
      compatible = resource_dimension == xenos::DataDimension::k3D;
      break;
    case xenos::FetchOpDimension::kCube:
      compatible = resource_dimension == xenos::DataDimension::kCube;
      break;
    default:
      compatible = false;
      break;
  }
  if (!compatible) {
    return VK_NULL_HANDLE;
  }
  const BindingViews& views = binding_views_[fetch_constant_index];
  return texture_util::IsSignedViewBound(binding->swizzled_signs, is_signed) ? views.view_signed
                                                                             : views.view_unsigned;
}

bool NativeTextureCache::GetActiveBoundTexture2D(uint32_t fetch_constant_index,
                                                 BoundTexture2D& out) {
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (!binding || !binding->texture) {
    return false;
  }
  const TextureKey& key = binding->key;
  if (key.dimension != xenos::DataDimension::k2DOrStacked || key.depth_or_array_size_minus_1) {
    return false;
  }
  auto* texture = static_cast<NativeTexture*>(binding->texture);
  // The identity view is in the guest's red / blue order only when the image
  // is (resolves are copied in swapped at 1x only, where this isn't used).
  if (texture->usage() != NativeTexture::Usage::kSampled || texture->red_blue_swapped()) {
    return false;
  }
  const HostFormatPair& host_format_pair = GetHostFormatPair(key);
  VkFormat format = host_format_pair.format_unsigned.format;
  if (format == VK_FORMAT_UNDEFINED) {
    return false;
  }
  // Signed only where the signed view is the unsigned one (floats, whose sign
  // the guest ignores: Perfect Dark Zero samples its 16-bit float scene as
  // signed); else the signed view is another format (SNORM).
  if (texture_util::IsAnySignSigned(binding->swizzled_signs) &&
      (!host_format_pair.unsigned_signed_compatible ||
       host_format_pair.format_signed.format != format)) {
    return false;
  }
  VkImageView view = texture->GetView(false, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, true);
  if (view == VK_NULL_HANDLE) {
    return false;
  }
  out.key = key;
  out.format = format;
  out.identity_view = view;
  out.guest_size = texture->GetGuestBaseSize();
  return true;
}

xenos::ClampMode NativeTextureCache::NormalizeClampMode(xenos::ClampMode clamp_mode) const {
  if (clamp_mode == xenos::ClampMode::kClampToHalfway) {
    // No GL_CLAMP equivalent in Vulkan (and none in Direct3D 9).
    return xenos::ClampMode::kClampToEdge;
  }
  if (clamp_mode == xenos::ClampMode::kMirrorClampToEdge ||
      clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
      clamp_mode == xenos::ClampMode::kMirrorClampToBorder) {
    return submitter_.vulkan_device()->properties().samplerMirrorClampToEdge
               ? xenos::ClampMode::kMirrorClampToEdge
               : xenos::ClampMode::kMirroredRepeat;
  }
  return clamp_mode;
}

NativeTextureCache::SamplerParameters NativeTextureCache::GetSamplerParameters(
    const SpirvShader::SamplerBinding& binding) const {
  const RegisterFile& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
  SamplerParameters parameters;

  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  texture_util::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y, fetch_clamp_z);
  parameters.clamp_x = NormalizeClampMode(fetch_clamp_x);
  parameters.clamp_y = NormalizeClampMode(fetch_clamp_y);
  parameters.clamp_z = NormalizeClampMode(fetch_clamp_z);
  if (xenos::ClampModeUsesBorder(parameters.clamp_x) ||
      xenos::ClampModeUsesBorder(parameters.clamp_y) ||
      xenos::ClampModeUsesBorder(parameters.clamp_z)) {
    if (binding.border_color_forced) {
      // getBCF's black and white border samplers.
      parameters.border_color = binding.forced_border_color;
    } else {
      parameters.border_color = fetch.border_color;
      parameters.force_bc_w_to_max = fetch.force_bc_w_to_max;
    }
  } else {
    parameters.border_color = xenos::BorderColor::k_ABGR_Black;
  }

  xenos::TextureFilter mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding.mag_filter;
  parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
  xenos::TextureFilter min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding.min_filter;
  parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
  xenos::TextureFilter mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding.mip_filter;
  parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  xenos::AnisoFilter aniso_filter = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                        ? fetch.aniso_filter
                                        : binding.aniso_filter;
  parameters.mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;

  uint32_t base_page, mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &base_page,
                                                 nullptr, &mip_min_level, &mip_max_level);
  if (parameters.mip_base_map && base_page != 0) {
    mip_min_level = 0;
  }
  parameters.mip_min_level = mip_min_level;
  bool has_mips = mip_max_level > mip_min_level;
  // anisotropic_override, for mipmapped bilinear or trilinear textures only.
  int32_t anisotropic_override = REXCVAR_GET(anisotropic_override);
  if (anisotropic_override > -1 && anisotropic_override < 6 && has_mips &&
      !parameters.mip_base_map && parameters.mag_linear && parameters.min_linear &&
      (mip_filter == xenos::TextureFilter::kPoint || mip_filter == xenos::TextureFilter::kLinear)) {
    aniso_filter = xenos::AnisoFilter(anisotropic_override);
  }
  parameters.aniso_filter = std::min(aniso_filter, max_anisotropy_);

  // Point sampling where the host can't filter the texture's formats
  // (anisotropic filtering is linear filtering too).
  if (parameters.mag_linear || parameters.min_linear || parameters.mip_linear ||
      parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    bool linear_filterable = false;
    TextureKey texture_key;
    uint8_t texture_swizzled_signs;
    BindingInfoFromFetchConstant(fetch, texture_key, &texture_swizzled_signs);
    if (texture_key.is_valid) {
      const HostFormatPair& host_format_pair = GetHostFormatPair(texture_key);
      linear_filterable = (!texture_util::IsAnySignNotSigned(texture_swizzled_signs) ||
                           host_format_pair.format_unsigned.linear_filterable) &&
                          (!texture_util::IsAnySignSigned(texture_swizzled_signs) ||
                           host_format_pair.format_signed.linear_filterable);
    }
    if (!linear_filterable) {
      parameters.mag_linear = 0;
      parameters.min_linear = 0;
      parameters.mip_linear = 0;
      parameters.aniso_filter = xenos::AnisoFilter::kDisabled;
    }
  }
  return parameters;
}

namespace {
// The fields of a sampler binding GetSamplerParameters reads (UINT32_MAX is
// never one).
uint32_t SamplerMemoBinding(const SpirvShader::SamplerBinding& binding) {
  return uint32_t(binding.mag_filter) | (uint32_t(binding.min_filter) << 2) |
         (uint32_t(binding.mip_filter) << 4) | (uint32_t(binding.aniso_filter) << 6) |
         (uint32_t(binding.border_color_forced) << 9) |
         (uint32_t(binding.forced_border_color) << 10);
}
}  // namespace

VkSampler NativeTextureCache::GetSampler(const SpirvShader::SamplerBinding& binding) {
  // Bits 12:31 of dwords 1 and 5 are the base and mip addresses >> 12, read
  // (GetSubresourcesFromFetchConstant, 17 bits of each) only as whether each
  // is zero and whether they are equal: those bits stand in their place.
  const uint32_t* registers =
      &register_file().values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + binding.fetch_constant * 6];
  uint32_t fetch_words[6];
  std::memcpy(fetch_words, registers, sizeof(fetch_words));
  const uint32_t base_page = (fetch_words[1] >> 12) & 0x1FFFF;
  const uint32_t mip_page = (fetch_words[5] >> 12) & 0x1FFFF;
  fetch_words[1] = (fetch_words[1] & 0xFFFu) | (uint32_t(base_page != 0) << 12) |
                   (uint32_t(base_page == mip_page) << 13);
  fetch_words[5] = (fetch_words[5] & 0xFFFu) | (uint32_t(mip_page != 0) << 12);
  const uint32_t binding_fields = SamplerMemoBinding(binding);
  const int32_t anisotropic_override = REXCVAR_GET(anisotropic_override);
  SamplerMemo& memo = sampler_memos_[binding.fetch_constant];
  if (memo.binding == binding_fields && memo.anisotropic_override == anisotropic_override &&
      !std::memcmp(memo.fetch, fetch_words, sizeof(memo.fetch))) {
    return memo.sampler;
  }
  VkSampler sampler = UseSampler(GetSamplerParameters(binding));
  std::memcpy(memo.fetch, fetch_words, sizeof(memo.fetch));
  memo.binding = binding_fields;
  memo.anisotropic_override = anisotropic_override;
  memo.sampler = sampler;
  return sampler;
}

VkSampler NativeTextureCache::UseSampler(SamplerParameters parameters) {
  auto it = samplers_.find(parameters.value);
  if (it != samplers_.end()) {
    return it->second;
  }
  if (samplers_.size() >= sampler_max_count_) {
    if (!(logged_ & kLogSamplerCount)) {
      logged_ |= kLogSamplerCount;
      REXGPU_WARN("Native renderer: the device's {} samplers are used up, more use a default one",
                  sampler_max_count_);
    }
    return VK_NULL_HANDLE;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_.vulkan_device();
  const ui::vulkan::VulkanDevice::Properties& properties = vulkan_device->properties();
  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  if (properties.nonSeamlessCubeMap && REXCVAR_GET(non_seamless_cube_map)) {
    sampler_info.flags |= VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT;
  }
  sampler_info.magFilter = parameters.mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sampler_info.minFilter = parameters.min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sampler_info.mipmapMode =
      parameters.mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  static const VkSamplerAddressMode kAddressModes[] = {
      VK_SAMPLER_ADDRESS_MODE_REPEAT,                // kRepeat
      VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,       // kMirroredRepeat
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,         // kClampToEdge
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,  // kMirrorClampToEdge
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,         // kClampToHalfway
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,  // kMirrorClampToHalfway
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,       // kClampToBorder
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,  // kMirrorClampToBorder
  };
  sampler_info.addressModeU = kAddressModes[uint32_t(parameters.clamp_x) & 7];
  sampler_info.addressModeV = kAddressModes[uint32_t(parameters.clamp_y) & 7];
  sampler_info.addressModeW = kAddressModes[uint32_t(parameters.clamp_z) & 7];
  // The LOD bias is applied in the shaders.
  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    sampler_info.anisotropyEnable = VK_TRUE;
    sampler_info.maxAnisotropy = float(
        uint32_t(1) << (uint32_t(parameters.aniso_filter) - uint32_t(xenos::AnisoFilter::kMax_1_1)));
  }
  sampler_info.minLod = float(parameters.mip_min_level);
  sampler_info.maxLod =
      parameters.mip_base_map ? sampler_info.minLod + 0.25f : VK_LOD_CLAMP_NONE;
  VkSamplerCustomBorderColorCreateInfoEXT custom_border_color_info = {};
  bool uses_custom_border_color = false;
  switch (parameters.border_color) {
    case xenos::BorderColor::k_ABGR_White:
      sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
    case xenos::BorderColor::k_ACBCRY_Black:
      if (properties.customBorderColors &&
          custom_border_color_sampler_count_ < properties.maxCustomBorderColorSamplers) {
        sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        custom_border_color_info.sType =
            VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
        custom_border_color_info.customBorderColor.float32[0] =
            parameters.border_color == xenos::BorderColor::k_ACBYCR_Black ? 0.5f : 0.0f;
        custom_border_color_info.customBorderColor.float32[1] =
            parameters.border_color == xenos::BorderColor::k_ACBYCR_Black ? 0.0f : 0.5f;
        custom_border_color_info.customBorderColor.float32[2] = 0.5f;
        custom_border_color_info.customBorderColor.float32[3] =
            parameters.force_bc_w_to_max ? 1.0f : 0.0f;
        custom_border_color_info.format = properties.customBorderColorWithoutFormat
                                              ? VK_FORMAT_UNDEFINED
                                              : VK_FORMAT_R32G32B32A32_SFLOAT;
        custom_border_color_info.pNext = sampler_info.pNext;
        sampler_info.pNext = &custom_border_color_info;
        uses_custom_border_color = true;
      } else {
        sampler_info.borderColor = parameters.force_bc_w_to_max
                                       ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                       : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
      }
      break;
    default:
      sampler_info.borderColor = parameters.force_bc_w_to_max
                                     ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                     : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
      break;
  }
  VkSampler sampler;
  if (vulkan_device->functions().vkCreateSampler(vulkan_device->device(), &sampler_info, nullptr,
                                                 &sampler) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a sampler (parameters {:08X})",
                 parameters.value);
    return VK_NULL_HANDLE;
  }
  if (uses_custom_border_color) {
    ++custom_border_color_sampler_count_;
  }
  samplers_.emplace(parameters.value, sampler);
  return sampler;
}

}  // namespace rex::graphics::native
