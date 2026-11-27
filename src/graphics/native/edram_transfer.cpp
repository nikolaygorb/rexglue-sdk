/**
 * @file        graphics/native/edram_transfer.cpp
 * @brief       Native renderer: EDRAM contents carried between host render targets
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              InitTransferPipelines, GetTransferPipeline, TransferTiles, kTransferShader),
 *              Copyright (c) 2026 OpenPortroid, MIT License
 *              (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/edram_transfer.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include <rex/graphics/native/edram_ownership.h>
#include <rex/graphics/native/render_targets.h>
#include <rex/graphics/native/submission.h>
#include <rex/cvar.h>
#include <rex/graphics/vulkan/glsl_compile.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

// render_targets.cpp: the transfer's render pass over its rectangles only.
REXCVAR_DECLARE(bool, native_pass_area_bounds);

REXCVAR_DEFINE_BOOL(native_transfer_stencil_bit_mask, true, "GPU",
                    "Native renderer: EDRAM transfers into depth draw only the stencil bits set "
                    "in their source (a compute pass finds them; needs conditional rendering); "
                    "off: all 8 bit passes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(native_transfer_sample_shading, true, "GPU",
                    "Native renderer: EDRAM transfers into a multisampled surface write all its "
                    "samples in one pass (sample rate shading, as xenos); off: a pass per sample")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(native_transfer_stencil_sample_mask, true, "GPU",
                    "Native renderer: EDRAM transfers' stencil bit passes into a multisampled "
                    "depth surface run once per pixel and set all its samples through the "
                    "sample mask; off: once per sample (native_transfer_sample_shading)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(native_transfer_stencil_export, true, "GPU",
                    "Native renderer: EDRAM transfers into depth write the stencil from the "
                    "shader in the depth pass (VK_EXT_shader_stencil_export, as xenos); off: a "
                    "pass per stencil bit")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::native {

// The SDK's clockwise fullscreen triangle (`xb buildshaders`).
namespace shaders {
#include "../shaders/vulkan_spirv/fullscreen_cw_vs.h"
}  // namespace shaders

namespace util = ui::vulkan::util;

namespace {

// Native renderer (rexgpu-native): EDRAM tiles one host render target leaves,
// brought into another one over the same tiles, as the EDRAM would read them:
// the source's samples packed into raw 32-bit cells, unpacked in the
// destination's format (64bpp colour copied as it is, between surfaces of one
// format). The fragments are the destination's tiles the source owned
// (scissor rectangles). A multisampled destination is written either all
// samples in one pass (SAMPLE_SHADING: an invocation a sample, gl_SampleID) or
// one sample a pass (gl_SampleMask); each sample finds its place in the tile
// as the EDRAM lays samples out, then the source's pixel and sample there.
// A depth destination's stencil comes either from the depth pass
// (STENCIL_EXPORT: gl_FragStencilRefARB) or from a pass per bit; into a
// multisampled one, a bit pass may cover all samples once per pixel through
// the sample mask (STENCIL_SAMPLE_MASK).
//
// Vulkan GLSL after a prefix (EdramTransfers::GetPixelShader): #version, the
// samplerless texture functions extension (and the stencil export one),
// SRC_DEPTH, DST_DEPTH, SRC_MS, DST_MS, SAMPLE_SHADING, STENCIL_EXPORT and
// STENCIL_SAMPLE_MASK (0 or 1).
// Compiled with the linked glslang (vulkan::CompileGlslToSpirv), so building
// needs no shader tools.
constexpr char kEdramTransferShaderBody[] = R"glsl(
layout(push_constant) uniform Constants {
  uint dst_base;
  uint dst_pitch_tiles;
  // xenos::ColorRenderTargetFormat (key) or DepthRenderTargetFormat.
  uint dst_format;
  uint src_base;
  uint src_pitch_tiles;
  uint src_format;
  // Depth tiles store their two halves swapped relative to colour ones.
  uint swap_halves;
  // Depth destination: 0 the depth pass, else the stencil bit of this pass.
  uint stencil_bit;
  // Host pixels per guest pixel in quarters: destination | source << 8.
  uint scales;
  // Guest pixels of a tile, width | height << 8 (TileWidthPixels,
  // TileHeightPixels): destination and source.
  uint dst_tile;
  uint src_tile;
  // xenos::MsaaSamples of the destination | of the source << 2 | the guest
  // sample this pass writes << 4; bit 8: the destination is 64bpp, bit 9: the
  // source.
  uint msaa;
  uint tile_count;
} c;

#if SRC_MS
#define SRC_TEXTURE texture2DMS
#define SRC_UTEXTURE utexture2DMS
#define FETCH(t, p, s) texelFetch(t, p, int(s))
#else
#define SRC_TEXTURE texture2D
#define SRC_UTEXTURE utexture2D
#define FETCH(t, p, s) texelFetch(t, p, 0)
#endif

#if SRC_DEPTH
layout(set = 0, binding = 0) uniform SRC_TEXTURE src_depth;
layout(set = 0, binding = 1) uniform SRC_UTEXTURE src_stencil;
#else
layout(set = 0, binding = 0) uniform SRC_TEXTURE src_color;
#endif

#if !DST_DEPTH
layout(location = 0) out vec4 out_color;
#endif

// Xenos 7e3 (2_10_10_10_FLOAT colour) and 20e4 (D24FS8 depth) floats.
uint Encode7e3(float f) {
  uint b = floatBitsToUint(clamp(f, 0.0, 31.875));
  uint biased = b < 0x3E800000u
                    ? ((b & 0x7FFFFFu) | 0x800000u) >> min(125u - (b >> 23), 24u)
                    : b + 0xC2000000u;
  return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
}

float Decode7e3(uint v) {
  uint m = v & 0x7Fu;
  uint e = (v >> 7) & 7u;
  return e == 0u ? float(m) / 512.0 : (1.0 + float(m) / 128.0) * exp2(float(e) - 3.0);
}

uint Encode20e4(float f) {
  uint b = floatBitsToUint(clamp(f, 0.0, 1.99999988));
  uint biased = b < 0x38800000u
                    ? ((b & 0x7FFFFFu) | 0x800000u) >> min(113u - (b >> 23), 24u)
                    : b + 0xC8000000u;
  return ((biased + 3u + ((biased >> 3) & 1u)) >> 3) & 0xFFFFFFu;
}

float Decode20e4(uint v) {
  uint m = v & 0xFFFFFu;
  uint e = v >> 20;
  return e == 0u ? float(m) * exp2(-34.0) : (1.0 + float(m) / 1048576.0) * exp2(float(e) - 15.0);
}

// Host values <-> the raw 32-bit cell. 16-bit fixed surfaces hold value / 32
// on the host: the raw snorm16.
uint PackColor(vec4 color, uint format) {
  switch (format) {
    case 2u:   // k_2_10_10_10
    case 10u: {  // k_2_10_10_10_AS_10_10_10_10
      uvec3 rgb = uvec3(round(clamp(color.rgb, 0.0, 1.0) * 1023.0));
      return rgb.r | (rgb.g << 10) | (rgb.b << 20) |
             (uint(round(clamp(color.a, 0.0, 1.0) * 3.0)) << 30);
    }
    case 3u:   // k_2_10_10_10_FLOAT
    case 12u:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
      return Encode7e3(color.r) | (Encode7e3(color.g) << 10) | (Encode7e3(color.b) << 20) |
             (uint(round(clamp(color.a, 0.0, 1.0) * 3.0)) << 30);
    case 4u: {  // k_16_16
      uvec2 s = uvec2(ivec2(round(clamp(color.rg, -1.0, 1.0) * 32767.0))) & 0xFFFFu;
      return s.r | (s.g << 16);
    }
    case 6u:  // k_16_16_FLOAT
      return packHalf2x16(color.rg);
    case 14u:  // k_32_FLOAT
      return floatBitsToUint(color.r);
    default: {  // k_8_8_8_8 (and its gamma variant's raw bytes)
      uvec4 b = uvec4(round(clamp(color, 0.0, 1.0) * 255.0));
      return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
    }
  }
}

vec4 UnpackColor(uint v, uint format) {
  switch (format) {
    case 2u:
    case 10u:
      return vec4(float(v & 0x3FFu) / 1023.0, float((v >> 10) & 0x3FFu) / 1023.0,
                  float((v >> 20) & 0x3FFu) / 1023.0, float(v >> 30) / 3.0);
    case 3u:
    case 12u:
      return vec4(Decode7e3(v & 0x3FFu), Decode7e3((v >> 10) & 0x3FFu),
                  Decode7e3((v >> 20) & 0x3FFu), float(v >> 30) / 3.0);
    case 4u: {
      ivec2 s = ivec2(int(v << 16), int(v)) >> 16;
      return vec4(max(vec2(s) / 32767.0, vec2(-1.0)), 0.0, 0.0);
    }
    case 6u:
      return vec4(unpackHalf2x16(v), 0.0, 0.0);
    case 14u:
      return vec4(uintBitsToFloat(v), 0.0, 0.0, 0.0);
    default:
      return vec4(float(v & 0xFFu), float((v >> 8) & 0xFFu), float((v >> 16) & 0xFFu),
                  float(v >> 24)) /
             255.0;
  }
}

// A guest pixel and sample within a tile <-> the sample of the tile (80x16
// 32-bit, 40x16 64-bit) where the EDRAM keeps it (the SDK's
// CanonicalizeSample / DecanonicalizeSample, XeEdramOffsetBytes): 2x samples
// are rows, 4x samples a 2x2 block, interleaved with the pixels' low bits.
uvec2 ToTileSample(uvec2 pixel, uint sample_index, uint msaa) {
  if (msaa >= 2u) {
    uvec2 sample_offset = (uvec2(sample_index) >> uvec2(0u, 1u)) & 1u;
    return ((pixel & ~1u) << 1u) | (pixel & 1u) | (sample_offset << 1u);
  }
  if (msaa == 1u) {
    return uvec2((pixel.x & ~2u) | ((sample_index & 1u) << 1u),
                 ((pixel.y & ~1u) << 1u) | (pixel.y & 1u) | (pixel.x & 2u));
  }
  return pixel;
}

uvec2 FromTileSample(uvec2 tile_sample, uint msaa, out uint sample_index) {
  if (msaa >= 2u) {
    sample_index = ((tile_sample.x >> 1u) & 1u) | (tile_sample.y & 2u);
    return ((tile_sample >> 2u) << 1u) | (tile_sample & 1u);
  }
  if (msaa == 1u) {
    sample_index = (tile_sample.x >> 1u) & 1u;
    return uvec2((tile_sample.x & ~2u) | (tile_sample.y & 2u),
                 ((tile_sample.y >> 2u) << 1u) | (tile_sample.y & 1u));
  }
  sample_index = 0u;
  return tile_sample;
}

// The host sample of a guest sample: 2x has the top (guest 0) at host sample
// 1 with the standard locations; 4x is the same (bit 0 horizontal, bit 1
// vertical).
uint HostSample(uint sample_index, uint msaa) {
  return msaa == 1u ? sample_index ^ 1u : sample_index;
}

// Where the source holds what the EDRAM has for this host pixel's guest
// sample dst_sample of the destination: its host pixel and host sample.
void SourceTexel(uint dst_sample, out ivec2 src_pixel, out uint src_sample) {
  uint dst_scale4 = c.scales & 0xFFu;
  uint src_scale4 = c.scales >> 8;
  uint dst_msaa = c.msaa & 3u;
  uint src_msaa = (c.msaa >> 2u) & 3u;
  uint dst_64bpp = (c.msaa >> 8u) & 1u;
  uint src_64bpp = (c.msaa >> 9u) & 1u;
  uvec2 dst_tile = uvec2(c.dst_tile & 0xFFu, c.dst_tile >> 8u);
  uvec2 src_tile = uvec2(c.src_tile & 0xFFu, c.src_tile >> 8u);
  // The destination's guest pixel of this host pixel, and where in it the
  // host pixel's centre is (for a source at another scale: its nearest
  // pixel).
  vec2 guest = (floor(gl_FragCoord.xy) + 0.5) * 4.0 / float(dst_scale4);
  uvec2 dst_pixel = uvec2(guest);
  vec2 in_pixel = guest - vec2(dst_pixel);
  uvec2 tile_xy = dst_pixel / dst_tile;
  uint tile = (c.dst_base + tile_xy.y * c.dst_pitch_tiles + tile_xy.x) % c.tile_count;
  uvec2 tile_sample = ToTileSample(dst_pixel - tile_xy * dst_tile, dst_sample, dst_msaa);
  // Between 32bpp and 64bpp surfaces, the nearest sample.
  tile_sample.x = (tile_sample.x << dst_64bpp) >> src_64bpp;
  if (c.swap_halves != 0u) {
    uint half_width = 40u >> src_64bpp;
    tile_sample.x = (tile_sample.x + half_width) % (half_width * 2u);
  }
  uint src_guest_sample;
  uvec2 src_in_tile = FromTileSample(tile_sample, src_msaa, src_guest_sample);
  src_sample = HostSample(src_guest_sample, src_msaa);
  uint relative = (tile + c.tile_count - c.src_base) % c.tile_count;
  uvec2 src_guest =
      uvec2(relative % c.src_pitch_tiles, relative / c.src_pitch_tiles) * src_tile + src_in_tile;
  src_pixel = ivec2((vec2(src_guest) + in_pixel) * float(src_scale4) / 4.0);
}

#if STENCIL_SAMPLE_MASK
// A stencil bit pass into a multisampled depth destination, once per pixel
// rather than once per sample: the samples whose source cell has the bit go
// into the sample mask (ReIGlue 0800).
void main() {
  uint dst_msaa = c.msaa & 3u;
  uint sample_count = 1u << dst_msaa;
  int mask = 0;
  for (uint dst_sample = 0u; dst_sample < sample_count; ++dst_sample) {
    ivec2 src_pixel;
    uint src_sample;
    SourceTexel(dst_sample, src_pixel, src_sample);
#if SRC_DEPTH
    uint raw = FETCH(src_stencil, src_pixel, src_sample).r & 0xFFu;
#else
    uint raw = PackColor(FETCH(src_color, src_pixel, src_sample), c.src_format);
#endif
    if ((raw & c.stencil_bit) != 0u) {
      mask |= int(1u << HostSample(dst_sample, dst_msaa));
    }
  }
  if (mask == 0) {
    discard;
  }
  gl_SampleMask[0] = mask;
}
#else
void main() {
  uint dst_msaa = c.msaa & 3u;
  uint dst_64bpp = (c.msaa >> 8u) & 1u;
#if SAMPLE_SHADING
  // This invocation's host sample; HostSample maps both ways.
  uint dst_sample = HostSample(uint(gl_SampleID), dst_msaa);
#else
  uint dst_sample = (c.msaa >> 4u) & 3u;
#if DST_MS
  gl_SampleMask[0] = int(1u << HostSample(dst_sample, dst_msaa));
#endif
#endif
  ivec2 src_pixel;
  uint src_sample;
  SourceTexel(dst_sample, src_pixel, src_sample);

#if SRC_DEPTH
  // Float24 depth is held at half its value on the host (the draws'
  // viewport, GetHostViewportInfo full_float24_in_0_to_1).
  float depth = FETCH(src_depth, src_pixel, src_sample).r;
  uint depth24 = c.src_format == 1u ? Encode20e4(depth * 2.0)
                                    : uint(round(clamp(depth, 0.0, 1.0) * 16777215.0));
  uint raw = (depth24 << 8) | (FETCH(src_stencil, src_pixel, src_sample).r & 0xFFu);
#else
  vec4 color = FETCH(src_color, src_pixel, src_sample);
#if !DST_DEPTH
  if (dst_64bpp != 0u) {
    out_color = color;
    return;
  }
#endif
  uint raw = PackColor(color, c.src_format);
#endif

#if DST_DEPTH
  // The depth pass, then one pass per stencil bit (no stencil export).
  if (c.stencil_bit != 0u && (raw & c.stencil_bit) == 0u) {
    discard;
  }
#if STENCIL_EXPORT
  gl_FragStencilRefARB = int(raw & 0xFFu);
#endif
#if SRC_DEPTH
  // Between depth surfaces of the same format, the host's depth as it is:
  // draws write it at float32 (not 20e4 or unorm24) precision, and later
  // draws testing against it (equal, prepasses) must see what they wrote,
  // as xenos keeps the host depth across such transfers.
  if (c.src_format == c.dst_format) {
    gl_FragDepth = depth;
    return;
  }
#endif
  // 20e4 reaches 2: held at half its value on the host.
  float dst_depth =
      c.dst_format == 1u ? Decode20e4(raw >> 8) * 0.5 : float(raw >> 8) / 16777215.0;
  gl_FragDepth = clamp(dst_depth, 0.0, 1.0);
#else
  out_color = UnpackColor(raw, c.dst_format);
#endif
}
#endif  // STENCIL_SAMPLE_MASK
)glsl";

enum LogReason : uint32_t {
  kLogShader = 1u << 0,
  kLogPipeline = 1u << 1,
  kLogDescriptors = 1u << 2,
};

// As kEdramTransferShaderBody's push constants.
struct TransferConstants {
  uint32_t dst_base;
  uint32_t dst_pitch_tiles;
  uint32_t dst_format;
  uint32_t src_base;
  uint32_t src_pitch_tiles;
  uint32_t src_format;
  uint32_t swap_halves;
  uint32_t stencil_bit;
  uint32_t scales;
  uint32_t dst_tile;
  uint32_t src_tile;
  uint32_t msaa;
  uint32_t tile_count;
};
static_assert(sizeof(TransferConstants) == 13 * sizeof(uint32_t));

constexpr uint32_t kSetsPerPool = 64;

// Which of the 8 stencil bits are set anywhere in rows [y_begin, y_end) of a
// depth source (EDRAM transfers into depth draw one pass per bit; with this
// mask, conditional rendering skips the passes of bits that are clear).
// Word slot * 8 + bit, zeroed before, becomes 1 where the bit is set in any
// sample. ReIGlue 0461 (stencil_bit_mask.cs.glsl).
constexpr char kStencilMaskShaderBody[] = R"(
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform StencilMaskConstants {
  uint width;
  uint y_begin;
  uint y_end;
  uint samples;
  uint slot;
} constants;

layout(set = 0, binding = 0) buffer StencilMask {
  uint bits[];
} mask;

// Binding 0 of the transfer source set is the depth, unused here.
#if SRC_MS
layout(set = 1, binding = 1) uniform utexture2DMS stencil_texture;
#else
layout(set = 1, binding = 1) uniform utexture2D stencil_texture;
#endif

shared uint group_bits;

void main() {
  if (gl_LocalInvocationIndex == 0u) {
    group_bits = 0u;
  }
  barrier();
  uvec2 pixel = uvec2(gl_GlobalInvocationID.x, gl_GlobalInvocationID.y + constants.y_begin);
  uint bits = 0u;
  if (pixel.x < constants.width && pixel.y < constants.y_end) {
#if SRC_MS
    for (uint i = 0u; i < constants.samples; ++i) {
      bits |= texelFetch(stencil_texture, ivec2(pixel), int(i)).r;
    }
#else
    bits = texelFetch(stencil_texture, ivec2(pixel), 0).r;
#endif
  }
  if (bits != 0u) {
    atomicOr(group_bits, bits & 0xFFu);
  }
  barrier();
  if (gl_LocalInvocationIndex == 0u && group_bits != 0u) {
    for (uint i = 0u; i < 8u; ++i) {
      if ((group_bits & (1u << i)) != 0u) {
        atomicOr(mask.bits[constants.slot * 8u + i], 1u);
      }
    }
  }
}
)";

}  // namespace

EdramTransfers::~EdramTransfers() {
  Shutdown();
}

bool EdramTransfers::Initialize(Submitter& submitter, RenderTargets& render_targets) {
  submitter_ = &submitter;
  render_targets_ = &render_targets;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter.vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkDescriptorSetLayoutBinding bindings[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[i].descriptorCount = 1;
    // Compute: the stencil bit mask reads the stencil.
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 2;
  set_layout_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push_constants = {};
  push_constants.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  push_constants.size = sizeof(TransferConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &set_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constants;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  vertex_shader_ = util::CreateShaderModule(vulkan_device, shaders::fullscreen_cw_vs,
                                            sizeof(shaders::fullscreen_cw_vs));
  // The single-sampled pixel shaders now (a few milliseconds, once), the
  // multisampled ones when a title has such surfaces.
  for (uint32_t variant = 0; variant < 16; variant += 4) {
    if (GetPixelShader(variant) == VK_NULL_HANDLE) {
      return false;
    }
  }
  if (vertex_shader_ == VK_NULL_HANDLE) {
    return false;
  }
  // Optional: without it every stencil bit pass runs.
  if (!InitializeStencilMask()) {
    ShutdownStencilMask();
  }
  return true;
}

bool EdramTransfers::InitializeStencilMask() {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (!vulkan_device->properties().conditionalRendering) {
    return false;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const VkDeviceSize size = VkDeviceSize(sizeof(uint32_t)) * 8 * kMaskRegions * kMaskSlotsPerRegion;
  if (!util::CreateDedicatedAllocationBuffer(
          vulkan_device, size,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT,
          util::MemoryPurpose::kDeviceLocal, mask_buffer_, mask_memory_)) {
    mask_buffer_ = VK_NULL_HANDLE;
    mask_memory_ = VK_NULL_HANDLE;
    return false;
  }
  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &binding;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &mask_set_layout_) !=
      VK_SUCCESS) {
    mask_set_layout_ = VK_NULL_HANDLE;
    return false;
  }
  VkDescriptorPoolSize pool_size = {};
  pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_size.descriptorCount = 1;
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &mask_pool_) != VK_SUCCESS) {
    mask_pool_ = VK_NULL_HANDLE;
    return false;
  }
  VkDescriptorSetAllocateInfo set_info = {};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_info.descriptorPool = mask_pool_;
  set_info.descriptorSetCount = 1;
  set_info.pSetLayouts = &mask_set_layout_;
  if (dfn.vkAllocateDescriptorSets(device, &set_info, &mask_set_) != VK_SUCCESS) {
    mask_set_ = VK_NULL_HANDLE;
    return false;
  }
  VkDescriptorBufferInfo buffer_info = {};
  buffer_info.buffer = mask_buffer_;
  buffer_info.range = VK_WHOLE_SIZE;
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = mask_set_;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &buffer_info;
  dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

  // Set 0 the mask, set 1 the source's transfer set.
  VkDescriptorSetLayout set_layouts[] = {mask_set_layout_, set_layout_};
  VkPushConstantRange push_constants = {};
  push_constants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constants.size = sizeof(uint32_t) * 5;
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 2;
  pipeline_layout_info.pSetLayouts = set_layouts;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constants;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &mask_pipeline_layout_) !=
      VK_SUCCESS) {
    mask_pipeline_layout_ = VK_NULL_HANDLE;
    return false;
  }
  for (uint32_t ms = 0; ms < 2; ++ms) {
    std::string source =
        "#version 460\n#extension GL_EXT_samplerless_texture_functions : require\n";
    source += "#define SRC_MS " + std::to_string(ms) + "\n";
    source += kStencilMaskShaderBody;
    std::vector<uint32_t> spirv;
    std::string error;
    if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, source, spirv, error)) {
      REXGPU_ERROR("Native renderer: stencil bit mask shader {} failed to compile: {}", ms, error);
      return false;
    }
    mask_pipelines_[ms] = util::CreateComputePipeline(
        vulkan_device, mask_pipeline_layout_, spirv.data(), spirv.size() * sizeof(uint32_t));
    if (mask_pipelines_[ms] == VK_NULL_HANDLE) {
      return false;
    }
  }
  REXGPU_INFO(
      "Native renderer: EDRAM transfers into depth draw only the stencil bits set in their "
      "source (conditional rendering)");
  return true;
}

void EdramTransfers::ShutdownStencilMask() {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (!vulkan_device) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (VkPipeline& pipeline : mask_pipelines_) {
    util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
  }
  util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, mask_pipeline_layout_);
  // Destroying the pool frees the set.
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, mask_pool_);
  mask_set_ = VK_NULL_HANDLE;
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, mask_set_layout_);
  util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, mask_buffer_);
  util::DestroyAndNullHandle(dfn.vkFreeMemory, device, mask_memory_);
  mask_region_submission_ = {};
  mask_submission_ = UINT64_MAX;
  mask_next_slot_ = 0;
  mask_region_usable_ = false;
}

uint32_t EdramTransfers::AllocateMaskSlot() {
  const uint64_t submission = submitter_->current();
  const uint32_t region = uint32_t(submission % kMaskRegions);
  if (mask_submission_ != submission) {
    mask_submission_ = submission;
    mask_next_slot_ = 0;
    // The region's words may be read by its last submission until that one
    // is complete.
    mask_region_usable_ = mask_region_submission_[region] <= submitter_->completed();
    if (mask_region_usable_) {
      mask_region_submission_[region] = submission;
    }
  }
  if (!mask_region_usable_ || mask_next_slot_ >= kMaskSlotsPerRegion) {
    return UINT32_MAX;
  }
  return region * kMaskSlotsPerRegion + mask_next_slot_++;
}

uint32_t EdramTransfers::RecordStencilMask(CommandRecorder* command_buffer, const RenderTarget& dst,
                                           const RenderTarget& src, const Source& source,
                                           const std::vector<Rect>& rects) {
  if (mask_pipelines_[0] == VK_NULL_HANDLE || !REXCVAR_GET(native_transfer_stencil_bit_mask) ||
      !src.depth || source.stencil_view == VK_NULL_HANDLE) {
    return UINT32_MAX;
  }
  // The source rows the transfer reads: those at the EDRAM tile rows of the
  // destination rectangles (all columns), else the whole source.
  const uint32_t tile_count = render_targets_->tile_count();
  const uint32_t dst_pitch = PitchTiles(dst.surface);
  const uint32_t src_pitch = PitchTiles(src.surface);
  const uint32_t dst_tile_height = TileHeightPixels(dst.surface);
  const uint32_t src_tile_height = TileHeightPixels(src.surface);
  uint32_t y_begin = 0;
  uint32_t y_end = src.surface.height;
  if (dst_pitch && src_pitch && dst_tile_height && src_tile_height && tile_count) {
    uint32_t row_begin = UINT32_MAX;
    uint32_t row_end = 0;
    for (const Rect& rect : rects) {
      if (rect.bottom <= rect.top || rect.bottom <= 0) {
        continue;
      }
      row_begin = std::min(row_begin, uint32_t(std::max(rect.top, int32_t(0))) / dst_tile_height);
      row_end = std::max(row_end, (uint32_t(rect.bottom) - 1) / dst_tile_height + 1);
    }
    if (row_begin < row_end) {
      // In tiles after the source's base (EDRAM addresses wrap).
      const uint32_t start = (dst.surface.base_tiles + row_begin * dst_pitch + tile_count -
                              src.surface.base_tiles % tile_count) %
                             tile_count;
      const uint32_t end = start + (row_end - row_begin) * dst_pitch;
      const uint32_t rows_begin = start / src_pitch * src_tile_height;
      const uint32_t rows_end = std::min((end + src_pitch - 1) / src_pitch * src_tile_height,
                                         uint32_t(src.surface.height));
      if (rows_begin < rows_end) {
        y_begin = rows_begin;
        y_end = rows_end;
      }
    }
  }
  const uint32_t host_y_begin = std::min(y_begin * src.scale4 / 4, src.host_height);
  const uint32_t host_y_end = std::min((y_end * src.scale4 + 3) / 4, src.host_height);
  if (host_y_begin >= host_y_end || !src.host_width) {
    return UINT32_MAX;
  }
  const uint32_t slot = AllocateMaskSlot();
  if (slot == UINT32_MAX) {
    return UINT32_MAX;
  }

  const VkDeviceSize offset = VkDeviceSize(sizeof(uint32_t)) * 8 * slot;
  const VkDeviceSize size = VkDeviceSize(sizeof(uint32_t)) * 8;
  command_buffer->CmdVkFillBuffer(mask_buffer_, offset, size, 0);
  VkBufferMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = mask_buffer_;
  barrier.offset = offset;
  barrier.size = size;
  command_buffer->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                       &barrier, 0, nullptr);
  const uint32_t samples = uint32_t(src.samples);
  command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE,
                                    mask_pipelines_[samples > 1 ? 1 : 0]);
  VkDescriptorSet sets[] = {mask_set_, source.set};
  command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, mask_pipeline_layout_, 0,
                                          2, sets, 0, nullptr);
  const uint32_t constants[] = {src.host_width, host_y_begin, host_y_end, samples, slot};
  command_buffer->CmdVkPushConstants(mask_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                     sizeof(constants), constants);
  command_buffer->CmdVkDispatch((src.host_width + 15) / 16, (host_y_end - host_y_begin + 7) / 8, 1);
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT;
  command_buffer->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT, 0, 0,
                                       nullptr, 1, &barrier, 0, nullptr);
  return slot;
}

VkShaderModule EdramTransfers::GetPixelShader(uint32_t variant) {
  if (pixel_shaders_tried_[variant]) {
    return pixel_shaders_[variant];
  }
  pixel_shaders_tried_[variant] = true;
  const uint32_t stencil_export = (variant >> 5) & 1;
  std::string source = "#version 460\n#extension GL_EXT_samplerless_texture_functions : require\n";
  if (stencil_export) {
    source += "#extension GL_ARB_shader_stencil_export : require\n";
  }
  source += "#define SRC_DEPTH " + std::to_string((variant >> 3) & 1) + "\n";
  source += "#define DST_DEPTH " + std::to_string((variant >> 2) & 1) + "\n";
  source += "#define SRC_MS " + std::to_string((variant >> 1) & 1) + "\n";
  source += "#define DST_MS " + std::to_string(variant & 1) + "\n";
  source += "#define SAMPLE_SHADING " + std::to_string((variant >> 4) & 1) + "\n";
  source += "#define STENCIL_EXPORT " + std::to_string(stencil_export) + "\n";
  source += "#define STENCIL_SAMPLE_MASK " + std::to_string((variant >> 6) & 1) + "\n";
  source += kEdramTransferShaderBody;
  std::vector<uint32_t> spirv;
  std::string error;
  if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_FRAGMENT_BIT, source, spirv, error)) {
    REXGPU_ERROR("Native renderer: EDRAM transfer shader {} failed to compile: {}", variant,
                 error);
    return VK_NULL_HANDLE;
  }
  pixel_shaders_[variant] = util::CreateShaderModule(submitter_->vulkan_device(), spirv.data(),
                                                     spirv.size() * sizeof(uint32_t));
  return pixel_shaders_[variant];
}

void EdramTransfers::Shutdown() {
  if (!submitter_) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (vulkan_device) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    for (auto& [key, pipeline] : pipelines_) {
      if (pipeline != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, pipeline, nullptr);
      }
    }
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
    for (VkShaderModule& module : pixel_shaders_) {
      util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, module);
    }
    pixel_shaders_tried_ = {};
    util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, vertex_shader_);
    ShutdownStencilMask();
    util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, pipeline_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, set_layout_);
  }
  pipelines_.clear();
  sources_.clear();
  pools_.clear();
  pool_sets_left_ = 0;
  logged_ = 0;
  submitter_ = nullptr;
  render_targets_ = nullptr;
}

void EdramTransfers::LogOnce(uint32_t reason_bit, const char* message) {
  if (!(logged_ & reason_bit)) {
    logged_ |= reason_bit;
    REXGPU_WARN("Native renderer: {}", message);
  }
}

const EdramTransfers::Source* EdramTransfers::GetSource(RenderTarget& src) {
  auto it = sources_.find(&src);
  if (it != sources_.end()) {
    return it->second.set != VK_NULL_HANDLE ? &it->second : nullptr;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  Source& source = sources_[&src];

  if (src.depth) {
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = src.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = src.vk_format;
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

  if (!pool_sets_left_) {
    VkDescriptorPoolSize pool_size = {};
    pool_size.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_size.descriptorCount = kSetsPerPool * 2;
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = kSetsPerPool;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
      LogOnce(kLogDescriptors, "failed to create an EDRAM transfer descriptor pool");
      return nullptr;
    }
    pools_.push_back(pool);
    pool_sets_left_ = kSetsPerPool;
  }
  VkDescriptorSetAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorPool = pools_.back();
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &set_layout_;
  if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &source.set) != VK_SUCCESS) {
    source.set = VK_NULL_HANDLE;
    LogOnce(kLogDescriptors, "failed to allocate an EDRAM transfer descriptor set");
    return nullptr;
  }
  --pool_sets_left_;

  // Read in the layouts Transfer moves the source to.
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
  if (src.depth) {
    add_write(source.depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    add_write(source.stencil_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  } else {
    // Binding 1 isn't read by the colour source shaders.
    add_write(src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  dfn.vkUpdateDescriptorSets(device, write_count, writes, 0, nullptr);
  return &source;
}

VkPipeline EdramTransfers::GetPipeline(const RenderTarget& src, const RenderTarget& dst,
                                       PassKind kind, bool sample_shading,
                                       bool stencil_sample_mask) {
  const bool stencil_pass = kind == PassKind::kStencilBit;
  const bool stencil_export = kind == PassKind::kDepthStencilExport ||
                              kind == PassKind::kStencilExport;
  const bool depth_write = kind == PassKind::kMain || kind == PassKind::kDepthStencilExport;
  stencil_sample_mask =
      stencil_sample_mask && stencil_pass && dst.samples != VK_SAMPLE_COUNT_1_BIT;
  if (stencil_sample_mask) {
    sample_shading = false;
  }
  uint32_t variant = (uint32_t(stencil_sample_mask) << 6) | (uint32_t(stencil_export) << 5) |
                     (uint32_t(sample_shading) << 4) | (uint32_t(src.depth) << 3) |
                     (uint32_t(dst.depth) << 2) |
                     (uint32_t(src.samples != VK_SAMPLE_COUNT_1_BIT) << 1) |
                     uint32_t(dst.samples != VK_SAMPLE_COUNT_1_BIT);
  uint64_t key = uint64_t(uint32_t(dst.vk_format)) | (uint64_t(variant) << 32) |
                 (uint64_t(kind) << 39) | (uint64_t(dst.samples) << 41);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second;
  }
  VkRenderPass render_pass =
      render_targets_->GetClearRenderPass(dst.vk_format, dst.samples, dst.depth);
  VkShaderModule pixel_shader = GetPixelShader(variant);
  if (pixel_shader == VK_NULL_HANDLE) {
    LogOnce(kLogShader, "an EDRAM transfer shader failed to compile, transfers dropped");
  }
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (render_pass != VK_NULL_HANDLE && pixel_shader != VK_NULL_HANDLE) {
    const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = pixel_shader;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization = {};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = dst.samples;
    if (sample_shading) {
      multisample.sampleShadingEnable = VK_TRUE;
      multisample.minSampleShading = 1.0f;
    }
    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    if (!dst.depth) {
      blend.attachmentCount = 1;
      blend.pAttachments = &blend_attachment;
    }
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (dst.depth) {
      if (depth_write) {
        depth_stencil.depthTestEnable = VK_TRUE;
        depth_stencil.depthWriteEnable = VK_TRUE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
      }
      if (stencil_export) {
        // All 8 bits from the shader (gl_FragStencilRefARB replaces the
        // reference).
        depth_stencil.stencilTestEnable = VK_TRUE;
        depth_stencil.front.failOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depth_stencil.front.compareMask = 0xFF;
        depth_stencil.front.writeMask = 0xFF;
        depth_stencil.back = depth_stencil.front;
      } else if (stencil_pass) {
        // The pass's bit (dynamic write mask) set where the shader keeps the
        // fragment; the reference is all ones.
        depth_stencil.stencilTestEnable = VK_TRUE;
        depth_stencil.front.failOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depth_stencil.front.compareMask = 0xFF;
        depth_stencil.back = depth_stencil.front;
      }
    }
    VkDynamicState dynamic_states[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                        VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = stencil_pass ? 4 : 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = dst.depth ? &depth_stencil : nullptr;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass;
    pipeline_info.basePipelineIndex = -1;
    if (vulkan_device->functions().vkCreateGraphicsPipelines(
            vulkan_device->device(), VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) !=
        VK_SUCCESS) {
      pipeline = VK_NULL_HANDLE;
    }
  }
  if (pipeline == VK_NULL_HANDLE) {
    LogOnce(kLogPipeline, "failed to create an EDRAM transfer pipeline");
  }
  pipelines_.emplace(key, pipeline);
  return pipeline;
}

void EdramTransfers::Transfer(RenderTarget& dst, RenderTarget& src, uint32_t parts,
                              const std::vector<Rect>& rects) {
  if (rects.empty() || !parts || &dst == &src) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  // A colour destination takes the whole cell.
  const bool main_pass = !dst.depth || (parts & kEdramPartMain);
  const bool stencil_pass = dst.depth && (parts & kEdramPartStencil);
  // All samples of a multisampled destination in one pass where the device
  // shades per sample (as xenos), else a pass per sample.
  const bool sample_shading = dst.samples != VK_SAMPLE_COUNT_1_BIT &&
                              vulkan_device->properties().sampleRateShading &&
                              REXCVAR_GET(native_transfer_sample_shading);
  // The stencil from the shader with the depth (as xenos), else bit by bit.
  const bool stencil_export = stencil_pass &&
                              vulkan_device->extensions().ext_EXT_shader_stencil_export &&
                              REXCVAR_GET(native_transfer_stencil_export);
  // The stencil bit passes into a multisampled destination once per pixel,
  // all its samples by the sample mask (ReIGlue 0800), instead of once per
  // sample (sample shading, or a pass per sample).
  const bool stencil_sample_mask = dst.samples != VK_SAMPLE_COUNT_1_BIT &&
                                   REXCVAR_GET(native_transfer_stencil_sample_mask);
  VkPipeline main_pipeline = VK_NULL_HANDLE;
  VkPipeline stencil_pipeline = VK_NULL_HANDLE;
  if (stencil_export) {
    main_pipeline = GetPipeline(
        src, dst, main_pass ? PassKind::kDepthStencilExport : PassKind::kStencilExport,
        sample_shading);
  } else {
    if (main_pass) {
      main_pipeline = GetPipeline(src, dst, PassKind::kMain, sample_shading);
    }
    if (stencil_pass) {
      stencil_pipeline = GetPipeline(src, dst, PassKind::kStencilBit, sample_shading,
                                     stencil_sample_mask);
    }
  }
  if (main_pipeline == VK_NULL_HANDLE && stencil_pipeline == VK_NULL_HANDLE) {
    return;
  }
  const Source* source = GetSource(src);
  if (!source) {
    return;
  }

  // Host scissor rectangles.
  std::vector<VkRect2D> scissors;
  scissors.reserve(rects.size());
  for (const Rect& rect : rects) {
    Rect guest = rect;
    guest.left = std::max(guest.left, int32_t(0));
    guest.top = std::max(guest.top, int32_t(0));
    guest.right = std::min(guest.right, int32_t(dst.surface.width));
    guest.bottom = std::min(guest.bottom, int32_t(dst.surface.height));
    if (guest.empty()) {
      continue;
    }
    Rect host = ScaleRect(guest, dst.scale4);
    host.right = std::min(host.right, int32_t(dst.host_width));
    host.bottom = std::min(host.bottom, int32_t(dst.host_height));
    if (host.empty()) {
      continue;
    }
    VkRect2D scissor;
    scissor.offset = {host.left, host.top};
    scissor.extent = {uint32_t(host.right - host.left), uint32_t(host.bottom - host.top)};
    scissors.push_back(scissor);
  }
  if (scissors.empty()) {
    return;
  }

  CommandRecorder* command_buffer = submitter_->OpenOutsideRenderPass(RenderPassEnd::kEdram);
  if (command_buffer == nullptr) {
    return;
  }
  render_targets_->Transition(command_buffer, src,
                              src.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  render_targets_->Transition(command_buffer, dst,
                              dst.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                        : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  // Which stencil bits the source has, before the render pass (the
  // transitions above order it after the source's writes).
  const uint32_t mask_slot = stencil_pipeline != VK_NULL_HANDLE
                                 ? RecordStencilMask(command_buffer, dst, src, *source, rects)
                                 : UINT32_MAX;
  VkRenderPassBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin_info.renderPass = render_targets_->GetClearRenderPass(dst.vk_format, dst.samples, dst.depth);
  begin_info.framebuffer = dst.framebuffer;
  begin_info.renderArea.extent = {dst.host_width, dst.host_height};
  if (REXCVAR_GET(native_pass_area_bounds)) {
    // Loads, stores (and on tiling GPUs the tiles) only over the transfer's
    // rectangles: every draw and clear below is scissored to them.
    int32_t left = INT32_MAX, top = INT32_MAX, right = 0, bottom = 0;
    for (const VkRect2D& scissor : scissors) {
      left = std::min(left, scissor.offset.x);
      top = std::min(top, scissor.offset.y);
      right = std::max(right, scissor.offset.x + int32_t(scissor.extent.width));
      bottom = std::max(bottom, scissor.offset.y + int32_t(scissor.extent.height));
    }
    begin_info.renderArea.offset = {left, top};
    begin_info.renderArea.extent = {uint32_t(right - left), uint32_t(bottom - top)};
  }
  command_buffer->CmdVkBeginRenderPass(&begin_info, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport = {};
  viewport.width = float(dst.host_width);
  viewport.height = float(dst.host_height);
  viewport.maxDepth = 1.0f;
  command_buffer->CmdVkSetViewport(0, 1, &viewport);
  command_buffer->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                          &source->set, 0, nullptr);

  TransferConstants constants = {};
  constants.dst_base = dst.surface.base_tiles;
  constants.dst_pitch_tiles = PitchTiles(dst.surface);
  constants.dst_format = dst.format;
  constants.src_base = src.surface.base_tiles;
  constants.src_pitch_tiles = PitchTiles(src.surface);
  constants.src_format = src.format;
  constants.swap_halves = uint32_t(src.depth != dst.depth);
  constants.scales = dst.scale4 | (src.scale4 << 8);
  constants.dst_tile = TileWidthPixels(dst.surface) | (TileHeightPixels(dst.surface) << 8);
  constants.src_tile = TileWidthPixels(src.surface) | (TileHeightPixels(src.surface) << 8);
  const uint32_t msaa = uint32_t(dst.msaa) | (uint32_t(src.msaa) << 2) |
                        (uint32_t(dst.surface.is_64bpp) << 8) |
                        (uint32_t(src.surface.is_64bpp) << 9);
  constants.tile_count = render_targets_->tile_count();
  // Each sample of a multisampled destination in a pass of its own, unless
  // the pass shades per sample.
  auto draw_rects = [&](bool once_per_pixel) {
    const uint32_t dst_sample_count = once_per_pixel ? 1 : uint32_t(dst.samples);
    for (uint32_t sample = 0; sample < dst_sample_count; ++sample) {
      constants.msaa = msaa | (sample << 4);
      command_buffer->CmdVkPushConstants(pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                         sizeof(constants), &constants);
      for (const VkRect2D& scissor : scissors) {
        command_buffer->CmdVkSetScissor(0, 1, &scissor);
        command_buffer->CmdVkDraw(3, 1, 0, 0);
      }
    }
  };
  if (main_pipeline != VK_NULL_HANDLE) {
    command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, main_pipeline);
    constants.stencil_bit = 0;
    draw_rects(sample_shading);
  }
  if (stencil_pipeline != VK_NULL_HANDLE) {
    // Cleared, then set bit by bit where the source has it.
    std::vector<VkClearRect> clear_rects;
    clear_rects.reserve(scissors.size());
    for (const VkRect2D& scissor : scissors) {
      clear_rects.push_back({scissor, 0, 1});
    }
    VkClearAttachment clear = {};
    clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    clear.clearValue.depthStencil.stencil = 0;
    command_buffer->CmdVkClearAttachments(1, &clear, uint32_t(clear_rects.size()),
                                          clear_rects.data());
    command_buffer->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, stencil_pipeline);
    command_buffer->CmdVkSetStencilReference(VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    for (uint32_t bit = 0; bit < 8; ++bit) {
      command_buffer->CmdVkSetStencilWriteMask(VK_STENCIL_FACE_FRONT_AND_BACK, uint32_t(1) << bit);
      constants.stencil_bit = uint32_t(1) << bit;
      // Only if the bit is set somewhere in the rows read (cleared above).
      if (mask_slot != UINT32_MAX) {
        VkConditionalRenderingBeginInfoEXT conditional_rendering_info = {};
        conditional_rendering_info.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
        conditional_rendering_info.buffer = mask_buffer_;
        conditional_rendering_info.offset = VkDeviceSize(sizeof(uint32_t)) * (mask_slot * 8 + bit);
        command_buffer->CmdVkBeginConditionalRenderingEXT(conditional_rendering_info.buffer,
                                                          conditional_rendering_info.offset,
                                                          conditional_rendering_info.flags);
      }
      draw_rects(sample_shading || stencil_sample_mask);
      if (mask_slot != UINT32_MAX) {
        command_buffer->CmdVkEndConditionalRenderingEXT();
      }
    }
  }
  command_buffer->CmdVkEndRenderPass();
}

}  // namespace rex::graphics::native
