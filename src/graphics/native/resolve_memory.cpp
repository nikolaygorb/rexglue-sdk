/**
 * @file        graphics/native/resolve_memory.cpp
 * @brief       Native renderer: resolves written into guest memory (the shared memory)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The SDK's resolve copy shaders and their use in VulkanRenderTargetCache::Resolve
 *              (Xenia, Copyright 2022 Ben Vanik, BSD license); the EDRAM layout of
 *              shaders/edram.xesli (XeEdramOffsetBytes).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/resolve_memory.h>

#include <algorithm>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/native/edram_tiles.h>
#include <rex/graphics/native/render_targets.h>
#include <rex/graphics/native/shared_memory.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/vulkan/glsl_compile.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(native_resolve_dump_samples, true, "GPU",
                    "Native renderer: a resolve's dump of the render target into the EDRAM "
                    "buffer writes only the samples its copy reads (its sample selection: one "
                    "sample of a multisampled surface, or the pair it averages); off: all "
                    "samples, as before")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::native {

// The SDK's resolve copy shaders (`xb buildshaders`), unscaled.
namespace shaders {
#include "../shaders/vulkan_spirv/resolve_dxt3aas1111_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_4xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_4xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_128bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_16bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_32bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_64bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_8bpp_cs.h"
}  // namespace shaders

namespace util = ui::vulkan::util;

namespace {

enum LogReason : uint32_t {
  kLogSetup = 1u << 0,
  kLogDescriptors = 1u << 1,
  kLogShader = 1u << 2,
  kLogDestMemory = 1u << 3,
};

struct CopyShaderCode {
  const uint32_t* code;
  size_t size;
};
// By draw_util::ResolveCopyShaderIndex.
const CopyShaderCode kCopyShaders[size_t(draw_util::ResolveCopyShaderIndex::kCount)] = {
    {shaders::resolve_fast_32bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_cs)},
    {shaders::resolve_fast_32bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_4xmsaa_cs)},
    {shaders::resolve_fast_64bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_cs)},
    {shaders::resolve_fast_64bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_4xmsaa_cs)},
    {shaders::resolve_full_8bpp_cs, sizeof(shaders::resolve_full_8bpp_cs)},
    {shaders::resolve_full_16bpp_cs, sizeof(shaders::resolve_full_16bpp_cs)},
    {shaders::resolve_full_32bpp_cs, sizeof(shaders::resolve_full_32bpp_cs)},
    {shaders::resolve_full_64bpp_cs, sizeof(shaders::resolve_full_64bpp_cs)},
    {shaders::resolve_full_128bpp_cs, sizeof(shaders::resolve_full_128bpp_cs)},
    {shaders::resolve_dxt3aas1111_cs, sizeof(shaders::resolve_dxt3aas1111_cs)},
};

constexpr VkDeviceSize kEdramBufferSize =
    VkDeviceSize(xenos::kEdramTileCount) * xenos::kEdramTileWidthSamples *
    xenos::kEdramTileHeightSamples * sizeof(uint32_t);

constexpr uint32_t kSetsPerPool = 64;
constexpr uint32_t kTransientSets = 256;

// After "#version 460", the samplerless texture extension and SRC_DEPTH /
// SRC_MS (0 or 1): one invocation per sample of the resolve's EDRAM tiles, in
// the EDRAM's order (neighbouring invocations write neighbouring cells, as the
// SDK's DumpRenderTargets does), into the EDRAM buffer as XeEdramOffsetBytes
// lays it out (at scale 1), in the raw cells the resolve shaders unpack. Each
// cell is read from the dumped target where it holds that tile (its own base
// and pitch: the target can be another one than the resolve's, with its own
// MSAA), from the guest pixel and sample the cell is (XeEdramOffsetBytes
// backwards); cells the resolve's copy doesn't read (its sample selection, in
// the resolve's MSAA) are skipped. At a resolution scale, from the host pixel
// holding the guest pixel's centre (CenterPixel, the SDK's readback downscale
// picks one sample too).
constexpr char kDumpShaderBody[] = R"glsl(
layout(local_size_x = 16, local_size_y = 4, local_size_z = 1) in;

layout(push_constant) uniform Constants {
  // The resolve's tiles: span_base + row * span_pitch + column, for
  // span_rows rows of span_columns.
  uint span_base;
  uint span_pitch;
  uint span_columns;
  uint span_rows;
  // The dumped target.
  uint base_tiles;
  uint pitch_tiles;
  // xenos::MsaaSamples.
  uint msaa_samples;
  // Key xenos::ColorRenderTargetFormat, or DepthRenderTargetFormat.
  uint format;
  // 1 for 64bpp.
  uint format_ints_log2;
  // Its guest pixels.
  uint surface_width;
  uint surface_height;
  uint tile_count;
  // The resolve's xenos::MsaaSamples, and a bit per sample its copy reads.
  uint view_msaa_samples;
  uint sample_mask;
  // The target's scale in quarters, and its host size (the last pixel).
  uint scale4;
  uint host_max_x;
  uint host_max_y;
} c;

#if SRC_MS
#define SOURCE_TEXTURE texture2DMS
#define SOURCE_UTEXTURE utexture2DMS
#define FETCH(t, p, s) texelFetch(t, p, int(s))
#else
#define SOURCE_TEXTURE texture2D
#define SOURCE_UTEXTURE utexture2D
#define FETCH(t, p, s) texelFetch(t, p, 0)
#endif

#if SRC_DEPTH
layout(set = 0, binding = 0) uniform SOURCE_TEXTURE src_depth;
layout(set = 0, binding = 1) uniform SOURCE_UTEXTURE src_stencil;
#else
layout(set = 0, binding = 0) uniform SOURCE_TEXTURE src_color;
#endif

layout(set = 1, binding = 0, std430) buffer Edram {
  uint edram[];
};

uint Encode7e3(float f) {
  uint b = floatBitsToUint(clamp(f, 0.0, 31.875));
  uint biased = b < 0x3E800000u
                    ? ((b & 0x7FFFFFu) | 0x800000u) >> min(125u - (b >> 23), 24u)
                    : b + 0xC2000000u;
  return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
}

uint Encode20e4(float f) {
  uint b = floatBitsToUint(clamp(f, 0.0, 1.99999988));
  uint biased = b < 0x38800000u
                    ? ((b & 0x7FFFFFu) | 0x800000u) >> min(113u - (b >> 23), 24u)
                    : b + 0xC8000000u;
  return ((biased + 3u + ((biased >> 3) & 1u)) >> 3) & 0xFFFFFFu;
}

// 16-bit fixed surfaces hold value / 32 on the host: the EDRAM's int16 is
// value * 1024 (XeUnpackR16G16Edram), the host's snorm * 32767.
uint PackFixed16(vec2 v) {
  uvec2 s = uvec2(ivec2(round(clamp(v, -1.0, 1.0) * 32767.0))) & 0xFFFFu;
  return s.x | (s.y << 16);
}

uint PackColor32(vec4 color) {
  switch (c.format) {
    case 2u:
    case 10u: {
      uvec3 rgb = uvec3(round(clamp(color.rgb, 0.0, 1.0) * 1023.0));
      return rgb.r | (rgb.g << 10) | (rgb.b << 20) |
             (uint(round(clamp(color.a, 0.0, 1.0) * 3.0)) << 30);
    }
    case 3u:
    case 12u:
      return Encode7e3(color.r) | (Encode7e3(color.g) << 10) | (Encode7e3(color.b) << 20) |
             (uint(round(clamp(color.a, 0.0, 1.0) * 3.0)) << 30);
    case 4u:
      return PackFixed16(color.rg);
    case 6u:
      return packHalf2x16(color.rg);
    case 14u:
      return floatBitsToUint(color.r);
    default: {
      // k_8_8_8_8 (and its gamma variant's raw bytes).
      uvec4 b = uvec4(round(clamp(color, 0.0, 1.0) * 255.0));
      return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
    }
  }
}

uvec2 PackColor64(vec4 color) {
  switch (c.format) {
    case 5u:  // k_16_16_16_16
      return uvec2(PackFixed16(color.rg), PackFixed16(color.ba));
    case 7u:  // k_16_16_16_16_FLOAT
      return uvec2(packHalf2x16(color.rg), packHalf2x16(color.ba));
    default:  // k_32_32_FLOAT
      return uvec2(floatBitsToUint(color.r), floatBitsToUint(color.g));
  }
}

void main() {
  // The cell: a tile of the span, and its sample column and row there (80x16
  // samples, 40x16 for 64bpp).
  uvec2 index = gl_GlobalInvocationID.xy;
  uint tile_width = 80u >> c.format_ints_log2;
  uint span_column = index.x / tile_width;
  uint span_row = index.y >> 4u;
  if (span_column >= c.span_columns || span_row >= c.span_rows) {
    return;
  }
  uvec2 cell = uvec2(index.x - span_column * tile_width, index.y & 15u);
  // The resolve's sample index of the cell (bit 1 of the sample column and row
  // within the tile, which a depth tile's swapped halves keep: 40 is a
  // multiple of 4).
  uint view_sample = c.view_msaa_samples >= 2u ? (((cell.x >> 1u) & 1u) | (cell.y & 2u))
                     : c.view_msaa_samples == 1u ? ((cell.x >> 1u) & 1u)
                                                 : 0u;
  if (((c.sample_mask >> view_sample) & 1u) == 0u) {
    return;
  }
  uint tile = (c.span_base + span_row * c.span_pitch + span_column) % c.tile_count;
  // The tile's row and column in the target.
  uint relative = (tile + c.tile_count - c.base_tiles % c.tile_count) % c.tile_count;
  uint target_row = relative / c.pitch_tiles;
  uint target_column = relative - target_row * c.pitch_tiles;
  // The target's sample there (depth tiles have their halves swapped).
  uvec2 guest_sample = cell;
#if SRC_DEPTH
  uint tile_width_half = tile_width >> 1u;
  guest_sample.x = guest_sample.x >= tile_width_half ? guest_sample.x - tile_width_half
                                                     : guest_sample.x + tile_width_half;
#endif
  guest_sample += uvec2(target_column * tile_width, target_row * 16u);
  // Its pixel and sample index (XeEdramOffsetBytes' mapping backwards).
  uvec2 pixel;
  uint pixel_sample_index;
  if (c.msaa_samples >= 2u) {
    pixel = ((guest_sample >> 2u) << 1u) | (guest_sample & 1u);
    pixel_sample_index = ((guest_sample.x >> 1u) & 1u) | (guest_sample.y & 2u);
  } else if (c.msaa_samples == 1u) {
    pixel.x = (guest_sample.x & ~2u) | (guest_sample.y & 2u);
    pixel.y = ((guest_sample.y >> 2u) << 1u) | (guest_sample.y & 1u);
    pixel_sample_index = (guest_sample.x >> 1u) & 1u;
  } else {
    pixel = guest_sample;
    pixel_sample_index = 0u;
  }
  if (pixel.x >= c.surface_width || pixel.y >= c.surface_height) {
    return;
  }
  ivec2 coord = ivec2(min(((pixel * 2u + 1u) * c.scale4 - 1u) >> 3u,
                          uvec2(c.host_max_x, c.host_max_y)));
  // The host sample of guest sample s: 2x has the top one (guest 0) at host
  // sample 1 with the standard locations, as the EDRAM transfers read it.
  uint host_s = c.msaa_samples == 1u ? pixel_sample_index ^ 1u : pixel_sample_index;
  uint offset = tile * (80u * 16u) + ((cell.y * tile_width + cell.x) << c.format_ints_log2);
#if SRC_DEPTH
  // Float24 depth is held at half its value on the host (the draws'
  // viewport, GetHostViewportInfo full_float24_in_0_to_1).
  float depth = FETCH(src_depth, coord, host_s).r;
  uint depth24 =
      c.format == 1u ? Encode20e4(depth * 2.0) : uint(round(clamp(depth, 0.0, 1.0) * 16777215.0));
  edram[offset] = (depth24 << 8) | (FETCH(src_stencil, coord, host_s).r & 0xFFu);
#else
  vec4 color = FETCH(src_color, coord, host_s);
  if (c.format_ints_log2 != 0u) {
    uvec2 cells = PackColor64(color);
    edram[offset] = cells.x;
    edram[offset + 1u] = cells.y;
  } else {
    edram[offset] = PackColor32(color);
  }
#endif
}
)glsl";

// The dump's local size.
constexpr uint32_t kDumpGroupWidth = 16;
constexpr uint32_t kDumpGroupHeight = 4;

struct DumpConstants {
  uint32_t span_base;
  uint32_t span_pitch;
  uint32_t span_columns;
  uint32_t span_rows;
  uint32_t base_tiles;
  uint32_t pitch_tiles;
  uint32_t msaa_samples;
  uint32_t format;
  uint32_t format_ints_log2;
  uint32_t surface_width;
  uint32_t surface_height;
  uint32_t tile_count;
  uint32_t view_msaa_samples;
  uint32_t sample_mask;
  uint32_t scale4;
  uint32_t host_max_x;
  uint32_t host_max_y;
};

// The samples (a bit per sample index, in the resolve's MSAA) the SDK's copy
// shaders read for a sanitized sample selection: the first one, and with
// averaging the others of the pair or the four (XeResolveFirstSampleIndex,
// XeResolveColorCopySampleOffsetsBytes).
uint32_t CopySampleMask(xenos::CopySampleSelect select) {
  switch (select) {
    case xenos::CopySampleSelect::k0:
    case xenos::CopySampleSelect::k1:
    case xenos::CopySampleSelect::k2:
    case xenos::CopySampleSelect::k3:
      return 1u << uint32_t(select);
    case xenos::CopySampleSelect::k01:
      return 0b0011;
    case xenos::CopySampleSelect::k23:
      return 0b1100;
    default:
      return 0b1111;
  }
}

}  // namespace

ResolveMemory::~ResolveMemory() {
  Shutdown();
}

void ResolveMemory::LogOnce(uint32_t reason_bit, const char* message) {
  if (!(logged_ & reason_bit)) {
    logged_ |= reason_bit;
    REXGPU_WARN("Native renderer: {}", message);
  }
}

bool ResolveMemory::Initialize(Submitter& submitter, RenderTargets& render_targets,
                               NativeSharedMemory& shared_memory) {
  submitter_ = &submitter;
  render_targets_ = &render_targets;
  shared_memory_ = &shared_memory;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter.vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // The EDRAM buffer.
  if (!util::CreateDedicatedAllocationBuffer(vulkan_device, kEdramBufferSize,
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             util::MemoryPurpose::kDeviceLocal, edram_buffer_,
                                             edram_memory_)) {
    return false;
  }

  // Set layouts.
  VkDescriptorSetLayoutBinding storage_binding = {};
  storage_binding.binding = 0;
  storage_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  storage_binding.descriptorCount = 1;
  storage_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &storage_binding;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &storage_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayoutBinding source_bindings[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    source_bindings[i].binding = i;
    source_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    source_bindings[i].descriptorCount = 1;
    source_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  set_layout_info.bindingCount = 2;
  set_layout_info.pBindings = source_bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &source_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  // Transient destination sets (when the shared memory can't be bound whole).
  VkDescriptorPoolSize transient_size = {};
  transient_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  transient_size.descriptorCount = kTransientSets;
  VkDescriptorPoolCreateInfo transient_pool_info = {};
  transient_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  transient_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  transient_pool_info.maxSets = kTransientSets;
  transient_pool_info.poolSizeCount = 1;
  transient_pool_info.pPoolSizes = &transient_size;
  if (dfn.vkCreateDescriptorPool(device, &transient_pool_info, nullptr, &transient_pool_) !=
      VK_SUCCESS) {
    return false;
  }

  // The persistent sets: the EDRAM buffer, and the whole shared memory.
  auto write_storage = [&](VkDescriptorSet set, VkBuffer buffer, VkDeviceSize offset,
                           VkDeviceSize range) {
    VkDescriptorBufferInfo info = {};
    info.buffer = buffer;
    info.offset = offset;
    info.range = range;
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  };
  edram_set_ = AllocateSet(storage_set_layout_);
  if (edram_set_ == VK_NULL_HANDLE) {
    return false;
  }
  write_storage(edram_set_, edram_buffer_, 0, kEdramBufferSize);
  if (vulkan_device->properties().maxStorageBufferRange >= SharedMemory::kBufferSize) {
    shared_memory_set_ = AllocateSet(storage_set_layout_);
    if (shared_memory_set_ == VK_NULL_HANDLE) {
      return false;
    }
    write_storage(shared_memory_set_, shared_memory.buffer(), 0, SharedMemory::kBufferSize);
  }

  // The dump pipelines.
  VkDescriptorSetLayout dump_set_layouts[2] = {source_set_layout_, storage_set_layout_};
  VkPushConstantRange dump_push_constants = {};
  dump_push_constants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  dump_push_constants.size = sizeof(DumpConstants);
  VkPipelineLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 2;
  layout_info.pSetLayouts = dump_set_layouts;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &dump_push_constants;
  if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &dump_layout_) != VK_SUCCESS) {
    return false;
  }
  for (uint32_t variant = 0; variant < 4; ++variant) {
    std::string source = "#version 460\n#extension GL_EXT_samplerless_texture_functions : require\n";
    source += "#define SRC_DEPTH " + std::to_string(variant >> 1) + "\n";
    source += "#define SRC_MS " + std::to_string(variant & 1) + "\n";
    source += kDumpShaderBody;
    std::vector<uint32_t> spirv;
    std::string error;
    if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, source, spirv, error)) {
      REXGPU_ERROR("Native renderer: EDRAM dump shader {} failed to compile: {}", variant, error);
      return false;
    }
    dump_pipelines_[variant] = util::CreateComputePipeline(
        vulkan_device, dump_layout_, spirv.data(), spirv.size() * sizeof(uint32_t));
    if (dump_pipelines_[variant] == VK_NULL_HANDLE) {
      return false;
    }
  }

  // The SDK's copy pipelines, with the full constants (the destination base
  // included).
  VkDescriptorSetLayout copy_set_layouts[2] = {storage_set_layout_, storage_set_layout_};
  VkPushConstantRange copy_push_constants = {};
  copy_push_constants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  copy_push_constants.size = sizeof(draw_util::ResolveCopyShaderConstants);
  layout_info.pSetLayouts = copy_set_layouts;
  layout_info.pPushConstantRanges = &copy_push_constants;
  if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &copy_layout_) != VK_SUCCESS) {
    return false;
  }
  for (size_t i = 0; i < copy_pipelines_.size(); ++i) {
    copy_pipelines_[i] = util::CreateComputePipeline(vulkan_device, copy_layout_,
                                                     kCopyShaders[i].code, kCopyShaders[i].size);
    if (copy_pipelines_[i] == VK_NULL_HANDLE) {
      REXGPU_ERROR("Native renderer: failed to create the resolve copy pipeline {}",
                   draw_util::resolve_copy_shader_info[i].debug_name);
      return false;
    }
  }
  return true;
}

void ResolveMemory::Shutdown() {
  if (!submitter_) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (vulkan_device) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    for (VkPipeline& pipeline : copy_pipelines_) {
      util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
    }
    for (VkPipeline& pipeline : dump_pipelines_) {
      util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
    }
    util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, copy_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, dump_layout_);
    for (auto& [target, source] : sources_) {
      if (source.depth_view != VK_NULL_HANDLE) {
        dfn.vkDestroyImageView(device, source.depth_view, nullptr);
      }
      if (source.stencil_view != VK_NULL_HANDLE) {
        dfn.vkDestroyImageView(device, source.stencil_view, nullptr);
      }
    }
    // Destroying the pools frees their sets.
    for (VkDescriptorPool pool : pools_) {
      dfn.vkDestroyDescriptorPool(device, pool, nullptr);
    }
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, transient_pool_);
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, source_set_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, storage_set_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, edram_buffer_);
    util::DestroyAndNullHandle(dfn.vkFreeMemory, device, edram_memory_);
  }
  sources_.clear();
  pools_.clear();
  pool_sets_left_ = 0;
  edram_set_ = VK_NULL_HANDLE;
  shared_memory_set_ = VK_NULL_HANDLE;
  logged_ = 0;
  submitter_ = nullptr;
  render_targets_ = nullptr;
  shared_memory_ = nullptr;
}

VkDescriptorSet ResolveMemory::AllocateSet(VkDescriptorSetLayout layout) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (!pool_sets_left_) {
    VkDescriptorPoolSize pool_sizes[2] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[0].descriptorCount = kSetsPerPool * 2;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = kSetsPerPool;
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = kSetsPerPool;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    VkDescriptorPool pool;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
      LogOnce(kLogDescriptors, "failed to create a resolve memory descriptor pool");
      return VK_NULL_HANDLE;
    }
    pools_.push_back(pool);
    pool_sets_left_ = kSetsPerPool;
  }
  VkDescriptorSetAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorPool = pools_.back();
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &layout;
  VkDescriptorSet set;
  if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &set) != VK_SUCCESS) {
    LogOnce(kLogDescriptors, "failed to allocate a resolve memory descriptor set");
    return VK_NULL_HANDLE;
  }
  --pool_sets_left_;
  return set;
}

const ResolveMemory::Source* ResolveMemory::GetSource(RenderTarget& source_target) {
  auto it = sources_.find(&source_target);
  if (it != sources_.end()) {
    return it->second.set != VK_NULL_HANDLE ? &it->second : nullptr;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  Source& source = sources_[&source_target];
  if (source_target.depth) {
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = source_target.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = source_target.vk_format;
    view_info.subresourceRange = util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT);
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &source.depth_view) != VK_SUCCESS) {
      source.depth_view = VK_NULL_HANDLE;
      return nullptr;
    }
    view_info.subresourceRange = util::InitializeSubresourceRange(VK_IMAGE_ASPECT_STENCIL_BIT);
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &source.stencil_view) != VK_SUCCESS) {
      source.stencil_view = VK_NULL_HANDLE;
      return nullptr;
    }
  }
  source.set = AllocateSet(source_set_layout_);
  if (source.set == VK_NULL_HANDLE) {
    return nullptr;
  }
  VkDescriptorImageInfo image_infos[2] = {};
  VkWriteDescriptorSet writes[2] = {};
  uint32_t write_count = 0;
  auto add_write = [&](VkImageView view, VkImageLayout layout) {
    image_infos[write_count].imageView = view;
    image_infos[write_count].imageLayout = layout;
    VkWriteDescriptorSet& write = writes[write_count];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = source.set;
    write.dstBinding = write_count;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image_infos[write_count];
    ++write_count;
  };
  if (source_target.depth) {
    add_write(source.depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    add_write(source.stencil_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  } else {
    add_write(source_target.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  dfn.vkUpdateDescriptorSets(device, write_count, writes, 0, nullptr);
  return &source;
}

void ResolveMemory::ComputeBarrier(CommandRecorder* command_buffer) {
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  command_buffer->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0,
                                       nullptr, 0, nullptr);
}

bool ResolveMemory::Dump(RenderTarget& target, uint32_t span_base, uint32_t span_pitch,
                         uint32_t span_columns, uint32_t span_rows,
                         xenos::MsaaSamples view_msaa, uint32_t sample_mask) {
  const Source* source = GetSource(target);
  const uint32_t pitch_tiles = PitchTiles(target.surface);
  if (!source || !pitch_tiles || !span_columns || !span_rows || !target.surface.width ||
      !target.surface.height || !target.host_width || !target.host_height) {
    return false;
  }
  const bool multisampled = target.samples != VK_SAMPLE_COUNT_1_BIT;
  CommandRecorder* command_buffer = submitter_->OpenOutsideRenderPass(
      RenderPassEnd::kResolve, target.depth   ? GpuKind::kResolveDumpDepth
                               : multisampled ? GpuKind::kResolveDumpMsaa
                                              : GpuKind::kResolveDump);
  if (command_buffer == nullptr) {
    return false;
  }
  render_targets_->Transition(command_buffer, target,
                              target.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  // After the previous copy's reads of the EDRAM buffer.
  ComputeBarrier(command_buffer);
  command_buffer->CmdVkBindPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      dump_pipelines_[(uint32_t(target.depth) << 1) | uint32_t(multisampled)]);
  VkDescriptorSet sets[2] = {source->set, edram_set_};
  command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, dump_layout_, 0, 2, sets,
                                          0, nullptr);
  DumpConstants constants = {};
  constants.span_base = span_base;
  constants.span_pitch = span_pitch;
  constants.span_columns = span_columns;
  constants.span_rows = span_rows;
  constants.base_tiles = target.surface.base_tiles;
  constants.pitch_tiles = pitch_tiles;
  constants.msaa_samples = uint32_t(target.msaa);
  constants.format = target.format;
  constants.format_ints_log2 = uint32_t(target.surface.is_64bpp);
  constants.surface_width = target.surface.width;
  constants.surface_height = target.surface.height;
  constants.tile_count = render_targets_->tile_count();
  constants.view_msaa_samples = uint32_t(view_msaa);
  constants.sample_mask = sample_mask;
  constants.scale4 = target.scale4;
  constants.host_max_x = target.host_width - 1;
  constants.host_max_y = target.host_height - 1;
  command_buffer->CmdVkPushConstants(dump_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                     sizeof(constants), &constants);
  const uint32_t span_width_samples =
      span_columns * (xenos::kEdramTileWidthSamples >> uint32_t(target.surface.is_64bpp));
  command_buffer->CmdVkDispatch(
      (span_width_samples + kDumpGroupWidth - 1) / kDumpGroupWidth,
      (span_rows * xenos::kEdramTileHeightSamples + kDumpGroupHeight - 1) / kDumpGroupHeight, 1);
  ComputeBarrier(command_buffer);
  return true;
}

bool ResolveMemory::Resolve(const RegisterFile& regs, const memory::Memory& memory,
                            RenderTarget& source, uint32_t& written_start_out,
                            uint32_t& written_length_out) {
  written_start_out = 0;
  written_length_out = 0;
  if (!submitter_) {
    return false;
  }
  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(regs, memory, 1, 1, render_targets_->tile_count(), false, false,
                                 resolve_info) ||
      !resolve_info.copy_dest_extent_length || !resolve_info.coordinate_info.width_div_8 ||
      !resolve_info.height_div_8) {
    return false;
  }
  draw_util::ResolveCopyShaderConstants constants;
  uint32_t group_count_x, group_count_y;
  draw_util::ResolveCopyShaderIndex copy_shader =
      resolve_info.GetCopyShader(1, 1, constants, group_count_x, group_count_y);
  if (copy_shader == draw_util::ResolveCopyShaderIndex::kUnknown || !group_count_x ||
      !group_count_y) {
    return false;
  }

  // The source's samples over the copied tiles (the source owns them): those
  // the copy reads (native_resolve_dump_samples).
  uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
  const xenos::MsaaSamples view_msaa =
      (resolve_info.IsCopyingDepth() ? resolve_info.depth_edram_info
                                     : resolve_info.color_edram_info)
          .msaa_samples;
  const uint32_t sample_mask =
      REXCVAR_GET(native_resolve_dump_samples)
          ? CopySampleMask(resolve_info.copy_dest_coordinate_info.copy_sample_select)
          : 0b1111;
  if (!Dump(source, dump_base, dump_pitch, dump_row_length_used, dump_rows, view_msaa,
            sample_mask)) {
    return false;
  }

  // The destination: the guest's data around the copy first (partial tiles).
  uint32_t dest_range = resolve_info.copy_dest_extent_start - resolve_info.copy_dest_base +
                        resolve_info.copy_dest_extent_length;
  if (!shared_memory_->RequestRange(resolve_info.copy_dest_extent_start,
                                    resolve_info.copy_dest_extent_length,
                                    SharedMemory::RequestKind::kResolve)) {
    LogOnce(kLogDestMemory, "a resolve destination's memory couldn't be made resident");
    return false;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkDescriptorSet dest_set = shared_memory_set_;
  if (dest_set == VK_NULL_HANDLE) {
    // A set over the destination, its offset aligned as the device needs.
    VkDeviceSize alignment =
        std::max(vulkan_device->properties().minStorageBufferOffsetAlignment, VkDeviceSize(1));
    VkDeviceSize offset = VkDeviceSize(resolve_info.copy_dest_base) / alignment * alignment;
    VkDescriptorSetAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = transient_pool_;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &storage_set_layout_;
    if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &dest_set) != VK_SUCCESS) {
      LogOnce(kLogDescriptors, "out of resolve destination descriptor sets");
      return false;
    }
    VkDescriptorBufferInfo info = {};
    info.buffer = shared_memory_->buffer();
    info.offset = offset;
    info.range = VkDeviceSize(resolve_info.copy_dest_base) - offset + dest_range;
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = dest_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    constants.dest_base -= uint32_t(offset);
    VkDescriptorPool pool = transient_pool_;
    submitter_->DestroyAfter([vulkan_device, pool, dest_set]() {
      vulkan_device->functions().vkFreeDescriptorSets(vulkan_device->device(), pool, 1, &dest_set);
    });
  }
  shared_memory_->Use(NativeSharedMemory::Usage::kComputeWrite,
                      std::make_pair(resolve_info.copy_dest_extent_start,
                                     resolve_info.copy_dest_extent_length));
  CommandRecorder* command_buffer =
      submitter_->OpenOutsideRenderPass(RenderPassEnd::kResolve, GpuKind::kResolveMemory);
  if (command_buffer == nullptr) {
    return false;
  }
  command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE,
                                    copy_pipelines_[size_t(copy_shader)]);
  VkDescriptorSet sets[2] = {edram_set_, dest_set};
  command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, copy_layout_, 0, 2, sets,
                                          0, nullptr);
  command_buffer->CmdVkPushConstants(copy_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                     sizeof(constants), &constants);
  command_buffer->CmdVkDispatch(group_count_x, group_count_y, 1);
  written_start_out = resolve_info.copy_dest_extent_start;
  written_length_out = resolve_info.copy_dest_extent_length;
  return true;
}

}  // namespace rex::graphics::native
