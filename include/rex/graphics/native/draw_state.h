/**
 * @file        graphics/native/draw_state.h
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

#pragma once

#include <cstdint>

#include <rex/graphics/native/scale.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::native {

// What the device can do, for the state below.
struct DrawStateCaps {
  bool fill_mode_non_solid = false;
  bool depth_clamp = false;
  bool constant_alpha_color_blend_factors = false;
  bool separate_stencil_mask_ref = false;
  // Metal can't disable primitive restart for strips (MoltenVK): a strip that
  // doesn't restart has no all-ones index, so keeping it on changes nothing.
  bool strip_restart_always = false;
};

// The fixed-function part of a draw's pipeline (the shaders and the render
// pass are keyed apart). Bytes only: compared and hashed as memory.
struct PipelineState {
  // VkPrimitiveTopology.
  uint8_t topology;
  uint8_t primitive_restart;
  // Tessellated (VK_PRIMITIVE_TOPOLOGY_PATCH_LIST): control points per patch.
  uint8_t patch_control_points;
  // VkPolygonMode, VkCullModeFlags, VkFrontFace.
  uint8_t polygon_mode;
  uint8_t cull_mode;
  uint8_t front_face;
  uint8_t depth_clamp;
  // Nothing rasterized: a vertex shader only exporting to memory.
  uint8_t rasterizer_discard;
  // With a depth attachment; the values are dynamic state.
  uint8_t depth_bias;
  uint8_t depth_test;
  uint8_t depth_write;
  // VkCompareOp.
  uint8_t depth_compare;
  uint8_t stencil_test;
  // VkStencilOp fail, pass, depth fail, then the VkCompareOp.
  uint8_t stencil_front[4];
  uint8_t stencil_back[4];
  // By render target index; only the attached ones are filled.
  struct ColorTarget {
    uint8_t blend;
    // VkBlendFactor, VkBlendFactor, VkBlendOp.
    uint8_t src_color, dst_color, color_op;
    uint8_t src_alpha, dst_alpha, alpha_op;
    // VkColorComponentFlags.
    uint8_t write_mask;
  } color[xenos::kMaxColorRenderTargets];
};
static_assert(alignof(PipelineState) == 1, "PipelineState must have no padding");

struct PipelineStateInput {
  // From the primitive processor.
  xenos::PrimitiveType host_primitive_type = xenos::PrimitiveType::kNone;
  bool host_primitive_reset = false;
  bool primitive_polygonal = false;
  reg::RB_DEPTHCONTROL normalized_depth_control = {};
  // RB_COLOR_MASK normalized for the pixel shader and the attached targets (4
  // bits per render target).
  uint32_t color_mask = 0;
  // Colour attachments by render target index; whether depth is attached.
  uint32_t color_attachments = 0;
  bool depth_attachment = false;
  // The pixel shader multiplies the render target 0 source factor in itself
  // (MIN / MAX blending, SpirvShaderTranslator's premultiply modification).
  bool rt0_rgb_premultiplied = false;
  bool rt0_alpha_premultiplied = false;
  // force_depth_clamp.
  bool depth_clamp_requested = false;
  // Rasterization not done (draw_util::IsRasterizationPotentiallyDone):
  // memory export from the vertex shader only.
  bool rasterizer_discard = false;
  // Tessellated: control points per patch (vulkan::TessellationInputControlPoints);
  // 0 otherwise. The topology is then a patch list, whatever the primitive.
  uint32_t patch_control_points = 0;
};

// False for a host primitive type the native renderer doesn't draw (points,
// fans, quads and rectangles arrive converted or expanded; patches are not
// drawn yet).
bool GetPipelineState(const RegisterFile& regs, const PipelineStateInput& input,
                      const DrawStateCaps& caps, PipelineState& state_out);

// The state set with vkCmdSet* before a draw.
struct DynamicState {
  VkViewport viewport;
  VkRect2D scissor;
  float depth_bias_constant;
  float depth_bias_slope;
  // 1: set in constant units (VK_EXT_depth_bias_control FORCE_UNORM, the
  // caller's choice; GetDynamicState leaves it 0). A uint32_t: the struct is
  // compared with memcmp and must have no padding.
  uint32_t depth_bias_force_unorm;
  float blend_constants[4];
  // Front, back.
  uint32_t stencil_compare_mask[2];
  uint32_t stencil_write_mask[2];
  uint32_t stencil_reference[2];
};

// A viewport the SDK computed in guest pixels (GetHostViewportInfo at scale 1)
// to host pixels at scale4: the host viewport takes the host pixels the guest
// one covers, rounded outwards, and NDC XY are remapped so that a guest
// position x lands at x * scale4 / 4 exactly (the guest pixel a host pixel's
// centre is in, as ScaleEdge). At a whole scale the same as GetHostViewportInfo
// at that scale. Z is not changed.
void ScaleViewportInfo(draw_util::ViewportInfo& info, uint32_t scale4);

// render_area: the framebuffer's extent in host pixels; the scissor is kept
// inside it. scale4: the render targets' scale in quarters (the viewport info
// is already scaled); the scissor's edges and the slope bias follow it.
void GetDynamicState(const RegisterFile& regs, const draw_util::ViewportInfo& viewport_info,
                     bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control,
                     VkExtent2D render_area, const DrawStateCaps& caps,
                     DynamicState& state_out, uint32_t scale4 = kScale4Min);

}  // namespace rex::graphics::native
