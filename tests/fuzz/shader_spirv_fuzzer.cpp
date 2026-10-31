/**
 * @file        fuzz/shader_spirv_fuzzer.cpp
 * @brief       libFuzzer target for Xenos microcode analysis and the SPIR-V
 *              translator (Vulkan)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/string/buffer.h>

using namespace rex::graphics;

namespace {

// The shader program area of the Xenos is 0x1000 instruction slots of 3 dwords.
constexpr size_t kMaxUcodeDwords = 0x1000 * 3;

// Input layout: the one of shader_dxbc_fuzzer, so both share a corpus, with
// the Vulkan meaning of the bits. Only states the Vulkan pipeline cache can
// create (see VulkanPipelineCache::GetCurrent*ShaderModification).
//   0: bit 0      - pixel shader (otherwise vertex);
//      bit 1      - EDRAM through fragment shader interlock (otherwise host
//                   render targets), if the device features allow it;
//      bit 2      - native 2x MSAA with attachments;
//      bit 3      - native 2x MSAA without attachments;
//      bits 5..6  - draw resolution scale X - 1 (1 to 3);
//   1: bits 0..5  - SQ_PROGRAM_CNTL vs_num_reg / ps_num_reg;
//      bit 6      - pixel shader: SQ_PROGRAM_CNTL param_gen;
//   2: bits 0..1  - draw resolution scale Y - 1 (1 to 3);
//      bit 2      - every optional device feature (SPIR-V 1.5), otherwise
//                   none (SPIR-V 1.0) but the vertex and fragment stores the
//                   backend requires; bits 3..7 flip single features: clip
//                   distance, cull distance, fragment shader sample
//                   interlock, demote to helper invocation, fragment shader
//                   barycentrics;
//   3..4          - interpolators used by the other stage;
//   vertex shader:
//   5: bits 0..2  - host vertex shader type (vertex, triangle or quad domain,
//                   point or rectangle list as a triangle strip);
//      bits 3..5  - user clip plane count (0 to 6);
//      bit 6      - user clip planes cull only;
//      bit 7      - vertex kill with "and";
//   6: bits 0..1  - PA_CL_CLIP_CNTL ps_ucp_mode;
//      bit 2      - point list (point size output), or for a point list as a
//                   triangle strip, the pixel shader uses PsParamGen;
//      bits 3..4  - tessellation mode, for domain shaders;
//   pixel shader:
//   5: bits 0..3  - SQ_CONTEXT_MISC param_gen_pos;
//      bit 4      - point list (param_gen_point);
//      bits 5..7  - depth / stencil mode (host render targets);
//   6: bit 0, 1   - RT0 color, alpha min / max blend pre-multiplied by the
//                   source alpha (host render targets);
//      bit 2      - draw at native resolution (host render targets);
//      bit 3      - hybrid occlusion query (host render targets);
//      bits 4..7  - centroid interpolation of the used interpolators 0..3.
constexpr size_t kHeaderSize = 8;

SpirvShaderTranslator::Features GetFeatures(const uint8_t* header) {
  SpirvShaderTranslator::Features features((header[2] & (1 << 2)) != 0);
  bool* const flipped[] = {
      &features.clip_distance,
      &features.cull_distance,
      &features.fragment_shader_sample_interlock,
      &features.demote_to_helper_invocation,
      &features.fragment_shader_barycentric,
  };
  for (size_t i = 0; i < std::size(flipped); ++i) {
    if (header[2] & (1 << (3 + i))) {
      *flipped[i] = !*flipped[i];
    }
  }
  // VulkanCommandProcessor refuses devices without them (the translator
  // doesn't handle memory export without them in alloc).
  features.vertex_pipeline_stores_and_atomics = true;
  features.fragment_stores_and_atomics = true;
  return features;
}

struct TranslatorConfig {
  uint8_t features;  // Byte 2 without the resolution scale.
  bool native_2x_msaa_with_attachments;
  bool native_2x_msaa_no_attachments;
  bool edram_fragment_shader_interlock;
  uint32_t draw_resolution_scale_x;
  uint32_t draw_resolution_scale_y;

  auto operator<=>(const TranslatorConfig&) const = default;
};

// One translator per configuration, created once: construction is not what is
// being tested, and TranslateAnalyzedShader resets the state.
SpirvShaderTranslator& GetTranslator(const TranslatorConfig& config, const uint8_t* header) {
  static std::map<TranslatorConfig, std::unique_ptr<SpirvShaderTranslator>> translators;
  std::unique_ptr<SpirvShaderTranslator>& translator = translators[config];
  if (!translator) {
    translator = std::make_unique<SpirvShaderTranslator>(
        GetFeatures(header), config.native_2x_msaa_with_attachments,
        config.native_2x_msaa_no_attachments, config.edram_fragment_shader_interlock,
        config.draw_resolution_scale_x, config.draw_resolution_scale_y);
  }
  return *translator;
}

uint64_t GetVertexShaderModification(const SpirvShader& shader,
                                     const SpirvShaderTranslator& translator,
                                     const SpirvShaderTranslator::Features& features,
                                     uint32_t dynamic_register_count, const uint8_t* header) {
  static constexpr Shader::HostVertexShaderType kHostVertexShaderTypes[] = {
      Shader::HostVertexShaderType::kVertex,
      Shader::HostVertexShaderType::kTriangleDomainCPIndexed,
      Shader::HostVertexShaderType::kTriangleDomainPatchIndexed,
      Shader::HostVertexShaderType::kQuadDomainCPIndexed,
      Shader::HostVertexShaderType::kQuadDomainPatchIndexed,
      Shader::HostVertexShaderType::kPointListAsTriangleStrip,
      Shader::HostVertexShaderType::kRectangleListAsTriangleStrip,
  };
  Shader::HostVertexShaderType host_vertex_shader_type =
      kHostVertexShaderTypes[(header[5] & 0b111) % std::size(kHostVertexShaderTypes)];
  SpirvShaderTranslator::Modification modification(
      translator.GetDefaultVertexShaderModification(dynamic_register_count,
                                                    host_vertex_shader_type));
  modification.vertex.interpolator_mask =
      (header[3] | (uint32_t(header[4]) << 8)) & shader.writes_interpolators();
  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    // xenos::TessellationMode: discrete, continuous, adaptive.
    modification.vertex.tessellation_mode = ((header[6] >> 3) & 0b11) % 3;
  }

  uint32_t user_clip_plane_count = ((header[5] >> 3) & 0b111) % 7;
  bool user_clip_plane_cull = false;
  if (user_clip_plane_count) {
    if ((header[5] & (1 << 6)) && features.cull_distance) {
      user_clip_plane_cull = true;
    } else if (!features.clip_distance) {
      // Culling as the fallback, or no user clip planes.
      user_clip_plane_cull = features.cull_distance;
      if (!features.cull_distance) {
        user_clip_plane_count = 0;
      }
    }
  }
  modification.vertex.user_clip_plane_count = user_clip_plane_count;
  modification.vertex.user_clip_plane_cull = uint32_t(user_clip_plane_cull);
  modification.vertex.point_ps_ucp_mode = header[6] & 0b11;
  bool point_bit = (header[6] & (1 << 2)) != 0;
  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(point_bit);
  } else {
    modification.vertex.output_point_parameters =
        uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) && point_bit);
  }
  modification.vertex.vertex_kill_and =
      uint32_t(features.cull_distance &&
               (shader.writes_point_size_edge_flag_kill_vertex() & 0b100) && (header[5] & (1 << 7)));
  return modification.value;
}

uint64_t GetPixelShaderModification(const SpirvShader& shader,
                                    const SpirvShaderTranslator& translator,
                                    bool edram_fragment_shader_interlock,
                                    uint32_t dynamic_register_count, const uint8_t* header) {
  using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
  SpirvShaderTranslator::Modification modification(
      translator.GetDefaultPixelShaderModification(dynamic_register_count));

  reg::SQ_PROGRAM_CNTL sq_program_cntl = {};
  sq_program_cntl.ps_num_reg = header[1] & 63;
  sq_program_cntl.param_gen = (header[1] >> 6) & 1;
  reg::SQ_CONTEXT_MISC sq_context_misc = {};
  sq_context_misc.param_gen_pos = header[5] & 0b1111;
  uint32_t param_gen_pos;
  uint32_t interpolator_mask =
      shader.GetInterpolatorInputMask(sq_program_cntl, sq_context_misc, param_gen_pos) &
      (header[3] | (uint32_t(header[4]) << 8));
  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid = interpolator_mask & (header[6] >> 4);
  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point = (header[5] >> 4) & 1;
  }

  if (!edram_fragment_shader_interlock) {
    modification.pixel.resolution_scale_native = (header[6] >> 2) & 1;
    auto depth_stencil_mode = DepthStencilMode((header[5] >> 5) % 7);
    bool polygon_offset = depth_stencil_mode == DepthStencilMode::kPolygonOffset ||
                          depth_stencil_mode == DepthStencilMode::kFloat24TruncatingPolygonOffset ||
                          depth_stencil_mode == DepthStencilMode::kFloat24RoundingPolygonOffset;
    if ((polygon_offset && shader.writes_depth()) ||
        (depth_stencil_mode == DepthStencilMode::kEarlyHint &&
         !shader.implicit_early_z_write_allowed())) {
      depth_stencil_mode = DepthStencilMode::kNoModifiers;
    }
    modification.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kOne;
    modification.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;
    if (shader.writes_color_target(0)) {
      if (header[6] & (1 << 0)) {
        modification.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kSrcAlpha;
      }
      if (header[6] & (1 << 1)) {
        modification.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kSrcAlpha;
      }
    }
    if (header[6] & (1 << 3)) {
      modification.pixel.zpd_total = 1;
      // The counter write disables early depth / stencil.
      if (depth_stencil_mode == DepthStencilMode::kEarlyHint) {
        depth_stencil_mode = DepthStencilMode::kNoModifiers;
      }
    }
    modification.pixel.depth_stencil_mode = depth_stencil_mode;
  }
  return modification.value;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < kHeaderSize + sizeof(uint32_t) * 3) {
    return 0;
  }
  const uint8_t* header = data;
  bool is_pixel = (header[0] & (1 << 0)) != 0;
  SpirvShaderTranslator::Features features = GetFeatures(header);
  TranslatorConfig config;
  config.features = header[2] & ~uint8_t(0b11);
  // The render target cache needs both for the interlock path.
  config.edram_fragment_shader_interlock = (header[0] & (1 << 1)) &&
                                           features.fragment_shader_sample_interlock &&
                                           features.fragment_stores_and_atomics;
  config.native_2x_msaa_with_attachments = (header[0] & (1 << 2)) != 0;
  config.native_2x_msaa_no_attachments = (header[0] & (1 << 3)) != 0;
  config.draw_resolution_scale_x = 1 + ((header[0] >> 5) & 0b11) % 3;
  config.draw_resolution_scale_y = 1 + (header[2] & 0b11) % 3;
  uint32_t program_cntl_num_reg = header[1] & 63;

  size_t dword_count = std::min((size - kHeaderSize) / sizeof(uint32_t), kMaxUcodeDwords);
  std::vector<uint32_t> ucode(dword_count);
  std::memcpy(ucode.data(), data + kHeaderSize, dword_count * sizeof(uint32_t));

  SpirvShader shader(is_pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, 0,
                     ucode.data(), ucode.size(), std::endian::big);
  static rex::string::StringBuffer ucode_disasm_buffer;
  shader.AnalyzeUcode(ucode_disasm_buffer);

  SpirvShaderTranslator& translator = GetTranslator(config, header);
  uint32_t dynamic_register_count = shader.GetDynamicAddressableRegisterCount(program_cntl_num_reg);
  uint64_t modification =
      is_pixel ? GetPixelShaderModification(shader, translator,
                                            config.edram_fragment_shader_interlock,
                                            dynamic_register_count, header)
               : GetVertexShaderModification(shader, translator, features,
                                             dynamic_register_count, header);
  Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
  translator.TranslateAnalyzedShader(*translation);
  return 0;
}
