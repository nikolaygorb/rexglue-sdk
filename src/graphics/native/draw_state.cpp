/**
 * @file        graphics/native/draw_state.cpp
 * @brief       Native renderer: a draw's fixed-function and dynamic state from the registers
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Follows the SDK's Vulkan backend for host render targets
 *              (VulkanPipelineCache::GetCurrentStateDescription, EnsurePipelineCreated,
 *              VulkanCommandProcessor::UpdateDynamicState; Xenia, Copyright 2022 Ben Vanik,
 *              BSD license), in place of AoT's Viewport / PolygonOffset / pipeline key code
 *              (ArmyOfTwoRecompilation renderer.cpp, MIT, OpenPortroid).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/draw_state.h>

#include <algorithm>
#include <cstring>

namespace rex::graphics::native {

namespace {

// xenos::BlendFactor (5 bits) to Vulkan; 2, 3 and 17+ are unknown.
constexpr VkBlendFactor kBlendFactors[32] = {
    VK_BLEND_FACTOR_ZERO,                      // 0 kZero
    VK_BLEND_FACTOR_ONE,                       // 1 kOne
    VK_BLEND_FACTOR_ZERO,                      // 2
    VK_BLEND_FACTOR_ZERO,                      // 3
    VK_BLEND_FACTOR_SRC_COLOR,                 // 4 kSrcColor
    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,       // 5 kOneMinusSrcColor
    VK_BLEND_FACTOR_SRC_ALPHA,                 // 6 kSrcAlpha
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,       // 7 kOneMinusSrcAlpha
    VK_BLEND_FACTOR_DST_COLOR,                 // 8 kDstColor
    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,       // 9 kOneMinusDstColor
    VK_BLEND_FACTOR_DST_ALPHA,                 // 10 kDstAlpha
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,       // 11 kOneMinusDstAlpha
    VK_BLEND_FACTOR_CONSTANT_COLOR,            // 12 kConstantColor
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,  // 13 kOneMinusConstantColor
    VK_BLEND_FACTOR_CONSTANT_ALPHA,            // 14 kConstantAlpha
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,  // 15 kOneMinusConstantAlpha
    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,        // 16 kSrcAlphaSaturate
};

// The same for the alpha factors: alpha is scalar, so a colour factor there is
// the matching alpha one.
constexpr VkBlendFactor kAlphaBlendFactors[32] = {
    VK_BLEND_FACTOR_ZERO,                      // 0 kZero
    VK_BLEND_FACTOR_ONE,                       // 1 kOne
    VK_BLEND_FACTOR_ZERO,                      // 2
    VK_BLEND_FACTOR_ZERO,                      // 3
    VK_BLEND_FACTOR_SRC_ALPHA,                 // 4 kSrcColor
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,       // 5 kOneMinusSrcColor
    VK_BLEND_FACTOR_SRC_ALPHA,                 // 6 kSrcAlpha
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,       // 7 kOneMinusSrcAlpha
    VK_BLEND_FACTOR_DST_ALPHA,                 // 8 kDstColor
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,       // 9 kOneMinusDstColor
    VK_BLEND_FACTOR_DST_ALPHA,                 // 10 kDstAlpha
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,       // 11 kOneMinusDstAlpha
    VK_BLEND_FACTOR_CONSTANT_ALPHA,            // 12 kConstantColor
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,  // 13 kOneMinusConstantColor
    VK_BLEND_FACTOR_CONSTANT_ALPHA,            // 14 kConstantAlpha
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,  // 15 kOneMinusConstantAlpha
    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,        // 16 kSrcAlphaSaturate
};

// xenos::BlendOp (3 bits); 5+ are unknown.
constexpr VkBlendOp kBlendOps[8] = {
    VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
    VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD,
};

// xenos::CompareFunction and xenos::StencilOp have the Vulkan order.
uint8_t CompareOp(xenos::CompareFunction function) {
  return uint8_t(uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(function));
}
uint8_t StencilOp(xenos::StencilOp op) {
  return uint8_t(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(op));
}

// Without constantAlphaColorBlendFactors (the Vulkan portability subset), the
// constant alpha factors become the constant colour ones for the colour.
VkBlendFactor PortableColorFactor(VkBlendFactor factor, const DrawStateCaps& caps) {
  if (!caps.constant_alpha_color_blend_factors) {
    if (factor == VK_BLEND_FACTOR_CONSTANT_ALPHA) {
      return VK_BLEND_FACTOR_CONSTANT_COLOR;
    }
    if (factor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA) {
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    }
  }
  return factor;
}

}  // namespace

namespace {

// The host topology of a primitive that arrives converted or expanded (see
// GetPipelineState); false for any other.
bool GetListOrStripTopology(xenos::PrimitiveType primitive_type, uint8_t& topology_out,
                            bool& strip_out) {
  strip_out = false;
  switch (primitive_type) {
    case xenos::PrimitiveType::kLineList:
      topology_out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      return true;
    case xenos::PrimitiveType::kLineStrip:
      topology_out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      strip_out = true;
      return true;
    case xenos::PrimitiveType::kTriangleList:
      topology_out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      return true;
    case xenos::PrimitiveType::kTriangleStrip:
      topology_out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      strip_out = true;
      return true;
    default:
      return false;
  }
}

}  // namespace

bool GetPipelineState(const RegisterFile& regs, const PipelineStateInput& input,
                      const DrawStateCaps& caps, PipelineState& state_out) {
  PipelineState& state = state_out;
  std::memset(&state, 0, sizeof(state));

  // Input assembly. Lists never restart (Vulkan doesn't allow it there).
  bool strip = false;
  if (input.patch_control_points) {
    // Tessellated: patches of control points (or of one patch index).
    if (input.patch_control_points > UINT8_MAX) {
      return false;
    }
    state.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    state.patch_control_points = uint8_t(input.patch_control_points);
  } else if (!GetListOrStripTopology(input.host_primitive_type, state.topology, strip)) {
    return false;
  }
  state.primitive_restart =
      uint8_t(strip && (input.host_primitive_reset || caps.strip_restart_always));

  // Rasterization.
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  state.polygon_mode = VK_POLYGON_MODE_FILL;
  state.cull_mode = VK_CULL_MODE_NONE;
  state.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  if (input.primitive_polygonal) {
    if (pa_su_sc_mode_cntl.cull_front) {
      state.cull_mode |= VK_CULL_MODE_FRONT_BIT;
    }
    if (pa_su_sc_mode_cntl.cull_back) {
      state.cull_mode |= VK_CULL_MODE_BACK_BIT;
    }
    // One mode for both faces: the most special of the faces drawn. Points
    // and lines are both wireframe, as on Direct3D 12.
    if (caps.fill_mode_non_solid &&
        pa_su_sc_mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
      xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
      if (!pa_su_sc_mode_cntl.cull_front) {
        polygon_type = std::min(polygon_type, pa_su_sc_mode_cntl.polymode_front_ptype);
      }
      if (!pa_su_sc_mode_cntl.cull_back) {
        polygon_type = std::min(polygon_type, pa_su_sc_mode_cntl.polymode_back_ptype);
      }
      if (polygon_type != xenos::PolygonType::kTriangles) {
        state.polygon_mode = VK_POLYGON_MODE_LINE;
      }
    }
    if (pa_su_sc_mode_cntl.face) {
      state.front_face = VK_FRONT_FACE_CLOCKWISE;
    }
  }
  // The host clamps depth instead of clipping it with CLIP_DISABLE (or
  // force_depth_clamp).
  state.depth_clamp =
      uint8_t(caps.depth_clamp &&
              (regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable || input.depth_clamp_requested));
  state.rasterizer_discard = uint8_t(input.rasterizer_discard);

  // Depth and stencil.
  if (input.depth_attachment) {
    // Without a depth attachment the bias formula is undefined.
    state.depth_bias = 1;
    reg::RB_DEPTHCONTROL depth_control = input.normalized_depth_control;
    xenos::CompareFunction depth_compare = xenos::CompareFunction::kAlways;
    if (depth_control.z_enable) {
      state.depth_write = uint8_t(depth_control.z_write_enable);
      depth_compare = depth_control.zfunc;
    }
    state.depth_compare = CompareOp(depth_compare);
    state.depth_test =
        uint8_t(state.depth_write || depth_compare != xenos::CompareFunction::kAlways);
    if (depth_control.stencil_enable) {
      state.stencil_test = 1;
      state.stencil_front[0] = StencilOp(depth_control.stencilfail);
      state.stencil_front[1] = StencilOp(depth_control.stencilzpass);
      state.stencil_front[2] = StencilOp(depth_control.stencilzfail);
      state.stencil_front[3] = CompareOp(depth_control.stencilfunc);
      if (input.primitive_polygonal && depth_control.backface_enable) {
        state.stencil_back[0] = StencilOp(depth_control.stencilfail_bf);
        state.stencil_back[1] = StencilOp(depth_control.stencilzpass_bf);
        state.stencil_back[2] = StencilOp(depth_control.stencilzfail_bf);
        state.stencil_back[3] = CompareOp(depth_control.stencilfunc_bf);
      } else {
        std::memcpy(state.stencil_back, state.stencil_front, sizeof(state.stencil_back));
      }
    }
  }

  // Colour blending and write masks of the attached targets.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(input.color_attachments & (uint32_t(1) << i))) {
      continue;
    }
    PipelineState::ColorTarget& target = state.color[i];
    uint32_t write_mask = (input.color_mask >> (i * 4)) & 0b1111;
    VkBlendFactor src_color = VK_BLEND_FACTOR_ONE, dst_color = VK_BLEND_FACTOR_ZERO;
    VkBlendFactor src_alpha = VK_BLEND_FACTOR_ONE, dst_alpha = VK_BLEND_FACTOR_ZERO;
    VkBlendOp color_op = VK_BLEND_OP_ADD, alpha_op = VK_BLEND_OP_ADD;
    if (write_mask) {
      auto blend_control =
          regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
      src_color = PortableColorFactor(kBlendFactors[uint32_t(blend_control.color_srcblend) & 31],
                                      caps);
      dst_color = PortableColorFactor(kBlendFactors[uint32_t(blend_control.color_destblend) & 31],
                                      caps);
      color_op = kBlendOps[uint32_t(blend_control.color_comb_fcn) & 7];
      src_alpha = kAlphaBlendFactors[uint32_t(blend_control.alpha_srcblend) & 31];
      dst_alpha = kAlphaBlendFactors[uint32_t(blend_control.alpha_destblend) & 31];
      alpha_op = kBlendOps[uint32_t(blend_control.alpha_comb_fcn) & 7];
    }
    bool blend = src_color != VK_BLEND_FACTOR_ONE || dst_color != VK_BLEND_FACTOR_ZERO ||
                 color_op != VK_BLEND_OP_ADD || src_alpha != VK_BLEND_FACTOR_ONE ||
                 dst_alpha != VK_BLEND_FACTOR_ZERO || alpha_op != VK_BLEND_OP_ADD;
    if (blend && i == 0) {
      // Already multiplied in by the pixel shader.
      if (input.rt0_rgb_premultiplied) {
        src_color = VK_BLEND_FACTOR_ONE;
      }
      if (input.rt0_alpha_premultiplied) {
        src_alpha = VK_BLEND_FACTOR_ONE;
      }
    }
    target.blend = uint8_t(blend);
    target.src_color = uint8_t(src_color);
    target.dst_color = uint8_t(dst_color);
    target.color_op = uint8_t(color_op);
    target.src_alpha = uint8_t(src_alpha);
    target.dst_alpha = uint8_t(dst_alpha);
    target.alpha_op = uint8_t(alpha_op);
    target.write_mask = uint8_t(write_mask);
  }
  return true;
}

void GetDynamicState(const RegisterFile& regs, const draw_util::ViewportInfo& viewport_info,
                     bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control,
                     VkExtent2D render_area, const DrawStateCaps& caps,
                     DynamicState& state_out, uint32_t scale4) {
  DynamicState& state = state_out;
  std::memset(&state, 0, sizeof(state));

  // Viewport. Vulkan needs a size above 0: an empty one goes outside the
  // framebuffer.
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    state.viewport.x = float(viewport_info.xy_offset[0]);
    state.viewport.y = float(viewport_info.xy_offset[1]);
    state.viewport.width = float(viewport_info.xy_extent[0]);
    state.viewport.height = float(viewport_info.xy_extent[1]);
  } else {
    state.viewport.x = -1.0f;
    state.viewport.y = -1.0f;
    state.viewport.width = 1.0f;
    state.viewport.height = 1.0f;
  }
  state.viewport.minDepth = viewport_info.z_min;
  state.viewport.maxDepth = viewport_info.z_max;

  // Scissor (the window offset is in it), within the render area: Vulkan
  // leaves drawing outside it undefined.
  // Its guest edges to host ones as the render targets' pixels (ScaleEdge).
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, true, false);
  auto host_edge = [scale4](uint32_t edge) {
    return uint32_t(ScaleEdge(int32_t(edge), scale4));
  };
  uint32_t left = std::min(host_edge(scissor.offset[0]), render_area.width);
  uint32_t top = std::min(host_edge(scissor.offset[1]), render_area.height);
  uint32_t right =
      std::min(host_edge(scissor.offset[0] + scissor.extent[0]), render_area.width);
  uint32_t bottom =
      std::min(host_edge(scissor.offset[1] + scissor.extent[1]), render_area.height);
  state.scissor.offset.x = int32_t(left);
  state.scissor.offset.y = int32_t(top);
  state.scissor.extent.width = std::max(right, left) - left;
  state.scissor.extent.height = std::max(bottom, top) - top;

  // Polygon offset of the face drawn, in the guest depth format's units: the
  // unorm24 factor also holds for unorm24 kept as float32 on the host.
  float slope = 0.0f, constant = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, slope, constant);
  bool depth_float24 =
      regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8;
  state.depth_bias_constant =
      constant * (depth_float24 ? draw_util::kD3D10PolygonOffsetFactorFloat24
                                : draw_util::kD3D10PolygonOffsetFactorUnorm24);
  // The slope is per host pixel: times the scale, so the bias over a guest
  // pixel stays the guest's.
  state.depth_bias_slope =
      slope * xenos::kPolygonOffsetScaleSubpixelUnit * (float(scale4) / float(kScale4Min));
  state.depth_bias_force_unorm = 0;

  state.blend_constants[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
  state.blend_constants[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
  state.blend_constants[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
  state.blend_constants[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);

  // Stencil: the back face values with two-sided stencil; without separate
  // front and back masks and references, the back ones only when only back
  // faces are drawn.
  Register front_register = XE_GPU_REG_RB_STENCILREFMASK;
  Register back_register = XE_GPU_REG_RB_STENCILREFMASK;
  if (normalized_depth_control.stencil_enable && primitive_polygonal &&
      normalized_depth_control.backface_enable) {
    if (caps.separate_stencil_mask_ref) {
      back_register = XE_GPU_REG_RB_STENCILREFMASK_BF;
    } else {
      auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
      if (pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
        front_register = XE_GPU_REG_RB_STENCILREFMASK_BF;
        back_register = XE_GPU_REG_RB_STENCILREFMASK_BF;
      }
    }
  }
  auto front = regs.Get<reg::RB_STENCILREFMASK>(front_register);
  auto back = regs.Get<reg::RB_STENCILREFMASK>(back_register);
  state.stencil_compare_mask[0] = front.stencilmask;
  state.stencil_compare_mask[1] = back.stencilmask;
  state.stencil_write_mask[0] = front.stencilwritemask;
  state.stencil_write_mask[1] = back.stencilwritemask;
  state.stencil_reference[0] = front.stencilref;
  state.stencil_reference[1] = back.stencilref;
}

void ScaleViewportInfo(draw_util::ViewportInfo& info, uint32_t scale4) {
  if (scale4 == kScale4Min) {
    return;
  }
  const double scale = double(scale4) * 0.25;
  for (uint32_t i = 0; i < 2; ++i) {
    int64_t guest_0 = info.xy_offset[i];
    int64_t guest_extent = info.xy_extent[i];
    // floor(guest_0 * scale) and ceil(guest_1 * scale), exact in quarters.
    int64_t host_0 = (guest_0 * int64_t(scale4)) >> 2;
    int64_t host_1 = -((-(guest_0 + guest_extent) * int64_t(scale4)) >> 2);
    info.xy_offset[i] = int32_t(host_0);
    info.xy_extent[i] = guest_extent ? uint32_t(host_1 - host_0) : 0;
    if (!guest_extent) {
      // Empty: nothing is drawn, the NDC transform doesn't matter.
      continue;
    }
    // NDC -1...1 was guest_0...guest_1; the host viewport maps it to
    // host_0...host_1. The guest position guest_0 + (ndc + 1) * extent / 2 at
    // the scale, in the new NDC: ndc * k + c.
    double host_extent = double(host_1 - host_0);
    double k = double(guest_extent) * scale / host_extent;
    double c = (2.0 * double(guest_0) * scale + double(guest_extent) * scale -
                2.0 * double(host_0)) /
                   host_extent -
               1.0;
    info.ndc_scale[i] = float(double(info.ndc_scale[i]) * k);
    info.ndc_offset[i] = float(double(info.ndc_offset[i]) * k + c);
  }
}

}  // namespace rex::graphics::native
