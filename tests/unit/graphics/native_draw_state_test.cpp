/**
 * @file        graphics/native_draw_state_test.cpp
 * @brief       Tests for the native renderer's draw state from the registers
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/native/draw_state.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

using namespace rex::graphics;
using namespace rex::graphics::native;

namespace {

void SetFloat(RegisterFile& regs, uint32_t index, float value) {
  std::memcpy(&regs.values[index], &value, sizeof(value));
}

PipelineStateInput TriangleListInput() {
  PipelineStateInput input;
  input.host_primitive_type = xenos::PrimitiveType::kTriangleList;
  input.primitive_polygonal = true;
  return input;
}

reg::RB_BLENDCONTROL Blend(xenos::BlendFactor color_src, xenos::BlendFactor color_dst,
                           xenos::BlendOp color_op, xenos::BlendFactor alpha_src,
                           xenos::BlendFactor alpha_dst, xenos::BlendOp alpha_op) {
  reg::RB_BLENDCONTROL blend = {};
  blend.color_srcblend = color_src;
  blend.color_destblend = color_dst;
  blend.color_comb_fcn = color_op;
  blend.alpha_srcblend = alpha_src;
  blend.alpha_destblend = alpha_dst;
  blend.alpha_comb_fcn = alpha_op;
  return blend;
}

}  // namespace

TEST_CASE("Native draw state: topology and primitive restart", "[graphics][native][draw]") {
  auto regs = std::make_unique<RegisterFile>();
  DrawStateCaps caps;
  PipelineState state;

  PipelineStateInput input = TriangleListInput();
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  CHECK(state.primitive_restart == 0);
  CHECK(state.polygon_mode == VK_POLYGON_MODE_FILL);
  CHECK(state.cull_mode == VK_CULL_MODE_NONE);
  CHECK(state.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE);

  // Restart only on strips, and always there on Metal.
  input.host_primitive_reset = true;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.primitive_restart == 0);
  input.host_primitive_type = xenos::PrimitiveType::kTriangleStrip;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
  CHECK(state.primitive_restart == 1);
  input.host_primitive_reset = false;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.primitive_restart == 0);
  caps.strip_restart_always = true;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.primitive_restart == 1);
  input.host_primitive_type = xenos::PrimitiveType::kLineStrip;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
  CHECK(state.primitive_restart == 1);
  input.host_primitive_type = xenos::PrimitiveType::kLineList;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
  CHECK(state.primitive_restart == 0);

  // Converted or expanded before they get here.
  for (xenos::PrimitiveType type :
       {xenos::PrimitiveType::kPointList, xenos::PrimitiveType::kTriangleFan,
        xenos::PrimitiveType::kRectangleList, xenos::PrimitiveType::kQuadList,
        xenos::PrimitiveType::kLineLoop}) {
    input.host_primitive_type = type;
    CHECK_FALSE(GetPipelineState(*regs, input, caps, state));
  }
}

TEST_CASE("Native draw state: tessellation and rasterizer discard", "[graphics][native][draw]") {
  auto regs = std::make_unique<RegisterFile>();
  DrawStateCaps caps;
  PipelineState state;
  PipelineStateInput input = TriangleListInput();
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.patch_control_points == 0);
  CHECK(state.rasterizer_discard == 0);

  // Patches of control points whatever the primitive, never restarted.
  input.patch_control_points = 3;
  input.host_primitive_reset = true;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST);
  CHECK(state.patch_control_points == 3);
  CHECK(state.primitive_restart == 0);
  for (xenos::PrimitiveType type :
       {xenos::PrimitiveType::kQuadList, xenos::PrimitiveType::kTrianglePatch,
        xenos::PrimitiveType::kQuadPatch}) {
    input.host_primitive_type = type;
    input.patch_control_points = 4;
    REQUIRE(GetPipelineState(*regs, input, caps, state));
    CHECK(state.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST);
    CHECK(state.patch_control_points == 4);
  }

  // Memory export from the vertex shader only.
  input = TriangleListInput();
  input.rasterizer_discard = true;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.rasterizer_discard == 1);
}

TEST_CASE("Native draw state: culling, winding and polygon mode", "[graphics][native][draw]") {
  auto regs = std::make_unique<RegisterFile>();
  DrawStateCaps caps;
  PipelineState state;
  PipelineStateInput input = TriangleListInput();

  reg::PA_SU_SC_MODE_CNTL mode = {};
  mode.cull_back = 1;
  mode.face = 1;
  regs->values[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = mode.value;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.cull_mode == VK_CULL_MODE_BACK_BIT);
  CHECK(state.front_face == VK_FRONT_FACE_CLOCKWISE);

  // Lines have no faces.
  input.host_primitive_type = xenos::PrimitiveType::kLineList;
  input.primitive_polygonal = false;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.cull_mode == VK_CULL_MODE_NONE);
  CHECK(state.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE);

  // Wireframe for the drawn (front) faces, only in dual mode and with the
  // device feature.
  input = TriangleListInput();
  mode.poly_mode = xenos::PolygonModeEnable::kDualMode;
  mode.polymode_front_ptype = xenos::PolygonType::kLines;
  mode.polymode_back_ptype = xenos::PolygonType::kTriangles;
  regs->values[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = mode.value;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.polygon_mode == VK_POLYGON_MODE_FILL);
  caps.fill_mode_non_solid = true;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.polygon_mode == VK_POLYGON_MODE_LINE);
  // The back faces' points don't count while they are culled.
  mode.polymode_front_ptype = xenos::PolygonType::kTriangles;
  mode.polymode_back_ptype = xenos::PolygonType::kPoints;
  regs->values[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = mode.value;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.polygon_mode == VK_POLYGON_MODE_FILL);
  mode.poly_mode = xenos::PolygonModeEnable::kDisabled;
  mode.cull_back = 0;
  regs->values[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = mode.value;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.polygon_mode == VK_POLYGON_MODE_FILL);
}

TEST_CASE("Native draw state: depth and stencil", "[graphics][native][draw]") {
  auto regs = std::make_unique<RegisterFile>();
  DrawStateCaps caps;
  PipelineState state;
  PipelineStateInput input = TriangleListInput();

  reg::RB_DEPTHCONTROL depth = {};
  depth.z_enable = 1;
  depth.z_write_enable = 1;
  depth.zfunc = xenos::CompareFunction::kLessEqual;
  depth.stencil_enable = 1;
  depth.stencilfunc = xenos::CompareFunction::kAlways;
  depth.stencilzpass = xenos::StencilOp::kReplace;
  depth.backface_enable = 1;
  depth.stencilfunc_bf = xenos::CompareFunction::kEqual;
  depth.stencilzfail_bf = xenos::StencilOp::kDecrementWrap;
  input.normalized_depth_control = depth;

  // Nothing without a depth attachment.
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.depth_test == 0);
  CHECK(state.stencil_test == 0);
  CHECK(state.depth_bias == 0);

  input.depth_attachment = true;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.depth_bias == 1);
  CHECK(state.depth_test == 1);
  CHECK(state.depth_write == 1);
  CHECK(state.depth_compare == VK_COMPARE_OP_LESS_OR_EQUAL);
  CHECK(state.stencil_test == 1);
  CHECK(state.stencil_front[0] == VK_STENCIL_OP_KEEP);
  CHECK(state.stencil_front[1] == VK_STENCIL_OP_REPLACE);
  CHECK(state.stencil_front[3] == VK_COMPARE_OP_ALWAYS);
  CHECK(state.stencil_back[2] == VK_STENCIL_OP_DECREMENT_AND_WRAP);
  CHECK(state.stencil_back[3] == VK_COMPARE_OP_EQUAL);

  // Lines: the back face is the front one.
  input.host_primitive_type = xenos::PrimitiveType::kLineList;
  input.primitive_polygonal = false;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(std::memcmp(state.stencil_back, state.stencil_front, sizeof(state.stencil_back)) == 0);

  // Without Z, the test always passes and doesn't write: no depth test.
  input = TriangleListInput();
  input.depth_attachment = true;
  depth.z_enable = 0;
  depth.stencil_enable = 0;
  input.normalized_depth_control = depth;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.depth_test == 0);
  CHECK(state.depth_write == 0);
  CHECK(state.depth_compare == VK_COMPARE_OP_ALWAYS);
  CHECK(state.stencil_test == 0);
}

TEST_CASE("Native draw state: blending and write masks", "[graphics][native][draw]") {
  auto regs = std::make_unique<RegisterFile>();
  DrawStateCaps caps;
  caps.constant_alpha_color_blend_factors = true;
  PipelineState state;
  PipelineStateInput input = TriangleListInput();
  input.color_attachments = 0b0011;
  input.color_mask = 0x00F7;

  regs->values[XE_GPU_REG_RB_BLENDCONTROL0] =
      Blend(xenos::BlendFactor::kSrcAlpha, xenos::BlendFactor::kOneMinusSrcAlpha,
            xenos::BlendOp::kAdd, xenos::BlendFactor::kSrcColor,
            xenos::BlendFactor::kOneMinusDstColor, xenos::BlendOp::kRevSubtract)
          .value;
  regs->values[XE_GPU_REG_RB_BLENDCONTROL1] =
      Blend(xenos::BlendFactor::kOne, xenos::BlendFactor::kZero, xenos::BlendOp::kAdd,
            xenos::BlendFactor::kOne, xenos::BlendFactor::kZero, xenos::BlendOp::kAdd)
          .value;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.color[0].blend == 1);
  CHECK(state.color[0].src_color == VK_BLEND_FACTOR_SRC_ALPHA);
  CHECK(state.color[0].dst_color == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
  CHECK(state.color[0].color_op == VK_BLEND_OP_ADD);
  // Colour factors in the alpha slot are the alpha ones.
  CHECK(state.color[0].src_alpha == VK_BLEND_FACTOR_SRC_ALPHA);
  CHECK(state.color[0].dst_alpha == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA);
  CHECK(state.color[0].alpha_op == VK_BLEND_OP_REVERSE_SUBTRACT);
  CHECK(state.color[0].write_mask == 0x7);
  // 1 * src + 0 * dst: no blending.
  CHECK(state.color[1].blend == 0);
  CHECK(state.color[1].write_mask == 0xF);
  // Not attached: untouched.
  CHECK(state.color[2].write_mask == 0);
  CHECK(state.color[2].src_color == 0);

  // A masked-out target never blends.
  input.color_mask = 0x00F0;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.color[0].blend == 0);
  CHECK(state.color[0].write_mask == 0);
}

TEST_CASE("Native draw state: constant alpha and premultiplied factors",
          "[graphics][native][draw]") {
  auto regs = std::make_unique<RegisterFile>();
  PipelineState state;
  PipelineStateInput input = TriangleListInput();
  input.color_attachments = 0b0001;
  input.color_mask = 0xF;

  regs->values[XE_GPU_REG_RB_BLENDCONTROL0] =
      Blend(xenos::BlendFactor::kConstantAlpha, xenos::BlendFactor::kOneMinusConstantAlpha,
            xenos::BlendOp::kAdd, xenos::BlendFactor::kConstantAlpha,
            xenos::BlendFactor::kZero, xenos::BlendOp::kAdd)
          .value;
  DrawStateCaps caps;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.color[0].src_color == VK_BLEND_FACTOR_CONSTANT_COLOR);
  CHECK(state.color[0].dst_color == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR);
  CHECK(state.color[0].src_alpha == VK_BLEND_FACTOR_CONSTANT_ALPHA);
  caps.constant_alpha_color_blend_factors = true;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.color[0].src_color == VK_BLEND_FACTOR_CONSTANT_ALPHA);

  // MAX with a source alpha factor: the shader multiplies it in.
  regs->values[XE_GPU_REG_RB_BLENDCONTROL0] =
      Blend(xenos::BlendFactor::kSrcAlpha, xenos::BlendFactor::kOne, xenos::BlendOp::kMax,
            xenos::BlendFactor::kSrcAlpha, xenos::BlendFactor::kOne, xenos::BlendOp::kMax)
          .value;
  input.rt0_rgb_premultiplied = true;
  REQUIRE(GetPipelineState(*regs, input, caps, state));
  CHECK(state.color[0].blend == 1);
  CHECK(state.color[0].src_color == VK_BLEND_FACTOR_ONE);
  CHECK(state.color[0].src_alpha == VK_BLEND_FACTOR_SRC_ALPHA);
  CHECK(state.color[0].color_op == VK_BLEND_OP_MAX);
}

TEST_CASE("Native dynamic state: scissor inside the render area", "[graphics][native][draw]") {
  auto regs = std::make_unique<RegisterFile>();
  DrawStateCaps caps;
  draw_util::ViewportInfo viewport = {};
  viewport.xy_extent[0] = 1280;
  viewport.xy_extent[1] = 720;
  viewport.z_max = 1.0f;
  reg::RB_SURFACE_INFO surface = {};
  surface.surface_pitch = 1280;
  regs->values[XE_GPU_REG_RB_SURFACE_INFO] = surface.value;
  reg::PA_SC_WINDOW_SCISSOR_TL tl = {};
  tl.tl_x = 100;
  tl.tl_y = 50;
  tl.window_offset_disable = 1;
  reg::PA_SC_WINDOW_SCISSOR_BR br = {};
  br.br_x = 1500;
  br.br_y = 1000;
  regs->values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = tl.value;
  regs->values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = br.value;

  DynamicState state;
  GetDynamicState(*regs, viewport, true, reg::RB_DEPTHCONTROL{}, VkExtent2D{1280, 720}, caps,
                  state);
  CHECK(state.viewport.width == 1280.0f);
  CHECK(state.viewport.height == 720.0f);
  CHECK(state.viewport.maxDepth == 1.0f);
  // Clamped to the pitch, then to the framebuffer.
  CHECK(state.scissor.offset.x == 100);
  CHECK(state.scissor.offset.y == 50);
  CHECK(state.scissor.extent.width == 1180);
  CHECK(state.scissor.extent.height == 670);

  // Wholly outside: empty, not negative.
  tl.tl_y = 800;
  regs->values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = tl.value;
  GetDynamicState(*regs, viewport, true, reg::RB_DEPTHCONTROL{}, VkExtent2D{1280, 720}, caps,
                  state);
  CHECK(state.scissor.offset.y == 720);
  CHECK(state.scissor.extent.height == 0);

  // An empty viewport goes outside the framebuffer with a valid size.
  viewport.xy_extent[1] = 0;
  GetDynamicState(*regs, viewport, true, reg::RB_DEPTHCONTROL{}, VkExtent2D{1280, 720}, caps,
                  state);
  CHECK(state.viewport.x == -1.0f);
  CHECK(state.viewport.width == 1.0f);
  CHECK(state.viewport.height == 1.0f);
}

TEST_CASE("Native dynamic state: resolution scale", "[graphics][native][draw]") {
  auto regs = std::make_unique<RegisterFile>();
  DrawStateCaps caps;
  draw_util::ViewportInfo viewport = {};
  viewport.xy_extent[0] = 2560;
  viewport.xy_extent[1] = 1440;
  viewport.z_max = 1.0f;
  reg::RB_SURFACE_INFO surface = {};
  surface.surface_pitch = 1280;
  regs->values[XE_GPU_REG_RB_SURFACE_INFO] = surface.value;
  reg::PA_SC_WINDOW_SCISSOR_TL tl = {};
  tl.tl_x = 100;
  tl.tl_y = 50;
  tl.window_offset_disable = 1;
  reg::PA_SC_WINDOW_SCISSOR_BR br = {};
  br.br_x = 1500;
  br.br_y = 1000;
  regs->values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = tl.value;
  regs->values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = br.value;
  reg::PA_SU_SC_MODE_CNTL mode = {};
  mode.poly_offset_front_enable = 1;
  regs->values[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = mode.value;
  SetFloat(*regs, XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE, 16.0f);

  // 2x: guest edges doubled (the pitch clamp first), then the framebuffer's.
  DynamicState state;
  GetDynamicState(*regs, viewport, true, reg::RB_DEPTHCONTROL{}, VkExtent2D{2560, 1440}, caps,
                  state, 8);
  CHECK(state.scissor.offset.x == 200);
  CHECK(state.scissor.offset.y == 100);
  CHECK(state.scissor.extent.width == 2360);
  CHECK(state.scissor.extent.height == 1340);
  // The slope bias is per host pixel.
  CHECK(state.depth_bias_slope == 2.0f);

  // 1.5x: the first host pixel whose centre is at or after the guest edge.
  GetDynamicState(*regs, viewport, true, reg::RB_DEPTHCONTROL{}, VkExtent2D{1920, 1080}, caps,
                  state, 6);
  CHECK(state.scissor.offset.x == 150);
  CHECK(state.scissor.offset.y == 75);
  CHECK(state.scissor.extent.width == 1920 - 150);
  CHECK(state.depth_bias_slope == 1.5f);
}

TEST_CASE("Native dynamic state: stencil faces and polygon offset", "[graphics][native][draw]") {
  auto regs = std::make_unique<RegisterFile>();
  draw_util::ViewportInfo viewport = {};
  reg::RB_STENCILREFMASK front = {};
  front.stencilref = 0x12;
  front.stencilmask = 0x34;
  front.stencilwritemask = 0x56;
  reg::RB_STENCILREFMASK back = {};
  back.stencilref = 0x78;
  back.stencilmask = 0x9A;
  back.stencilwritemask = 0xBC;
  regs->values[XE_GPU_REG_RB_STENCILREFMASK] = front.value;
  regs->values[XE_GPU_REG_RB_STENCILREFMASK_BF] = back.value;
  reg::RB_DEPTHCONTROL depth = {};
  depth.stencil_enable = 1;
  depth.backface_enable = 1;

  DrawStateCaps caps;
  caps.separate_stencil_mask_ref = true;
  DynamicState state;
  GetDynamicState(*regs, viewport, true, depth, VkExtent2D{64, 64}, caps, state);
  CHECK(state.stencil_reference[0] == 0x12);
  CHECK(state.stencil_compare_mask[0] == 0x34);
  CHECK(state.stencil_write_mask[0] == 0x56);
  CHECK(state.stencil_reference[1] == 0x78);
  CHECK(state.stencil_compare_mask[1] == 0x9A);
  CHECK(state.stencil_write_mask[1] == 0xBC);

  // One value for both faces: the back one only when only back faces are drawn.
  caps.separate_stencil_mask_ref = false;
  GetDynamicState(*regs, viewport, true, depth, VkExtent2D{64, 64}, caps, state);
  CHECK(state.stencil_reference[1] == 0x12);
  reg::PA_SU_SC_MODE_CNTL mode = {};
  mode.cull_front = 1;
  regs->values[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = mode.value;
  GetDynamicState(*regs, viewport, true, depth, VkExtent2D{64, 64}, caps, state);
  CHECK(state.stencil_reference[0] == 0x78);
  CHECK(state.stencil_reference[1] == 0x78);

  // Polygon offset of the front face, in unorm24 or float24 units.
  mode.cull_front = 0;
  mode.poly_offset_front_enable = 1;
  regs->values[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = mode.value;
  const float offset = 1.0f / 1024.0f;
  SetFloat(*regs, XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET, offset);
  SetFloat(*regs, XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE, 16.0f);
  GetDynamicState(*regs, viewport, true, depth, VkExtent2D{64, 64}, caps, state);
  CHECK(state.depth_bias_constant == offset * draw_util::kD3D10PolygonOffsetFactorUnorm24);
  // In 1/16 pixel units on the console.
  CHECK(state.depth_bias_slope == 1.0f);
  // Constant units are the command processor's choice (the device extension).
  CHECK(state.depth_bias_force_unorm == 0);
  reg::RB_DEPTH_INFO depth_info = {};
  depth_info.depth_format = xenos::DepthRenderTargetFormat::kD24FS8;
  regs->values[XE_GPU_REG_RB_DEPTH_INFO] = depth_info.value;
  GetDynamicState(*regs, viewport, true, depth, VkExtent2D{64, 64}, caps, state);
  CHECK(state.depth_bias_constant == offset * draw_util::kD3D10PolygonOffsetFactorFloat24);
  CHECK(state.depth_bias_constant == 16384.0f);
}

TEST_CASE("Native viewport: guest pixels to host pixels at a scale", "[graphics][native][draw]") {
  auto make = [](int32_t offset, uint32_t extent, float ndc_scale, float ndc_offset) {
    draw_util::ViewportInfo info = {};
    info.xy_offset[0] = offset;
    info.xy_extent[0] = extent;
    info.xy_offset[1] = 0;
    info.xy_extent[1] = 720;
    info.ndc_scale[0] = ndc_scale;
    info.ndc_offset[0] = ndc_offset;
    info.ndc_scale[1] = 1.0f;
    info.ndc_offset[1] = 0.0f;
    info.ndc_scale[2] = 1.0f;
    info.ndc_offset[2] = 0.0f;
    return info;
  };
  // A guest NDC x to the host pixel it lands on.
  auto host_x = [](const draw_util::ViewportInfo& info, float ndc) {
    float host_ndc = ndc * info.ndc_scale[0] + info.ndc_offset[0];
    return float(info.xy_offset[0]) + (host_ndc + 1.0f) * 0.5f * float(info.xy_extent[0]);
  };

  SECTION("whole scale: as GetHostViewportInfo at it") {
    draw_util::ViewportInfo info = make(3, 7, 0.7f, 0.1f);
    ScaleViewportInfo(info, 8);
    CHECK(info.xy_offset[0] == 6);
    CHECK(info.xy_extent[0] == 14);
    CHECK(info.ndc_scale[0] == 0.7f);
    CHECK(info.ndc_offset[0] == 0.1f);
    CHECK(info.xy_extent[1] == 1440);
  }
  SECTION("1x unchanged") {
    draw_util::ViewportInfo info = make(3, 7, 0.7f, 0.1f);
    ScaleViewportInfo(info, 4);
    CHECK(info.xy_offset[0] == 3);
    CHECK(info.xy_extent[0] == 7);
  }
  SECTION("1.5x: the full screen") {
    draw_util::ViewportInfo info = make(0, 1280, 1.0f, 0.0f);
    ScaleViewportInfo(info, 6);
    CHECK(info.xy_offset[0] == 0);
    CHECK(info.xy_extent[0] == 1920);
    CHECK(info.xy_extent[1] == 1080);
    CHECK(host_x(info, -1.0f) == 0.0f);
    CHECK(host_x(info, 1.0f) == 1920.0f);
  }
  SECTION("1.5x: odd edges round outwards, positions stay exact") {
    // Guest 1...4 is host 1.5...6.
    draw_util::ViewportInfo info = make(1, 3, 1.0f, 0.0f);
    ScaleViewportInfo(info, 6);
    CHECK(info.xy_offset[0] == 1);
    CHECK(info.xy_extent[0] == 5);
    CHECK(std::abs(host_x(info, -1.0f) - 1.5f) < 1.0e-5f);
    CHECK(std::abs(host_x(info, 1.0f) - 6.0f) < 1.0e-5f);
    CHECK(std::abs(host_x(info, 0.0f) - 3.75f) < 1.0e-5f);
  }
  SECTION("1.25x: a negative offset") {
    // Guest -3...2 is host -3.75...2.5.
    draw_util::ViewportInfo info = make(-3, 5, 1.0f, 0.0f);
    ScaleViewportInfo(info, 5);
    CHECK(info.xy_offset[0] == -4);
    CHECK(info.xy_extent[0] == 7);
    CHECK(std::abs(host_x(info, -1.0f) + 3.75f) < 1.0e-5f);
    CHECK(std::abs(host_x(info, 1.0f) - 2.5f) < 1.0e-5f);
  }
  SECTION("empty stays empty") {
    draw_util::ViewportInfo info = make(5, 0, 1.0f, 0.0f);
    ScaleViewportInfo(info, 6);
    CHECK(info.xy_extent[0] == 0);
  }
}
