/**
 * @file        fuzz/shader_dxbc_fuzzer.cpp
 * @brief       libFuzzer target for Xenos microcode analysis, the DXBC
 *              translator and the Direct3D 11 bytecode conversion
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
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <rex/graphics/d3d11/shader_bytecode.h>
#include <rex/graphics/format/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/string/buffer.h>
#include <rex/ui/graphics_provider.h>

#include "last_input.h"

using namespace rex::graphics;

namespace {

// The shader program area of the Xenos is 0x1000 instruction slots of 3 dwords.
constexpr size_t kMaxUcodeDwords = 0x1000 * 3;

// Input layout: kHeaderSize bytes, then the microcode as big-endian dwords, as
// in guest memory. The header only selects states the pipeline caches can
// create (see GetCurrent*ShaderModification in d3d12/pipeline_cache.cpp).
//   0: bit 0      - pixel shader (otherwise vertex);
//      bit 1      - EDRAM through ROV (otherwise host render targets);
//      bit 2      - bindless resources;
//      bit 3      - gamma render targets as unorm8;
//      bit 4      - no 2x MSAA;
//      bits 5..6  - draw resolution scale X - 1 (1 to 3);
//      bit 7      - convert to Shader Model 5.0 for Direct3D 11 (bindful host
//                   render targets only, like the D3D11 backend);
//   1: bits 0..5  - SQ_PROGRAM_CNTL vs_num_reg / ps_num_reg;
//      bit 6      - pixel shader: SQ_PROGRAM_CNTL param_gen;
//      bit 7      - D3D11: shared memory UAV reads aliased to the SRV;
//   2: bits 0..1  - draw resolution scale Y - 1 (1 to 3);
//      bits 2..5  - D3D11 device features: UAVs at every stage (and 64 UAVs),
//                   stencil reference output, ROVs, typed UAV loads;
//   3..4          - interpolators used by the other stage;
//   vertex shader:
//   5: bits 0..2  - host vertex shader type (vertex or a domain shader);
//      bits 3..5  - user clip plane count (0 to 6);
//      bit 6      - user clip planes cull only;
//      bit 7      - vertex kill with "and";
//   6: bits 0..1  - PA_CL_CLIP_CNTL ps_ucp_mode;
//      bit 2      - point list (point size output);
//   pixel shader:
//   5: bits 0..3  - SQ_CONTEXT_MISC param_gen_pos;
//      bit 4      - point list (param_gen_point);
//      bits 5..7  - depth / stencil mode (host render targets);
//   6: bit 0, 1   - RT0 color, alpha min / max blend pre-multiplied by the
//                   source alpha (host render targets);
//      bit 2      - draw at native resolution (host render targets);
//      bit 3      - hybrid occlusion query (host render targets);
//      bits 4..7  - centroid interpolation of the used interpolators 0..3;
//   7:            - D3D11: seed of the texture swizzles (0 - identity).
constexpr size_t kHeaderSize = 8;

struct TranslatorConfig {
  bool edram_rov_used;
  bool bindless_resources_used;
  bool gamma_render_target_as_unorm8;
  bool msaa_2x_supported;
  uint32_t draw_resolution_scale_x;
  uint32_t draw_resolution_scale_y;

  auto operator<=>(const TranslatorConfig&) const = default;
};

// One translator per configuration, created once: construction is not what is
// being tested, and TranslateAnalyzedShader resets the state.
DxbcShaderTranslator& GetTranslator(const TranslatorConfig& config) {
  static std::map<TranslatorConfig, std::unique_ptr<DxbcShaderTranslator>> translators;
  std::unique_ptr<DxbcShaderTranslator>& translator = translators[config];
  if (!translator) {
    translator = std::make_unique<DxbcShaderTranslator>(
        rex::ui::GraphicsProvider::GpuVendorID::kNvidia, config.bindless_resources_used,
        config.edram_rov_used, config.gamma_render_target_as_unorm8, config.msaa_2x_supported,
        config.draw_resolution_scale_x, config.draw_resolution_scale_y);
  }
  return *translator;
}

uint64_t GetVertexShaderModification(const DxbcShader& shader,
                                     const DxbcShaderTranslator& translator,
                                     uint32_t dynamic_register_count, const uint8_t* header) {
  constexpr uint32_t kHostVertexShaderTypeCount =
      uint32_t(Shader::HostVertexShaderType::kDomainEnd);
  auto host_vertex_shader_type =
      Shader::HostVertexShaderType((header[5] & 0b111) % kHostVertexShaderTypeCount);
  DxbcShaderTranslator::Modification modification(
      translator.GetDefaultVertexShaderModification(dynamic_register_count,
                                                    host_vertex_shader_type));
  modification.vertex.interpolator_mask =
      (header[3] | (uint32_t(header[4]) << 8)) & shader.writes_interpolators();
  modification.vertex.user_clip_plane_count = ((header[5] >> 3) & 0b111) % 7;
  modification.vertex.user_clip_plane_cull =
      uint32_t(modification.vertex.user_clip_plane_count && (header[5] & (1 << 6)));
  modification.vertex.vertex_kill_and = uint32_t(
      (shader.writes_point_size_edge_flag_kill_vertex() & 0b100) && (header[5] & (1 << 7)));
  modification.vertex.point_ps_ucp_mode = header[6] & 0b11;
  modification.vertex.output_point_size = uint32_t(
      (shader.writes_point_size_edge_flag_kill_vertex() & 0b001) && (header[6] & (1 << 2)));
  return modification.value;
}

uint64_t GetPixelShaderModification(const DxbcShader& shader,
                                    const DxbcShaderTranslator& translator, bool edram_rov_used,
                                    uint32_t dynamic_register_count, const uint8_t* header) {
  using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
  DxbcShaderTranslator::Modification modification(
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

  if (!edram_rov_used) {
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
      // The counter UAV write disables early depth / stencil.
      if (depth_stencil_mode == DepthStencilMode::kEarlyHint) {
        depth_stencil_mode = DepthStencilMode::kNoModifiers;
      }
    }
    modification.pixel.depth_stencil_mode = depth_stencil_mode;
  }
  return modification.value;
}

// The D3D11 backend's conversion of a translated guest shader (see
// PipelineCache::CreateNativeShader and the texture bindings of
// D3D11CommandProcessor).
void ConvertForD3D11(const DxbcShader& shader, const std::vector<uint8_t>& translated,
                     const uint8_t* header) {
  uint64_t supported_feature_flags = 0;
  if (header[2] & (1 << 2)) {
    supported_feature_flags |=
        dxbc::kShaderFeature0_UAVsAtEveryStage | dxbc::kShaderFeature0_64UAVs;
  }
  if (header[2] & (1 << 3)) {
    supported_feature_flags |= dxbc::kShaderFeature0_StencilRef;
  }
  if (header[2] & (1 << 4)) {
    supported_feature_flags |= dxbc::kShaderFeature0_ROVs;
  }
  if (header[2] & (1 << 5)) {
    supported_feature_flags |= dxbc::kShaderFeature0_TypedUAVLoadAdditionalFormats;
  }
  d3d11::ShaderBytecodeOptions options = d3d11::GetBackendShaderBytecodeOptions(
      supported_feature_flags,
      (supported_feature_flags & dxbc::kShaderFeature0_UAVsAtEveryStage) ? 64 : 8,
      (header[1] >> 7) & 1);

  if (uint32_t seed = header[7]) {
    // D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT.
    constexpr size_t kSrvSlotCount = 128;
    constexpr uint32_t kTexturesStart =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    size_t texture_count = std::min(shader.GetTextureBindingsAfterTranslation().size(),
                                    kSrvSlotCount - kTexturesStart);
    for (size_t i = 0; i < texture_count; ++i) {
      uint32_t random = (seed + uint32_t(i)) * 0x9E3779B1u;
      // Each component: R, G, B, A, 0 or 1.
      uint32_t swizzle = 0;
      for (uint32_t j = 0; j < 4; ++j) {
        swizzle |= ((random >> (8 * j)) % 6) << (3 * j);
      }
      if (swizzle != d3d11::kIdentityTextureSwizzle) {
        options.texture_swizzles.push_back({kTexturesStart + uint32_t(i), swizzle});
      }
    }
  }

  d3d11::ShaderBytecode output;
  std::string error;
  d3d11::ConvertShaderBytecode(translated, options, output, error);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  rex::fuzz::SaveLastInput(data, size);
  if (size < kHeaderSize + sizeof(uint32_t) * 3) {
    return 0;
  }
  const uint8_t* header = data;
  bool is_pixel = (header[0] & (1 << 0)) != 0;
  TranslatorConfig config;
  config.edram_rov_used = (header[0] & (1 << 1)) != 0;
  config.bindless_resources_used = (header[0] & (1 << 2)) != 0;
  config.gamma_render_target_as_unorm8 = (header[0] & (1 << 3)) != 0;
  config.msaa_2x_supported = (header[0] & (1 << 4)) == 0;
  config.draw_resolution_scale_x = 1 + ((header[0] >> 5) & 0b11) % 3;
  config.draw_resolution_scale_y = 1 + (header[2] & 0b11) % 3;
  bool convert_for_d3d11 =
      (header[0] & (1 << 7)) && !config.edram_rov_used && !config.bindless_resources_used;
  uint32_t program_cntl_num_reg = header[1] & 63;

  size_t dword_count = std::min((size - kHeaderSize) / sizeof(uint32_t), kMaxUcodeDwords);
  std::vector<uint32_t> ucode(dword_count);
  std::memcpy(ucode.data(), data + kHeaderSize, dword_count * sizeof(uint32_t));

  DxbcShader shader(is_pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, 0,
                    ucode.data(), ucode.size(), std::endian::big);
  static rex::string::StringBuffer ucode_disasm_buffer;
  shader.AnalyzeUcode(ucode_disasm_buffer);

  DxbcShaderTranslator& translator = GetTranslator(config);
  uint32_t dynamic_register_count = shader.GetDynamicAddressableRegisterCount(program_cntl_num_reg);
  uint64_t modification =
      is_pixel ? GetPixelShaderModification(shader, translator, config.edram_rov_used,
                                            dynamic_register_count, header)
               : GetVertexShaderModification(shader, translator, dynamic_register_count, header);
  Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
  if (translator.TranslateAnalyzedShader(*translation) && convert_for_d3d11) {
    ConvertForD3D11(shader, translation->translated_binary(), header);
  }
  return 0;
}
