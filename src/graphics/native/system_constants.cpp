/**
 * @file        graphics/native/system_constants.cpp
 * @brief       Native renderer: the translated shaders' system constants of a draw
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The SDK's VulkanCommandProcessor::UpdateSystemConstantValues (Xenia, Copyright
 *              2022 Ben Vanik, BSD license) for host render targets.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/system_constants.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/math.h>

namespace rex::graphics::native {

void FillSystemConstants(const RegisterFile& regs, const SystemConstantsInput& input,
                         SpirvShaderTranslator::SystemConstants& out) {
  using Translator = SpirvShaderTranslator;
  std::memset(&out, 0, sizeof(out));
  const PrimitiveProcessor::ProcessingResult& primitive = *input.primitive;
  const draw_util::ViewportInfo& viewport_info = *input.viewport_info;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  // Flags.
  uint32_t flags = 0;
  if (input.shader_32bit_index_dma) {
    flags |= Translator::kSysFlag_VertexIndexLoad;
  }
  if (primitive.index_buffer_type == PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA ||
      primitive.index_buffer_type == PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA ||
      primitive.index_buffer_type == PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted) {
    flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    // Point and rectangle expansion restart only in the built-in strip index
    // buffer, the guest indices don't.
    if (primitive.host_primitive_reset_enabled &&
        primitive.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        primitive.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexReset;
    }
  }
  // W0 division (VTX_XY_FMT, VTX_Z_FMT, VTX_W0_FMT).
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= Translator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= Translator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= Translator::kSysFlag_WNotReciprocal;
  }
  if (input.primitive_polygonal) {
    flags |= Translator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= Translator::kSysFlag_PrimitiveLine;
  }
  flags |= uint32_t(rb_surface_info.msaa_samples) << Translator::kSysFlag_MsaaSamples_Shift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= Translator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << Translator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma targets share the unorm image of k_8_8_8_8: the shader writes the
  // piecewise linear gamma itself.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= Translator::kSysFlag_ConvertColor0ToGamma << i;
    }
  }
  out.flags = flags;

  // Indices the shaders load.
  if (flags & (Translator::kSysFlag_VertexIndexLoad |
               Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)) {
    out.vertex_index_load_address = primitive.guest_index_base;
  }
  out.vertex_index_reset = (flags & Translator::kSysFlag_ComputeOrPrimitiveVertexIndexReset)
                               ? regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx
                               : 0;
  xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 &&
      guest_index_endian != xenos::Endian::kNone && guest_index_endian != xenos::Endian::k8in16) {
    guest_index_endian =
        guest_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
  }
  out.vertex_index_endian = (flags & Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)
                                ? guest_index_endian
                                : primitive.host_shader_index_endian;
  out.line_loop_closing_index = primitive.line_loop_closing_index;
  out.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  out.vertex_index_min = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);
  out.vertex_index_max = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);

  // User clip planes (UCP_ENA_#) unless CLIP_DISABLE, packed: the shader knows
  // only how many.
  if (!pa_cl_clip_cntl.clip_disable) {
    uint32_t plane_count = 0;
    uint32_t planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t plane_index;
    while (rex::bit_scan_forward(planes_remaining, &plane_index)) {
      planes_remaining &= ~(uint32_t(1) << plane_index);
      std::memcpy(out.user_clip_planes[plane_count++],
                  &regs[XE_GPU_REG_PA_CL_UCP_0_X + plane_index * 4], 4 * sizeof(float));
    }
  }

  // Tessellation factors are the register plus 1.
  out.tessellation_factor_range_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  out.tessellation_factor_range_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;

  // To the host's normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    out.ndc_scale[i] = viewport_info.ndc_scale[i];
    out.ndc_offset[i] = viewport_info.ndc_offset[i];
  }
  // The window offset is in the viewport, not in the render target bases.
  out.param_gen_window_offset[0] = 0.0f;
  out.param_gen_window_offset[1] = 0.0f;

  // Point sizes, and a guest pixel in the NDC, for points and lines.
  xenos::PrimitiveType prim_type = vgt_draw_initiator.prim_type;
  if (prim_type == xenos::PrimitiveType::kPointList ||
      prim_type == xenos::PrimitiveType::kLineList ||
      prim_type == xenos::PrimitiveType::kLineStrip ||
      prim_type == xenos::PrimitiveType::kLineLoop) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    out.point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    out.point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    out.point_constant_diameter[0] = float(pa_su_point_size.width) * (2.0f / 16.0f);
    out.point_constant_diameter[1] = float(pa_su_point_size.height) * (2.0f / 16.0f);
    // Diameter (guest pixels) to radius (0.5) times 2 (the NDC spans 2), the
    // viewport being in host pixels.
    out.point_screen_diameter_to_ndc_radius[0] =
        input.resolution_scale / float(std::max(viewport_info.xy_extent[0], uint32_t(1)));
    out.point_screen_diameter_to_ndc_radius[1] =
        input.resolution_scale / float(std::max(viewport_info.xy_extent[1], uint32_t(1)));
  }

  // Texture signedness and integer scaling of the bound textures (none is
  // resolution-scaled at scale 1), and their swizzle where the shader applies
  // it; unused slots unsigned with the identity swizzle.
  constexpr uint32_t kIdentitySwizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  for (uint32_t& swizzles : out.texture_swizzles) {
    swizzles = kIdentitySwizzle | (kIdentitySwizzle << 12);
  }
  if (input.texture_cache) {
    // Stacked textures wrapping their layers (stacked_texture_layer_wrap).
    const uint32_t stacked_layer_wrap_min_layers =
        uint32_t(std::max(REXCVAR_GET(stacked_texture_layer_wrap), int32_t(0)));
    uint32_t textures_remaining = input.used_texture_mask;
    uint32_t texture_index;
    while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(uint32_t(1) << texture_index);
      if (stacked_layer_wrap_min_layers) {
        out.textures_stacked_layer_wrap |=
            uint32_t(texture_util::IsStackedLayerWrapped(regs.GetTextureFetch(texture_index),
                                                         stacked_layer_wrap_min_layers))
            << texture_index;
      }
      out.texture_swizzled_signs[texture_index >> 2] |=
          uint32_t(input.texture_cache->GetActiveTextureSwizzledSigns(texture_index))
          << (8 * (texture_index & 3));
      out.texture_integer_scale_bits[texture_index] =
          input.texture_cache->GetActiveIntegerScaleBits(texture_index);
      if (!input.image_view_format_swizzle) {
        uint32_t& swizzles = out.texture_swizzles[texture_index >> 1];
        uint32_t shift = 12 * (texture_index & 1);
        swizzles = (swizzles & ~(uint32_t(0xFFF) << shift)) |
                   ((input.texture_cache->GetActiveTextureHostSwizzle(texture_index) & 0xFFF)
                    << shift);
      }
    }
  }

  // Alpha test and alpha to coverage.
  out.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  out.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                          ? (rb_colorcontrol.value >> 24) | (uint32_t(1) << 8)
                          : 0;
  // No occlusion counting in the shaders.
  out.zpd_counter_index = UINT32_MAX;

  // The polygon offset the pixel shader adds (its slope already scaled).
  if (input.host_depth_polygon_offset) {
    out.edram_poly_offset_front_scale = input.host_depth_polygon_offset->front_scale;
    out.edram_poly_offset_front_offset = input.host_depth_polygon_offset->front_offset;
    out.edram_poly_offset_back_scale = input.host_depth_polygon_offset->back_scale;
    out.edram_poly_offset_back_offset = input.host_depth_polygon_offset->back_offset;
  }

  // Colour exponent bias; 16-bit fixed targets are stored /32 (the EDRAM range
  // is -32...32), as the SDK does with 1024 / 32767.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    int32_t color_exp_bias = color_info.color_exp_bias;
    float scale = std::ldexp(1.0f, color_exp_bias);
    if (input.fixed16_color_targets & (uint32_t(1) << i)) {
      scale *= 1024.0f / 32767.0f;
    }
    out.color_exp_bias[i] = scale;
  }
}

}  // namespace rex::graphics::native
