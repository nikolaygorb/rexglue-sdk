/**
 * @file        graphics/d3d11/render_target_cache.cpp
 * @brief       Host render target cache of the experimental Direct3D 11 backend
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/render_target_cache.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <span>
#include <string>

#include <rex/assert.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/d3d11/pipeline_cache.h>
#include <rex/graphics/d3d11/shader_bytecode.h>
#include <rex/graphics/d3d11/texture_cache.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(d3d11_resolve_into_textures, false, "GPU/D3D11",
                    "Also copy resolved 32bpp k_8_8_8_8 surfaces straight from the render target "
                    "into their existing textures (as vulkan_resolve_into_textures), so they "
                    "aren't loaded again from memory. Unscaled only");

namespace rex::graphics::d3d11 {

namespace shaders {
#include "../shaders/bytecode/d3d11_5_0/clear_uint2_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fullscreen_cw_vs.h"
#include "../shaders/bytecode/d3d11_5_0/passthrough_position_xy_vs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_dxt3aas1111_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_fast_32bpp_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_fast_64bpp_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_full_128bpp_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_full_128bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_full_16bpp_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_full_16bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_full_32bpp_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_full_32bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_full_64bpp_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_full_64bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_full_8bpp_cs.h"
#include "../shaders/bytecode/d3d11_5_0/resolve_full_8bpp_scaled_cs.h"
}  // namespace shaders

namespace {
struct ResolveCopyShaderCode {
  const void* unscaled;
  size_t unscaled_size;
  const void* scaled;
  size_t scaled_size;
};
// In the order of draw_util::ResolveCopyShaderIndex, as D3D12's
// kResolveCopyShaders.
#define REX_D3D11_RESOLVE_COPY_SHADER(name) \
  {shaders::name##_cs, sizeof(shaders::name##_cs), shaders::name##_scaled_cs, \
   sizeof(shaders::name##_scaled_cs)}
const ResolveCopyShaderCode kResolveCopyShaders[size_t(
    draw_util::ResolveCopyShaderIndex::kCount)] = {
    REX_D3D11_RESOLVE_COPY_SHADER(resolve_fast_32bpp_1x2xmsaa),
    REX_D3D11_RESOLVE_COPY_SHADER(resolve_fast_32bpp_4xmsaa),
    REX_D3D11_RESOLVE_COPY_SHADER(resolve_fast_64bpp_1x2xmsaa),
    REX_D3D11_RESOLVE_COPY_SHADER(resolve_fast_64bpp_4xmsaa),
    REX_D3D11_RESOLVE_COPY_SHADER(resolve_full_8bpp),
    REX_D3D11_RESOLVE_COPY_SHADER(resolve_full_16bpp),
    REX_D3D11_RESOLVE_COPY_SHADER(resolve_full_32bpp),
    REX_D3D11_RESOLVE_COPY_SHADER(resolve_full_64bpp),
    REX_D3D11_RESOLVE_COPY_SHADER(resolve_full_128bpp),
    {shaders::resolve_dxt3aas1111_cs, sizeof(shaders::resolve_dxt3aas1111_cs), nullptr, 0},
};
#undef REX_D3D11_RESOLVE_COPY_SHADER
}  // namespace

D3D11RenderTargetCache::~D3D11RenderTargetCache() { Shutdown(true); }

bool D3D11RenderTargetCache::Initialize() {
  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  ID3D11Device* device = provider.GetDevice();

  gamma_render_target_as_unorm16_ = REXCVAR_GET(gamma_render_target_as_unorm16);
  depth_float24_round_ = REXCVAR_GET(depth_float24_round);
  depth_float24_convert_in_pixel_shader_ = REXCVAR_GET(depth_float24_convert_in_pixel_shader);

  // Check if 2x MSAA is supported or needs to be emulated with 4x MSAA instead,
  // with the formats of the D3D12 backend's check.
  msaa_2x_supported_ = REXCVAR_GET(native_2x_msaa);
  if (msaa_2x_supported_) {
    static const DXGI_FORMAT kRenderTargetDXGIFormats[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_SNORM,
        DXGI_FORMAT_R32G32_FLOAT,
        DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R16G16_SNORM,
        DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_D24_UNORM_S8_UINT,
        // For ownership transfer.
        DXGI_FORMAT_R16G16B16A16_UINT,
        DXGI_FORMAT_R32G32_UINT,
        DXGI_FORMAT_R16G16_UINT,
        DXGI_FORMAT_R32_UINT,
        // Gamma as linear.
        DXGI_FORMAT_R16G16B16A16_UNORM,
    };
    for (DXGI_FORMAT format : kRenderTargetDXGIFormats) {
      if (format == DXGI_FORMAT_R16G16B16A16_UNORM && !gamma_render_target_as_unorm16_) {
        continue;
      }
      UINT quality_levels = 0;
      if (FAILED(device->CheckMultisampleQualityLevels(format, 2, &quality_levels)) ||
          !quality_levels) {
        msaa_2x_supported_ = false;
        break;
      }
    }
    if (!msaa_2x_supported_) {
      REXGPU_WARN(
          "2x MSAA is not supported, emulated via top-left and bottom-right samples of 4x MSAA");
    }
  }

  use_stencil_reference_output_ =
      REXCVAR_GET(native_stencil_value_output) && provider.IsPSSpecifiedStencilReferenceSupported();
  RenderTargetShaders::Config shaders_config;
  shaders_config.msaa_2x_supported = msaa_2x_supported_;
  shaders_config.use_stencil_reference_output = use_stencil_reference_output_;
  shaders_config.gamma_render_target_as_unorm16 = gamma_render_target_as_unorm16_;
  shaders_config.draw_resolution_scale_x = draw_resolution_scale_x();
  shaders_config.draw_resolution_scale_y = draw_resolution_scale_y();
  shaders_config.edram_tile_count = edram_tile_count();
  shaders_config.depth_float24_round = depth_float24_round_;
  shaders_config.depth_float24_convert_in_pixel_shader = depth_float24_convert_in_pixel_shader_;
  shaders_ = std::make_unique<RenderTargetShaders>(shaders_config);

  // Objects for clears by drawing.
  if (FAILED(device->CreateVertexShader(shaders::fullscreen_cw_vs, sizeof(shaders::fullscreen_cw_vs),
                                        nullptr, &clear_vs_)) ||
      FAILED(device->CreatePixelShader(shaders::clear_uint2_ps, sizeof(shaders::clear_uint2_ps),
                                       nullptr, &clear_uint2_ps_))) {
    REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the clear shaders");
    Shutdown();
    return false;
  }
  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.ByteWidth = 16;
  constants_desc.Usage = D3D11_USAGE_DEFAULT;
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (FAILED(device->CreateBuffer(&constants_desc, nullptr, &clear_uint2_constants_))) {
    REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the clear constant buffer");
    Shutdown();
    return false;
  }
  D3D11_DEPTH_STENCIL_DESC depth_stencil_desc = {};
  depth_stencil_desc.DepthEnable = TRUE;
  depth_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  depth_stencil_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  depth_stencil_desc.StencilEnable = TRUE;
  depth_stencil_desc.StencilReadMask = 0xFF;
  depth_stencil_desc.StencilWriteMask = 0xFF;
  depth_stencil_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_REPLACE;
  depth_stencil_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_REPLACE;
  depth_stencil_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
  depth_stencil_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
  depth_stencil_desc.BackFace = depth_stencil_desc.FrontFace;
  D3D11_DEPTH_STENCIL_DESC color_depth_stencil_desc = {};
  color_depth_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  color_depth_stencil_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  color_depth_stencil_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  color_depth_stencil_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
  color_depth_stencil_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
  color_depth_stencil_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
  color_depth_stencil_desc.BackFace = color_depth_stencil_desc.FrontFace;
  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  rasterizer_desc.ScissorEnable = TRUE;
  D3D11_BLEND_DESC blend_desc = {};
  blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
  blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device->CreateDepthStencilState(&depth_stencil_desc, &clear_depth_stencil_state_)) ||
      FAILED(device->CreateDepthStencilState(&color_depth_stencil_desc,
                                             &clear_color_depth_stencil_state_)) ||
      FAILED(device->CreateRasterizerState(&rasterizer_desc, &clear_rasterizer_state_)) ||
      FAILED(device->CreateBlendState(&blend_desc, &clear_blend_state_))) {
    REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the clear states");
    Shutdown();
    return false;
  }

  // Ownership transfers: rectangles in NDC of a 16384x16384 viewport.
  D3D11_INPUT_ELEMENT_DESC transfer_input_element = {
      "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
  if (FAILED(device->CreateVertexShader(shaders::passthrough_position_xy_vs,
                                        sizeof(shaders::passthrough_position_xy_vs), nullptr,
                                        &transfer_vs_)) ||
      FAILED(device->CreateInputLayout(&transfer_input_element, 1,
                                       shaders::passthrough_position_xy_vs,
                                       sizeof(shaders::passthrough_position_xy_vs),
                                       &transfer_input_layout_))) {
    REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the transfer vertex shader");
    Shutdown();
    return false;
  }
  D3D11_RASTERIZER_DESC transfer_rasterizer_desc = rasterizer_desc;
  transfer_rasterizer_desc.ScissorEnable = FALSE;
  // As the D3D12 transfer pipelines.
  D3D11_DEPTH_STENCIL_DESC transfer_depth_desc = {};
  transfer_depth_desc.DepthEnable = TRUE;
  transfer_depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  transfer_depth_desc.DepthFunc = REXCVAR_GET(depth_transfer_not_equal_test)
                                      ? D3D11_COMPARISON_NOT_EQUAL
                                      : D3D11_COMPARISON_ALWAYS;
  transfer_depth_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  transfer_depth_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
  transfer_depth_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
  transfer_depth_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
  if (use_stencil_reference_output_) {
    transfer_depth_desc.StencilEnable = TRUE;
    transfer_depth_desc.StencilWriteMask = UINT8_MAX;
    transfer_depth_desc.FrontFace.StencilDepthFailOp = REXCVAR_GET(depth_transfer_not_equal_test)
                                                           ? D3D11_STENCIL_OP_REPLACE
                                                           : D3D11_STENCIL_OP_KEEP;
    transfer_depth_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
  }
  transfer_depth_desc.BackFace = transfer_depth_desc.FrontFace;
  D3D11_DEPTH_STENCIL_DESC transfer_stencil_desc = {};
  transfer_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  transfer_stencil_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  transfer_stencil_desc.StencilEnable = TRUE;
  transfer_stencil_desc.StencilReadMask = UINT8_MAX;
  transfer_stencil_desc.StencilWriteMask = UINT8_MAX;
  transfer_stencil_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  transfer_stencil_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
  transfer_stencil_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
  transfer_stencil_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
  transfer_stencil_desc.BackFace = transfer_stencil_desc.FrontFace;
  if (FAILED(device->CreateRasterizerState(&transfer_rasterizer_desc,
                                           &transfer_rasterizer_state_)) ||
      FAILED(device->CreateDepthStencilState(&transfer_depth_desc, &transfer_depth_state_)) ||
      FAILED(device->CreateDepthStencilState(&transfer_stencil_desc,
                                             &transfer_stencil_clear_state_))) {
    REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the transfer states");
    Shutdown();
    return false;
  }
  for (uint32_t i = 0; i < 8; ++i) {
    transfer_stencil_desc.StencilWriteMask = UINT8(1) << i;
    if (FAILED(device->CreateDepthStencilState(&transfer_stencil_desc,
                                               &transfer_stencil_bit_states_[i]))) {
      REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the transfer states");
      Shutdown();
      return false;
    }
  }
  for (Microsoft::WRL::ComPtr<ID3D11Buffer>& transfer_constants : transfer_constants_) {
    if (FAILED(device->CreateBuffer(&constants_desc, nullptr, &transfer_constants))) {
      REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the transfer constant buffers");
      Shutdown();
      return false;
    }
  }

  // Resolves. The EDRAM buffer has the scaled layout; a raw view addresses at
  // most 2^27 dwords.
  uint64_t edram_buffer_size =
      uint64_t(edram_size_bytes()) * (draw_resolution_scale_x() * draw_resolution_scale_y());
  if (edram_buffer_size > (uint64_t(4) << D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP)) {
    REXGPU_ERROR(
        "D3D11RenderTargetCache: The EDRAM buffer of {} MB for the {}x{} draw resolution scale is "
        "larger than a buffer view can address",
        edram_buffer_size >> 20, draw_resolution_scale_x(), draw_resolution_scale_y());
    Shutdown();
    return false;
  }
  D3D11_BUFFER_DESC edram_buffer_desc = {};
  edram_buffer_desc.ByteWidth = UINT(edram_buffer_size);
  edram_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  edram_buffer_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  edram_buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  D3D11_SHADER_RESOURCE_VIEW_DESC edram_srv_desc = {};
  edram_srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  edram_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  edram_srv_desc.BufferEx.NumElements = edram_buffer_desc.ByteWidth >> 2;
  edram_srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  D3D11_UNORDERED_ACCESS_VIEW_DESC edram_uav_desc = {};
  edram_uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  edram_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  edram_uav_desc.Buffer.NumElements = edram_buffer_desc.ByteWidth >> 2;
  edram_uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(device->CreateBuffer(&edram_buffer_desc, nullptr, &edram_buffer_)) ||
      FAILED(device->CreateShaderResourceView(edram_buffer_.Get(), &edram_srv_desc,
                                              &edram_buffer_raw_srv_)) ||
      FAILED(device->CreateUnorderedAccessView(edram_buffer_.Get(), &edram_uav_desc,
                                               &edram_buffer_raw_uav_))) {
    REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the EDRAM buffer");
    Shutdown();
    return false;
  }
  for (Microsoft::WRL::ComPtr<ID3D11Buffer>& dump_constants : dump_constants_) {
    if (FAILED(device->CreateBuffer(&constants_desc, nullptr, &dump_constants))) {
      REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the dump constant buffers");
      Shutdown();
      return false;
    }
  }
  D3D11_BUFFER_DESC resolve_constants_desc = constants_desc;
  resolve_constants_desc.ByteWidth =
      UINT(rex::align(sizeof(draw_util::ResolveCopyShaderConstants), size_t(16)));
  if (FAILED(device->CreateBuffer(&resolve_constants_desc, nullptr, &resolve_copy_constants_))) {
    REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the resolve constant buffer");
    Shutdown();
    return false;
  }
  // Unscaled shaders for copies to the shared memory (also of fully native
  // sources with resolution scaling), scaled ones for copies to the scaled
  // resolve memory.
  bool draw_resolution_scaled = IsDrawResolutionScaled();
  for (size_t i = 0; i < size_t(draw_util::ResolveCopyShaderIndex::kCount); ++i) {
    const ResolveCopyShaderCode& shader_code = kResolveCopyShaders[i];
    if (FAILED(device->CreateComputeShader(shader_code.unscaled, shader_code.unscaled_size,
                                           nullptr, &resolve_copy_shaders_[i])) ||
        (draw_resolution_scaled && shader_code.scaled &&
         FAILED(device->CreateComputeShader(shader_code.scaled, shader_code.scaled_size, nullptr,
                                            &resolve_copy_scaled_shaders_[i])))) {
      REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the {} resolve copy shader",
                   draw_util::resolve_copy_shader_info[i].debug_name);
      Shutdown();
      return false;
    }
  }

  InitializeCommon();
  return true;
}

void D3D11RenderTargetCache::Shutdown(bool from_destructor) {
  temporary_guest_output_ = nullptr;
  retired_render_targets_.clear();
  std::memset(bound_render_targets_, 0, sizeof(bound_render_targets_));
  are_bound_render_targets_valid_ = false;

  resolve_copy_constants_.Reset();
  for (Microsoft::WRL::ComPtr<ID3D11ComputeShader>& shader : resolve_copy_shaders_) {
    shader.Reset();
  }
  for (Microsoft::WRL::ComPtr<ID3D11ComputeShader>& shader : resolve_copy_scaled_shaders_) {
    shader.Reset();
  }
  dump_shaders_.clear();
  for (Microsoft::WRL::ComPtr<ID3D11Buffer>& dump_constants : dump_constants_) {
    dump_constants.Reset();
  }
  edram_buffer_raw_uav_.Reset();
  edram_buffer_raw_srv_.Reset();
  edram_buffer_.Reset();
  host_depth_copy_srv_.Reset();
  host_depth_copy_.Reset();
  transfer_pixel_shaders_.clear();
  transfer_vertex_buffer_.Reset();
  transfer_vertex_buffer_size_ = 0;
  transfer_vertex_buffer_position_ = 0;
  for (Microsoft::WRL::ComPtr<ID3D11Buffer>& transfer_constants : transfer_constants_) {
    transfer_constants.Reset();
  }
  for (Microsoft::WRL::ComPtr<ID3D11DepthStencilState>& state : transfer_stencil_bit_states_) {
    state.Reset();
  }
  transfer_stencil_clear_state_.Reset();
  transfer_depth_state_.Reset();
  transfer_rasterizer_state_.Reset();
  transfer_input_layout_.Reset();
  transfer_vs_.Reset();

  clear_blend_state_.Reset();
  clear_rasterizer_state_.Reset();
  clear_color_depth_stencil_state_.Reset();
  clear_depth_stencil_state_.Reset();
  clear_uint2_constants_.Reset();
  clear_uint2_ps_.Reset();
  clear_vs_.Reset();
  shaders_.reset();

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D11RenderTargetCache::ClearCache() {
  temporary_guest_output_ = nullptr;
  std::memset(bound_render_targets_, 0, sizeof(bound_render_targets_));
  are_bound_render_targets_valid_ = false;
  RenderTargetCache::ClearCache();
}

void D3D11RenderTargetCache::BeginFrame() {
  temporary_guest_output_ = nullptr;
  retired_render_targets_.clear();
  RenderTargetCache::BeginFrame();
}

bool D3D11RenderTargetCache::Update(bool is_rasterization_done,
                                    reg::RB_DEPTHCONTROL normalized_depth_control,
                                    uint32_t normalized_color_mask, const Shader& vertex_shader,
                                    int32_t window_offset_tiles) {
  if (!RenderTargetCache::Update(is_rasterization_done, normalized_depth_control,
                                 normalized_color_mask, vertex_shader, window_offset_tiles)) {
    return false;
  }
  RenderTarget* const* depth_and_color_render_targets = last_update_accumulated_render_targets();
  bool transfers_done = PerformTransfers(1 + xenos::kMaxColorRenderTargets,
                                         depth_and_color_render_targets, last_update_transfers(),
                                         nullptr);
  BindRenderTargets(depth_and_color_render_targets);
  if (!transfers_done) {
    return false;
  }

  const auto* color_0 = static_cast<const D3D11RenderTarget*>(depth_and_color_render_targets[1]);
  if (color_0) {
    uint32_t area = color_0->key().GetWidth() * color_0->height();
    if (!temporary_guest_output_ ||
        area >= temporary_guest_output_->key().GetWidth() * temporary_guest_output_->height()) {
      temporary_guest_output_ = color_0;
    }
  }
  return true;
}

// Follows D3D12RenderTargetCache::Resolve for host render targets.
bool D3D11RenderTargetCache::Resolve(const memory::Memory& memory,
                                     D3D11SharedMemory& shared_memory,
                                     D3D11TextureCache& texture_cache,
                                     uint32_t& written_address_out, uint32_t& written_length_out,
                                     reg::RB_COPY_DEST_INFO& copy_dest_info_out,
                                     bool& written_scaled_out) {
  written_address_out = 0;
  written_length_out = 0;
  written_scaled_out = false;

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  draw_util::ResolveInfo resolve_info;
  bool fixed_16_truncated_to_minus_1_to_1 = IsFixed16TruncatedToMinus1To1();
  if (!draw_util::GetResolveInfo(register_file(), memory, draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), edram_tile_count(),
                                 fixed_16_truncated_to_minus_1_to_1,
                                 fixed_16_truncated_to_minus_1_to_1, resolve_info)) {
    return false;
  }
  // Normalized by GetResolveInfo to the format the extent was calculated for.
  copy_dest_info_out = resolve_info.copy_dest_info;

  // Nothing to copy/clear.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

  // Copy at 1x1 if all render targets owning the source are native.
  bool copy_native = false;
  if (resolve_info.copy_dest_extent_length && draw_resolution_scaled) {
    uint32_t dump_base;
    uint32_t dump_row_length_used;
    uint32_t dump_rows;
    uint32_t dump_pitch;
    resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
    copy_native = IsResolveSourceNativeOnly(dump_base, dump_row_length_used, dump_rows, dump_pitch);
    if (copy_native) {
      // Redo the resolve info at 1x1 for the unscaled copy shader.
      if (!draw_util::GetResolveInfo(register_file(), memory, 1, 1, edram_tile_count(),
                                     fixed_16_truncated_to_minus_1_to_1,
                                     fixed_16_truncated_to_minus_1_to_1, resolve_info)) {
        return false;
      }
    }
  }
  if (resolve_info.copy_dest_extent_length && draw_resolution_scaled && !copy_native &&
      resolve_info.rb_copy_control.copy_command == xenos::CopyCommand::kConvertTo1111) {
    REXGPU_WARN("Resolve copy command 2 is not supported with resolution scaling");
    // Skip the copy, but still perform the clears.
    resolve_info.copy_dest_extent_length = 0;
  }

  // Copying.
  bool copied = true;
  if (resolve_info.copy_dest_extent_length) {
    bool copy_dest_scaled = draw_resolution_scaled && !copy_native;
    draw_util::ResolveCopyShaderConstants copy_shader_constants;
    uint32_t copy_group_count_x, copy_group_count_y;
    draw_util::ResolveCopyShaderIndex copy_shader = resolve_info.GetCopyShader(
        copy_native ? 1 : draw_resolution_scale_x(), copy_native ? 1 : draw_resolution_scale_y(),
        copy_shader_constants, copy_group_count_x, copy_group_count_y);
    assert_true(copy_group_count_x && copy_group_count_y);
    ID3D11ComputeShader* copy_compute_shader =
        copy_shader == draw_util::ResolveCopyShaderIndex::kUnknown ? nullptr
        : copy_dest_scaled ? resolve_copy_scaled_shaders_[size_t(copy_shader)].Get()
                           : resolve_copy_shaders_[size_t(copy_shader)].Get();
    if (copy_compute_shader) {
      // Dump the current contents of the render targets owning the affected
      // range to the EDRAM buffer.
      uint32_t dump_base;
      uint32_t dump_row_length_used;
      uint32_t dump_rows;
      uint32_t dump_pitch;
      resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
      // Make sure there is memory to write to. Scaled, committing starting
      // with the beginning of the potentially written extent, but binding from
      // the base, which the copy shader writes relative to.
      ID3D11UnorderedAccessView* dest_uav = nullptr;
      if (copy_dest_scaled) {
        if (texture_cache.EnsureScaledResolveMemoryCommitted(
                resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length) &&
            texture_cache.MakeScaledResolveRangeCurrent(
                resolve_info.copy_dest_base, resolve_info.copy_dest_extent_start -
                                                 resolve_info.copy_dest_base +
                                                 resolve_info.copy_dest_extent_length)) {
          dest_uav = texture_cache.GetCurrentScaledResolveRangeUAV();
        }
      } else if (shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                            resolve_info.copy_dest_extent_length)) {
        dest_uav = shared_memory.GetRawUAV();
      }
      if (!DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                             copy_native)) {
        REXGPU_ERROR("D3D11RenderTargetCache: Failed to dump host render targets for resolve");
        copied = false;
      } else if (dest_uav) {
        ID3D11DeviceContext* context =
            command_processor_.GetD3D11Provider().GetImmediateContext();
        // The shared memory may be bound as an input of draws, or as a UAV of
        // memexporting ones, which a compute UAV binding conflicts with.
        ID3D11ShaderResourceView* null_srv = nullptr;
        ID3D11UnorderedAccessView* null_uav = nullptr;
        context->VSSetShaderResources(0, 1, &null_srv);
        context->PSSetShaderResources(0, 1, &null_srv);
        context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
        context->OMSetRenderTargetsAndUnorderedAccessViews(
            D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
            xenos::kMaxColorRenderTargets, 1, &null_uav, nullptr);
        // The scaled shaders read only the DestRelative part, at the start.
        context->UpdateSubresource(resolve_copy_constants_.Get(), 0, nullptr,
                                   &copy_shader_constants, 0, 0);
        ID3D11Buffer* constants = resolve_copy_constants_.Get();
        context->CSSetConstantBuffers(0, 1, &constants);
        ID3D11ShaderResourceView* edram_srv = edram_buffer_raw_srv_.Get();
        context->CSSetShaderResources(0, 1, &edram_srv);
        context->CSSetUnorderedAccessViews(0, 1, &dest_uav, nullptr);
        context->CSSetShader(copy_compute_shader, nullptr, 0);
        context->Dispatch(copy_group_count_x, copy_group_count_y, 1);
        context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        context->CSSetShaderResources(0, 1, &null_srv);
        command_processor_.InvalidateDrawState();

        // Invalidate textures and mark the range as scaled if needed.
        texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                          resolve_info.copy_dest_extent_length, copy_dest_scaled);
        // Before any clear of the render target that follows.
        if (REXCVAR_GET(d3d11_resolve_into_textures) && !draw_resolution_scaled) {
          CopyResolveIntoTexture(resolve_info, copy_shader, texture_cache);
        }
        written_address_out = resolve_info.copy_dest_extent_start;
        written_length_out = resolve_info.copy_dest_extent_length;
        written_scaled_out = copy_dest_scaled;
      } else {
        REXGPU_ERROR(
            "D3D11RenderTargetCache: Failed to obtain the resolve destination memory region");
        copied = false;
      }
    }
  }

  if (!resolve_info.IsClearingDepth() && !resolve_info.IsClearingColor()) {
    return copied;
  }
  Transfer::Rectangle clear_rectangle;
  RenderTarget* clear_render_targets[2];
  // If PrepareHostRenderTargetsResolveClear returns false, may be just an empty
  // region (success) or an error - don't care.
  if (!PrepareHostRenderTargetsResolveClear(resolve_info, clear_rectangle, clear_render_targets[0],
                                            clear_transfers_[0], clear_render_targets[1],
                                            clear_transfers_[1])) {
    return copied;
  }
  bool transfers_done =
      PerformTransfers(2, clear_render_targets, clear_transfers_, &clear_rectangle);
  uint64_t clear_values[2];
  clear_values[0] = resolve_info.rb_depth_clear;
  // For 64bpp formats, RB_COLOR_CLEAR_LO is the lower 32 bits of the packed
  // clear value. RB_COLOR_CLEAR is the upper 32 bits and, for 32bpp formats,
  // the whole value.
  clear_values[1] = resolve_info.color_edram_info.format_is_64bpp
                        ? resolve_info.rb_color_clear_lo |
                              (uint64_t(resolve_info.rb_color_clear) << 32)
                        : resolve_info.rb_color_clear;
  for (uint32_t i = 0; i < 2; ++i) {
    auto* render_target = static_cast<D3D11RenderTarget*>(clear_render_targets[i]);
    if (!render_target) {
      continue;
    }
    // All render targets of a clear share the pitch and the scale class.
    RenderTargetKey key = render_target->key();
    D3D11_RECT rect;
    rect.left = LONG(clear_rectangle.x_pixels * GetKeyScaleX(key));
    rect.top = LONG(clear_rectangle.y_pixels * GetKeyScaleY(key));
    rect.right =
        LONG((clear_rectangle.x_pixels + clear_rectangle.width_pixels) * GetKeyScaleX(key));
    rect.bottom =
        LONG((clear_rectangle.y_pixels + clear_rectangle.height_pixels) * GetKeyScaleY(key));
    ClearRenderTarget(*render_target, clear_values[i], rect);
  }
  return copied && transfers_done;
}

ID3D11Texture2D* D3D11RenderTargetCache::GetTemporaryGuestOutput(DXGI_FORMAT& format_out,
                                                                 uint32_t& width_out,
                                                                 uint32_t& height_out) const {
  if (!temporary_guest_output_) {
    return nullptr;
  }
  RenderTargetKey key = temporary_guest_output_->key();
  format_out = shaders_->GetColorDrawDXGIFormat(key.GetColorFormat());
  width_out = key.GetWidth() * GetKeyScaleX(key);
  height_out = temporary_guest_output_->height() * GetKeyScaleY(key);
  return temporary_guest_output_->texture();
}

RenderTargetCache::RenderTarget* D3D11RenderTargetCache::CreateRenderTarget(RenderTargetKey key,
                                                                            uint32_t height) {
  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  ID3D11Device* device = provider.GetDevice();

  D3D11_TEXTURE2D_DESC texture_desc = {};
  texture_desc.Width = key.GetWidth() * GetKeyScaleX(key);
  texture_desc.Height = height * GetKeyScaleY(key);
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  if (key.is_depth) {
    texture_desc.Format = RenderTargetShaders::GetDepthResourceDXGIFormat(key.GetDepthFormat());
  } else {
    texture_desc.Format = shaders_->GetColorResourceDXGIFormat(key.GetColorFormat());
  }
  if (texture_desc.Format == DXGI_FORMAT_UNKNOWN) {
    REXGPU_ERROR("D3D11RenderTargetCache: Unknown {} render target format {}",
                 key.is_depth ? "depth" : "color", uint32_t(key.resource_format));
    return nullptr;
  }
  if (key.msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_supported_) {
    texture_desc.SampleDesc.Count = 4;
  } else {
    texture_desc.SampleDesc.Count = UINT(1) << UINT(key.msaa_samples);
  }
  texture_desc.Usage = D3D11_USAGE_DEFAULT;
  texture_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE |
      (key.is_depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET);
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &texture))) {
    REXGPU_ERROR("D3D11RenderTargetCache: Failed to create a {}x{} {} render target",
                 texture_desc.Width, texture_desc.Height, key.GetDebugName());
    return nullptr;
  }
  bool multisample = texture_desc.SampleDesc.Count > 1;

  ID3D11DeviceContext* context = provider.GetImmediateContext();
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv, rtv_load_separate;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv, srv_stencil;
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  if (multisample) {
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
  }
  if (key.is_depth) {
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
    dsv_desc.Format = RenderTargetShaders::GetDepthDSVDXGIFormat(key.GetDepthFormat());
    dsv_desc.ViewDimension =
        multisample ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateDepthStencilView(texture.Get(), &dsv_desc, &dsv))) {
      REXGPU_ERROR("D3D11RenderTargetCache: Failed to create a depth/stencil view");
      return nullptr;
    }
    srv_desc.Format = RenderTargetShaders::GetDepthSRVDepthDXGIFormat(key.GetDepthFormat());
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &srv_desc, &srv))) {
      REXGPU_ERROR("D3D11RenderTargetCache: Failed to create a depth shader resource view");
      return nullptr;
    }
    srv_desc.Format = RenderTargetShaders::GetDepthSRVStencilDXGIFormat(key.GetDepthFormat());
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &srv_desc, &srv_stencil))) {
      REXGPU_ERROR("D3D11RenderTargetCache: Failed to create a stencil shader resource view");
      return nullptr;
    }
    // Zeroed like a new Direct3D 12 committed resource, for determinism.
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
  } else {
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = shaders_->GetColorDrawDXGIFormat(key.GetColorFormat());
    rtv_desc.ViewDimension =
        multisample ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateRenderTargetView(texture.Get(), &rtv_desc, &rtv))) {
      REXGPU_ERROR("D3D11RenderTargetCache: Failed to create a render target view");
      return nullptr;
    }
    // Ownership transfer RTV.
    DXGI_FORMAT load_format = shaders_->GetColorOwnershipTransferDXGIFormat(key.GetColorFormat());
    if (rtv_desc.Format != load_format) {
      rtv_desc.Format = load_format;
      if (FAILED(device->CreateRenderTargetView(texture.Get(), &rtv_desc, &rtv_load_separate))) {
        REXGPU_ERROR("D3D11RenderTargetCache: Failed to create a render target view");
        return nullptr;
      }
    }
    srv_desc.Format = load_format;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &srv_desc, &srv))) {
      REXGPU_ERROR("D3D11RenderTargetCache: Failed to create a shader resource view");
      return nullptr;
    }
    const float zero[4] = {};
    context->ClearRenderTargetView(rtv_load_separate ? rtv_load_separate.Get() : rtv.Get(), zero);
  }

  return new D3D11RenderTarget(key, height, std::move(texture), std::move(rtv),
                               std::move(rtv_load_separate), std::move(dsv), std::move(srv),
                               std::move(srv_stencil));
}

void D3D11RenderTargetCache::RenderTargetReused(RenderTarget& render_target) {
  auto& d3d11_render_target = static_cast<D3D11RenderTarget&>(render_target);
  ID3D11DeviceContext* context = command_processor_.GetD3D11Provider().GetImmediateContext();
  // Zeroed like a new one.
  if (render_target.key().is_depth) {
    context->ClearDepthStencilView(d3d11_render_target.dsv(),
                                   D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
  } else {
    const float zero[4] = {};
    context->ClearRenderTargetView(d3d11_render_target.rtv_load(), zero);
  }
  are_bound_render_targets_valid_ = false;
}

RenderTargetCache::RenderTarget* D3D11RenderTargetCache::GrowRenderTarget(
    RenderTarget* source, uint32_t height, const std::vector<Transfer>& transfers) {
  auto* dest = static_cast<D3D11RenderTarget*>(CreateRenderTarget(source->key(), height));
  if (!dest) {
    return nullptr;
  }
  RenderTarget* dest_base = dest;
  if (!transfers.empty() && !PerformTransfers(1, &dest_base, &transfers, nullptr)) {
    // Commands using the new resource may have been queued anyway.
    retired_render_targets_.emplace_back(dest);
    return nullptr;
  }
  for (RenderTarget*& bound : bound_render_targets_) {
    if (bound == source) {
      are_bound_render_targets_valid_ = false;
    }
  }
  if (temporary_guest_output_ == source) {
    temporary_guest_output_ = dest;
  }
  // Direct3D 11 keeps the resources alive while the GPU uses them, but the
  // common code may still refer to the source in this Update.
  retired_render_targets_.emplace_back(source);
  return dest;
}

bool D3D11RenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24FS8) {
    return !depth_float24_convert_in_pixel_shader_;
  }
  return false;
}

// Follows D3D12RenderTargetCache::PerformTransfersAndResolveClears, without the
// merging of draws. The host depth source equal to the destination is read
// from a copy of the destination taken before the transfers, instead of the
// EDRAM buffer copy of D3D12 (a different key of the same shaders).
bool D3D11RenderTargetCache::PerformTransfers(uint32_t render_target_count,
                                              RenderTarget* const* render_targets,
                                              const std::vector<Transfer>* render_target_transfers,
                                              const Transfer::Rectangle* resolve_clear_rectangle) {
  bool any_transfers = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    if (render_targets[i] && !render_target_transfers[i].empty()) {
      any_transfers = true;
      break;
    }
  }
  if (!any_transfers) {
    return true;
  }
  bool success = true;
  D3D11CommandProcessor::ScopedGpuSection gpu_section(command_processor_,
                                                      D3D11CommandProcessor::GpuSection::kTransfers);

  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  ID3D11DeviceContext* context = provider.GetImmediateContext();
  context->IASetInputLayout(transfer_input_layout_.Get());
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(transfer_vs_.Get(), nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);
  context->GSSetShader(nullptr, nullptr, 0);
  context->RSSetState(transfer_rasterizer_state_.Get());
  // NDC are passed directly, the viewport is the maximum host render target
  // size, a power of two for exact pixel coordinates.
  D3D11_VIEWPORT transfer_viewport = {0.0f,
                                      0.0f,
                                      float(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION),
                                      float(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION),
                                      0.0f,
                                      1.0f};
  context->RSSetViewports(1, &transfer_viewport);
  ID3D11Buffer* transfer_constants[3];
  for (uint32_t i = 0; i < 3; ++i) {
    transfer_constants[i] = transfer_constants_[i].Get();
  }
  context->PSSetConstantBuffers(0, 3, transfer_constants);
  ID3D11ShaderResourceView* const null_srvs[RenderTargetShaders::kTransferSRVRegisterCount] = {};

  using TransferShaderKey = RenderTargetShaders::TransferShaderKey;
  using TransferMode = RenderTargetShaders::TransferMode;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    const std::vector<Transfer>& current_transfers = render_target_transfers[i];
    if (!dest_rt || current_transfers.empty()) {
      continue;
    }
    auto& dest_d3d11_rt = *static_cast<D3D11RenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_d3d11_rt.key();

    // The destination may have been a source of the previous transfers.
    context->PSSetShaderResources(0, RenderTargetShaders::kTransferSRVRegisterCount, null_srvs);

    ID3D11ShaderResourceView* host_depth_copy_srv = nullptr;
    if (dest_rt_key.is_depth) {
      for (const Transfer& transfer : current_transfers) {
        if (transfer.host_depth_source == dest_rt) {
          host_depth_copy_srv = CopyHostDepth(dest_d3d11_rt);
          break;
        }
      }
      context->OMSetRenderTargets(0, nullptr, dest_d3d11_rt.dsv());
    } else {
      ID3D11RenderTargetView* rtv = dest_d3d11_rt.rtv_load();
      context->OMSetRenderTargets(1, &rtv, nullptr);
    }
    // Using sample 0 as 0 and 3 as 1 for 2x instead.
    context->OMSetBlendState(nullptr, nullptr,
                             (dest_rt_key.msaa_samples == xenos::MsaaSamples::k2X &&
                              !msaa_2x_supported_)
                                 ? 0b1001
                                 : UINT_MAX);

    bool need_stencil_bit_draws = dest_rt_key.is_depth && !use_stencil_reference_output_;
    uint32_t dest_pitch_tiles = dest_rt_key.GetPitchTiles();
    for (const Transfer& transfer : current_transfers) {
      Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
      uint32_t rectangle_count = transfer.GetRectangles(dest_rt_key, edram_tile_count(), rectangles,
                                                        resolve_clear_rectangle);
      if (!rectangle_count) {
        continue;
      }
      uint32_t vertex_count = UploadTransferRectangles(rectangles, rectangle_count, dest_rt_key);
      if (!vertex_count) {
        success = false;
        continue;
      }
      assert_not_null(transfer.source);
      auto& source_d3d11_rt = *static_cast<D3D11RenderTarget*>(transfer.source);
      RenderTargetKey source_rt_key = source_d3d11_rt.key();
      auto* host_depth_source_d3d11_rt = static_cast<D3D11RenderTarget*>(transfer.host_depth_source);
      ID3D11ShaderResourceView* host_depth_srv = nullptr;
      if (host_depth_source_d3d11_rt) {
        host_depth_srv = host_depth_source_d3d11_rt == &dest_d3d11_rt
                             ? host_depth_copy_srv
                             : host_depth_source_d3d11_rt->srv();
        if (!host_depth_srv) {
          success = false;
          continue;
        }
      }

      if (need_stencil_bit_draws) {
        // Clear the stencil to 0 where it will be loaded - the bits that need
        // to be 1 are set by discarding samples.
        context->PSSetShader(nullptr, nullptr, 0);
        context->OMSetDepthStencilState(transfer_stencil_clear_state_.Get(), 0);
        context->Draw(vertex_count, 0);
      }

      for (uint32_t j = 0; j <= uint32_t(need_stencil_bit_draws); ++j) {
        // j == 0 - color or depth, j == 1 - stencil bits (never with host
        // depth).
        TransferShaderKey shader_key;
        shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
        shader_key.dest_resource_format = dest_rt_key.resource_format;
        shader_key.dest_scale_native = dest_rt_key.scale_native;
        shader_key.source_msaa_samples = source_rt_key.msaa_samples;
        shader_key.source_resource_format = source_rt_key.resource_format;
        shader_key.source_scale_native = source_rt_key.scale_native;
        bool use_host_depth = !j && host_depth_source_d3d11_rt;
        shader_key.host_depth_source_msaa_samples =
            use_host_depth ? host_depth_source_d3d11_rt->key().msaa_samples
                           : xenos::MsaaSamples::k1X;
        if (j) {
          shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToStencilBit
                                                   : TransferMode::kColorToStencilBit;
        } else if (dest_rt_key.is_depth) {
          if (use_host_depth) {
            shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthAndHostDepthToDepth
                                                     : TransferMode::kColorAndHostDepthToDepth;
          } else {
            shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToDepth
                                                     : TransferMode::kColorToDepth;
          }
        } else {
          shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToColor
                                                   : TransferMode::kColorToColor;
        }
        ID3D11PixelShader* pixel_shader = GetOrCreateTransferPixelShader(shader_key);
        if (!pixel_shader) {
          success = false;
          break;
        }
        const RenderTargetShaders::TransferModeInfo& mode_info =
            RenderTargetShaders::kTransferModes[size_t(shader_key.mode)];
        uint32_t used = RenderTargetShaders::kTransferUsedRootParameters[size_t(
            use_stencil_reference_output_ ? mode_info.root_signature_with_stencil_ref
                                          : mode_info.root_signature_no_stencil_ref)];

        ID3D11ShaderResourceView* srvs[RenderTargetShaders::kTransferSRVRegisterCount] = {};
        if (used & RenderTargetShaders::kTransferUsedRootParameterColorSRVBit) {
          srvs[RenderTargetShaders::kTransferSRVRegisterColor] = source_d3d11_rt.srv();
        }
        if (used & RenderTargetShaders::kTransferUsedRootParameterDepthSRVBit) {
          srvs[RenderTargetShaders::kTransferSRVRegisterDepth] = source_d3d11_rt.srv();
        }
        if (used & RenderTargetShaders::kTransferUsedRootParameterStencilSRVBit) {
          srvs[RenderTargetShaders::kTransferSRVRegisterStencil] = source_d3d11_rt.srv_stencil();
        }
        if (used & RenderTargetShaders::kTransferUsedRootParameterHostDepthSRVBit) {
          srvs[RenderTargetShaders::kTransferSRVRegisterHostDepth] = host_depth_srv;
        }
        context->PSSetShaderResources(0, RenderTargetShaders::kTransferSRVRegisterCount, srvs);
        if (used & RenderTargetShaders::kTransferUsedRootParameterAddressConstantBit) {
          RenderTargetShaders::TransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_pitch_tiles;
          address_constant.source_pitch = source_rt_key.GetPitchTiles();
          address_constant.source_to_dest =
              (uint32_t(dest_rt_key.base_tiles) - uint32_t(source_rt_key.base_tiles)) &
              (edram_tile_count() - 1);
          address_constant.source_next_period = source_rt_key.next_period;
          uint32_t constant_data[4] = {address_constant.constant};
          context->UpdateSubresource(
              transfer_constants_[RenderTargetShaders::kTransferCBVRegisterAddress].Get(), 0,
              nullptr, constant_data, 0, 0);
        }
        if (used & RenderTargetShaders::kTransferUsedRootParameterHostDepthAddressConstantBit) {
          RenderTargetKey host_depth_source_rt_key = host_depth_source_d3d11_rt->key();
          RenderTargetShaders::TransferAddressConstant host_depth_address_constant;
          host_depth_address_constant.dest_pitch = dest_pitch_tiles;
          host_depth_address_constant.source_pitch = host_depth_source_rt_key.GetPitchTiles();
          host_depth_address_constant.source_to_dest =
              (uint32_t(dest_rt_key.base_tiles) - uint32_t(host_depth_source_rt_key.base_tiles)) &
              (edram_tile_count() - 1);
          uint32_t constant_data[4] = {host_depth_address_constant.constant};
          context->UpdateSubresource(
              transfer_constants_[RenderTargetShaders::kTransferCBVRegisterHostDepthAddress].Get(),
              0, nullptr, constant_data, 0, 0);
        }
        context->PSSetShader(pixel_shader, nullptr, 0);

        if (j) {
          for (uint32_t bit = 0; bit < 8; ++bit) {
            uint32_t constant_data[4] = {uint32_t(1) << bit};
            context->UpdateSubresource(
                transfer_constants_[RenderTargetShaders::kTransferCBVRegisterStencilMask].Get(), 0,
                nullptr, constant_data, 0, 0);
            context->OMSetDepthStencilState(transfer_stencil_bit_states_[bit].Get(), UINT8_MAX);
            context->Draw(vertex_count, 0);
          }
        } else {
          context->OMSetDepthStencilState(
              dest_rt_key.is_depth ? transfer_depth_state_.Get() : nullptr, 0);
          context->Draw(vertex_count, 0);
        }
      }
    }
  }

  context->PSSetShaderResources(0, RenderTargetShaders::kTransferSRVRegisterCount, null_srvs);
  are_bound_render_targets_valid_ = false;
  command_processor_.InvalidateDrawState();
  return success;
}

ID3D11PixelShader* D3D11RenderTargetCache::GetOrCreateTransferPixelShader(
    RenderTargetShaders::TransferShaderKey key) {
  auto it = transfer_pixel_shaders_.find(key.key);
  if (it != transfer_pixel_shaders_.end()) {
    return it->second.Get();
  }
  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
  std::vector<uint32_t> code;
  ShaderBytecode bytecode;
  std::string error;
  if (!shaders_->BuildTransferShader(key, code)) {
    error = "generation failed";
  } else if (ConvertShaderBytecode(
                 std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(code.data()),
                                          code.size() * sizeof(uint32_t)),
                 PipelineCache::GetShaderBytecodeOptions(provider, false), bytecode, error)) {
    HRESULT result = provider.GetDevice()->CreatePixelShader(
        bytecode.data.data(), bytecode.data.size(), nullptr, &pixel_shader);
    if (FAILED(result)) {
      error = fmt::format("creation failed with 0x{:08X}", uint32_t(result));
    }
  }
  if (!pixel_shader) {
    REXGPU_ERROR(
        "D3D11RenderTargetCache: Failed to create the render target ownership transfer pixel "
        "shader {:08X}: {}",
        key.key, error);
  }
  return transfer_pixel_shaders_.emplace(key.key, std::move(pixel_shader)).first->second.Get();
}

uint32_t D3D11RenderTargetCache::UploadTransferRectangles(const Transfer::Rectangle* rectangles,
                                                          uint32_t count, RenderTargetKey dest_key) {
  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  ID3D11DeviceContext* context = provider.GetImmediateContext();
  uint32_t vertex_count = 6 * count;
  uint32_t size = sizeof(float) * 2 * vertex_count;
  if (transfer_vertex_buffer_size_ < size) {
    transfer_vertex_buffer_.Reset();
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = std::max(uint32_t(64) << 10, rex::next_pow2(size));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(provider.GetDevice()->CreateBuffer(&desc, nullptr, &transfer_vertex_buffer_))) {
      REXGPU_ERROR("D3D11RenderTargetCache: Failed to create a {} byte transfer vertex buffer",
                   desc.ByteWidth);
      transfer_vertex_buffer_size_ = 0;
      return 0;
    }
    transfer_vertex_buffer_size_ = desc.ByteWidth;
    transfer_vertex_buffer_position_ = transfer_vertex_buffer_size_;
  }
  uint32_t offset = transfer_vertex_buffer_position_;
  D3D11_MAP map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (transfer_vertex_buffer_size_ - offset < size) {
    offset = 0;
    map_type = D3D11_MAP_WRITE_DISCARD;
  }
  D3D11_MAPPED_SUBRESOURCE mapping;
  if (FAILED(context->Map(transfer_vertex_buffer_.Get(), 0, map_type, 0, &mapping))) {
    return 0;
  }
  float* write_ptr =
      reinterpret_cast<float*>(static_cast<uint8_t*>(mapping.pData) + offset);
  // GetRectangles returns guest pixels, scaled to the destination.
  float pixels_to_ndc_unscaled = 2.0f / float(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
  float pixels_to_ndc_x = pixels_to_ndc_unscaled * GetKeyScaleX(dest_key);
  float pixels_to_ndc_y = pixels_to_ndc_unscaled * GetKeyScaleY(dest_key);
  for (uint32_t i = 0; i < count; ++i) {
    const Transfer::Rectangle& rectangle = rectangles[i];
    float x0 = -1.0f + rectangle.x_pixels * pixels_to_ndc_x;
    float y0 = 1.0f - rectangle.y_pixels * pixels_to_ndc_y;
    float x1 = x0 + rectangle.width_pixels * pixels_to_ndc_x;
    float y1 = y0 - rectangle.height_pixels * pixels_to_ndc_y;
    const float vertices[] = {x0, y0, x1, y0, x0, y1, x0, y1, x1, y0, x1, y1};
    std::memcpy(write_ptr, vertices, sizeof(vertices));
    write_ptr += rex::countof(vertices);
  }
  context->Unmap(transfer_vertex_buffer_.Get(), 0);
  transfer_vertex_buffer_position_ = offset + size;
  ID3D11Buffer* vertex_buffer = transfer_vertex_buffer_.Get();
  UINT stride = sizeof(float) * 2;
  context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
  return vertex_count;
}

ID3D11ShaderResourceView* D3D11RenderTargetCache::CopyHostDepth(
    const D3D11RenderTarget& render_target) {
  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  D3D11_TEXTURE2D_DESC desc;
  render_target.texture()->GetDesc(&desc);
  D3D11_TEXTURE2D_DESC copy_desc = {};
  if (host_depth_copy_) {
    host_depth_copy_->GetDesc(&copy_desc);
  }
  if (!host_depth_copy_ || std::memcmp(&copy_desc, &desc, sizeof(desc))) {
    host_depth_copy_srv_.Reset();
    host_depth_copy_.Reset();
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = RenderTargetShaders::GetDepthSRVDepthDXGIFormat(
        render_target.key().GetDepthFormat());
    if (desc.SampleDesc.Count > 1) {
      srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      srv_desc.Texture2D.MipLevels = 1;
    }
    if (FAILED(provider.GetDevice()->CreateTexture2D(&desc, nullptr, &host_depth_copy_)) ||
        FAILED(provider.GetDevice()->CreateShaderResourceView(host_depth_copy_.Get(), &srv_desc,
                                                              &host_depth_copy_srv_))) {
      REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the host depth copy");
      host_depth_copy_srv_.Reset();
      host_depth_copy_.Reset();
      return nullptr;
    }
  }
  provider.GetImmediateContext()->CopyResource(host_depth_copy_.Get(), render_target.texture());
  return host_depth_copy_srv_.Get();
}

// Follows D3D12RenderTargetCache::DumpRenderTargets, without sorting.
void D3D11RenderTargetCache::CopyResolveIntoTexture(const draw_util::ResolveInfo& resolve_info,
                                                    draw_util::ResolveCopyShaderIndex copy_shader,
                                                    D3D11TextureCache& texture_cache) {
  // Why a resolve isn't copied, logged once for each reason, so a game whose
  // surfaces never take this path says why. The message is only put together
  // that first time (this runs for every resolve).
  static uint32_t reasons_logged = 0;
  auto first_time = [](uint32_t reason) {
    if (reasons_logged & (UINT32_C(1) << reason)) {
      return false;
    }
    reasons_logged |= UINT32_C(1) << reason;
    return true;
  };
  auto log_skip = [](const std::string& why) {
    REXGPU_INFO("d3d11_resolve_into_textures: a resolve not copied into its texture: {}", why);
  };
  const draw_util::ResolveEdramInfo edram_info = resolve_info.color_edram_info;
  const reg::RB_COPY_DEST_INFO dest_info = resolve_info.copy_dest_info;
  const draw_util::ResolveCopyDestCoordinateInfo dest_coordinates =
      resolve_info.copy_dest_coordinate_info;
  if (resolve_info.IsCopyingDepth()) {
    return;
  }
  // The texels are copied as they are: one sample (with one sample a "both
  // samples" or "all samples" select takes that one too, through the full copy
  // shader, which then repacks 8-bit values exactly), no exponent bias, a
  // destination format bitwise equal to the render target's.
  if (edram_info.msaa_samples != xenos::MsaaSamples::k1X || edram_info.format_is_64bpp ||
      edram_info.decode_pwl_gamma ||
      (copy_shader != draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA &&
       copy_shader != draw_util::ResolveCopyShaderIndex::kFull32bpp)) {
    if (first_time(0)) {
      log_skip(fmt::format("MSAA {}x, 64bpp {}, gamma {}, copy shader {}",
                           UINT32_C(1) << uint32_t(edram_info.msaa_samples),
                           uint32_t(edram_info.format_is_64bpp),
                           uint32_t(edram_info.decode_pwl_gamma), uint32_t(copy_shader)));
    }
    return;
  }
  if (xenos::ColorRenderTargetFormat(edram_info.format) !=
          xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
      dest_info.copy_dest_format != xenos::ColorFormat::k_8_8_8_8 || dest_info.copy_dest_exp_bias ||
      dest_info.copy_dest_array ||
      uint32_t(dest_info.copy_dest_endian) > uint32_t(xenos::Endian128::k16in32) ||
      dest_coordinates.offset_x_div_8 || dest_coordinates.offset_y_div_8) {
    if (first_time(1)) {
      log_skip(
          fmt::format("render target format {}, destination format {}, red/blue swap {}, "
                      "exponent bias {}, array {}, endian {}, destination offset {},{}",
                      uint32_t(edram_info.format), uint32_t(dest_info.copy_dest_format),
                      uint32_t(dest_info.copy_dest_swap), int32_t(dest_info.copy_dest_exp_bias),
                      uint32_t(dest_info.copy_dest_array), uint32_t(dest_info.copy_dest_endian),
                      uint32_t(dest_coordinates.offset_x_div_8) * 8,
                      uint32_t(dest_coordinates.offset_y_div_8) * 8));
    }
    return;
  }

  // One render target owns the whole area (the dump just made of it).
  uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
  if (dump_rectangles_.size() != 1) {
    if (first_time(2)) {
      log_skip(fmt::format("{} render targets own the area", dump_rectangles_.size()));
    }
    return;
  }
  const ResolveCopyDumpRectangle& rectangle = dump_rectangles_.front();
  if (rectangle.row_first || rectangle.rows != dump_rows || rectangle.row_first_start ||
      rectangle.row_last_end != dump_row_length_used || !rectangle.render_target) {
    if (first_time(3)) {
      log_skip(
          fmt::format("the render target owns part of the area (rows {}+{} of {}, tiles {}..{} "
                      "of {})",
                      rectangle.row_first, rectangle.rows, dump_rows, rectangle.row_first_start,
                      rectangle.row_last_end, dump_row_length_used));
    }
    return;
  }
  auto& d3d11_rt = *static_cast<D3D11RenderTarget*>(rectangle.render_target);
  const RenderTargetKey rt_key = d3d11_rt.key();
  if (rt_key.is_depth || rt_key.msaa_samples != xenos::MsaaSamples::k1X ||
      rt_key.GetColorFormat() != xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
      rt_key.GetPitchTiles() != edram_info.pitch_tiles || !rt_key.GetPitchTiles()) {
    if (first_time(4)) {
      log_skip(fmt::format("the owner is {} (the resolve's pitch {} tiles)", rt_key.GetDebugName(),
                           uint32_t(edram_info.pitch_tiles)));
    }
    return;
  }

  // Where the area is in the render target's texture: its tiles, rows of the
  // pitch from the render target's base, 80x16 pixels each.
  const uint32_t pitch_tiles = rt_key.GetPitchTiles();
  const uint32_t tile_offset =
      (uint32_t(edram_info.base_tiles) - rt_key.base_tiles) & (edram_tile_count() - 1);
  const uint32_t source_x =
      (tile_offset % pitch_tiles) * xenos::kEdramTileWidthSamples +
      (resolve_info.coordinate_info.edram_offset_x_div_8 << xenos::kResolveAlignmentPixelsLog2);
  const uint32_t source_y =
      (tile_offset / pitch_tiles) * xenos::kEdramTileHeightSamples +
      (resolve_info.coordinate_info.edram_offset_y_div_8 << xenos::kResolveAlignmentPixelsLog2);
  const uint32_t width = resolve_info.coordinate_info.width_div_8
                         << xenos::kResolveAlignmentPixelsLog2;
  const uint32_t height = resolve_info.height_div_8 << xenos::kResolveAlignmentPixelsLog2;
  const uint32_t rt_height = d3d11_rt.height();
  if (source_x + width > pitch_tiles * xenos::kEdramTileWidthSamples ||
      source_y + height > rt_height) {
    if (first_time(5)) {
      log_skip(fmt::format("{}x{} at {},{} is outside the render target ({}x{})", width, height,
                           source_x, source_y, pitch_tiles * xenos::kEdramTileWidthSamples,
                           rt_height));
    }
    return;
  }

  texture_cache.CopyResolveIntoTexture(
      resolve_info.copy_dest_base,
      dest_coordinates.pitch_aligned_div_32 << xenos::kTextureTileWidthHeightLog2, width, height,
      xenos::TextureFormat::k_8_8_8_8, xenos::Endian(dest_info.copy_dest_endian),
      dest_info.copy_dest_swap != 0, d3d11_rt.texture(), source_x, source_y);
}

bool D3D11RenderTargetCache::DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                                               uint32_t dump_rows, uint32_t dump_pitch,
                                               bool native_layout) {
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_);
  if (dump_rectangles_.empty()) {
    return true;
  }
  ID3D11DeviceContext* context = command_processor_.GetD3D11Provider().GetImmediateContext();
  // The render targets are read through SRVs.
  context->OMSetRenderTargets(0, nullptr, nullptr);
  are_bound_render_targets_valid_ = false;
  ID3D11Buffer* dump_constants[RenderTargetShaders::kDumpCbufferCount];
  for (uint32_t i = 0; i < RenderTargetShaders::kDumpCbufferCount; ++i) {
    dump_constants[i] = dump_constants_[i].Get();
  }
  context->CSSetConstantBuffers(0, RenderTargetShaders::kDumpCbufferCount, dump_constants);
  ID3D11UnorderedAccessView* edram_uav = edram_buffer_raw_uav_.Get();
  context->CSSetUnorderedAccessViews(0, 1, &edram_uav, nullptr);

  bool all_shaders_available = true;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& d3d11_rt = *static_cast<D3D11RenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = d3d11_rt.key();
    // The native layout is only for resolves with all sources native.
    assert_true(!native_layout || rt_key.scale_native);
    RenderTargetShaders::DumpPipelineKey shader_key;
    shader_key.msaa_samples = rt_key.msaa_samples;
    shader_key.resource_format = rt_key.resource_format;
    shader_key.is_depth = rt_key.is_depth;
    shader_key.source_scale_native = rt_key.scale_native;
    shader_key.native_layout = uint32_t(native_layout);
    ID3D11ComputeShader* shader = GetOrCreateDumpShader(shader_key);
    if (!shader) {
      all_shaders_available = false;
      continue;
    }
    context->CSSetShader(shader, nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {d3d11_rt.srv(), d3d11_rt.srv_stencil()};
    context->CSSetShaderResources(0, rt_key.is_depth ? 2 : 1, srvs);

    RenderTargetShaders::DumpPitches pitches;
    pitches.dest_pitch = dump_pitch;
    pitches.source_pitch = rt_key.GetPitchTiles();
    uint32_t pitches_data[4] = {pitches.pitches};
    context->UpdateSubresource(dump_constants_[RenderTargetShaders::kDumpCbufferPitches].Get(), 0,
                               nullptr, pitches_data, 0, 0);

    RenderTargetShaders::DumpOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    bool format_is_64bpp = rt_key.Is64bpp();
    ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count = rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile =
          rt_key.GetNonWrappedTileIndex(dump_base + dispatch.offset, edram_tile_count());
      uint32_t offsets_data[4] = {offsets.offsets};
      context->UpdateSubresource(dump_constants_[RenderTargetShaders::kDumpCbufferOffsets].Get(),
                                 0, nullptr, offsets_data, 0, 0);
      // 40 x 16 x scale samples per group (a 32bpp tile in two groups at 1x1,
      // 64bpp in one). The native layout has a 1x1 footprint.
      context->Dispatch(
          (dispatch.width_tiles * (native_layout ? 1 : draw_resolution_scale_x()))
              << uint32_t(!format_is_64bpp),
          dispatch.height_tiles * (native_layout ? 1 : draw_resolution_scale_y()), 1);
    }
  }

  ID3D11ShaderResourceView* null_srvs[2] = {};
  context->CSSetShaderResources(0, 2, null_srvs);
  ID3D11UnorderedAccessView* null_uav = nullptr;
  context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
  return all_shaders_available;
}

ID3D11ComputeShader* D3D11RenderTargetCache::GetOrCreateDumpShader(
    RenderTargetShaders::DumpPipelineKey key) {
  auto it = dump_shaders_.find(key.key);
  if (it != dump_shaders_.end()) {
    return it->second.Get();
  }
  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
  std::vector<uint32_t> code;
  ShaderBytecode bytecode;
  std::string error;
  if (!shaders_->BuildDumpShader(key, code)) {
    error = "generation failed";
  } else if (ConvertShaderBytecode(
                 std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(code.data()),
                                          code.size() * sizeof(uint32_t)),
                 PipelineCache::GetShaderBytecodeOptions(provider, false), bytecode, error)) {
    HRESULT result = provider.GetDevice()->CreateComputeShader(
        bytecode.data.data(), bytecode.data.size(), nullptr, &shader);
    if (FAILED(result)) {
      error = fmt::format("creation failed with 0x{:08X}", uint32_t(result));
    }
  }
  if (!shader) {
    REXGPU_ERROR("D3D11RenderTargetCache: Failed to create the EDRAM dump shader {:08X}: {}",
                 key.key, error);
  }
  return dump_shaders_.emplace(key.key, std::move(shader)).first->second.Get();
}

void D3D11RenderTargetCache::BindRenderTargets(
    RenderTarget* const* depth_and_color_render_targets) {
  if (are_bound_render_targets_valid_ &&
      !std::memcmp(bound_render_targets_, depth_and_color_render_targets,
                   sizeof(bound_render_targets_))) {
    return;
  }
  std::memcpy(bound_render_targets_, depth_and_color_render_targets,
              sizeof(bound_render_targets_));
  ID3D11RenderTargetView* rtvs[xenos::kMaxColorRenderTargets] = {};
  uint32_t rtv_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    const auto* render_target =
        static_cast<const D3D11RenderTarget*>(depth_and_color_render_targets[1 + i]);
    if (render_target) {
      // Gaps stay null, which Direct3D 11 allows.
      rtvs[i] = render_target->rtv();
      rtv_count = i + 1;
    }
  }
  const auto* depth_render_target =
      static_cast<const D3D11RenderTarget*>(depth_and_color_render_targets[0]);
  command_processor_.GetD3D11Provider().GetImmediateContext()->OMSetRenderTargets(
      rtv_count, rtvs, depth_render_target ? depth_render_target->dsv() : nullptr);
  are_bound_render_targets_valid_ = true;
}

void D3D11RenderTargetCache::ClearRenderTarget(D3D11RenderTarget& render_target,
                                               uint64_t clear_value, const D3D11_RECT& rect) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return;
  }
  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  ID3D11DeviceContext* context = provider.GetImmediateContext();
  RenderTargetKey key = render_target.key();
  bool whole = rect.left <= 0 && rect.top <= 0 &&
               uint32_t(rect.right) >= key.GetWidth() * GetKeyScaleX(key) &&
               uint32_t(rect.bottom) >= render_target.height() * GetKeyScaleY(key);
  if (key.is_depth) {
    if (whole) {
      context->ClearDepthStencilView(render_target.dsv(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                     RenderTargetShaders::GetHostDepthClearValue(key, clear_value),
                                     UINT8(clear_value & 0xFF));
    } else {
      ClearByDrawing(render_target, clear_value, rect);
    }
    return;
  }
  float color_clear_value[4];
  if (RenderTargetShaders::GetHostColorClearValue(key, clear_value, color_clear_value)) {
    ClearByDrawing(render_target, clear_value, rect);
    return;
  }
  ID3D11DeviceContext1* context_1 = provider.GetImmediateContext1();
  if (whole || !context_1) {
    context->ClearRenderTargetView(render_target.rtv_load(), color_clear_value);
  } else {
    context_1->ClearView(render_target.rtv_load(), color_clear_value, &rect, 1);
  }
}

void D3D11RenderTargetCache::ClearByDrawing(D3D11RenderTarget& render_target, uint64_t clear_value,
                                            const D3D11_RECT& rect) {
  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  ID3D11DeviceContext* context = provider.GetImmediateContext();
  RenderTargetKey key = render_target.key();

  D3D11_VIEWPORT viewport;
  viewport.TopLeftX = float(rect.left);
  viewport.TopLeftY = float(rect.top);
  viewport.Width = float(rect.right - rect.left);
  viewport.Height = float(rect.bottom - rect.top);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  if (key.is_depth) {
    // The fullscreen triangle is at Z 0, so the viewport depth range collapsed
    // to the clear value gives the depth.
    viewport.MinDepth = RenderTargetShaders::GetHostDepthClearValue(key, clear_value);
    viewport.MaxDepth = viewport.MinDepth;
    context->OMSetRenderTargets(0, nullptr, render_target.dsv());
    context->OMSetDepthStencilState(clear_depth_stencil_state_.Get(), UINT(clear_value & 0xFF));
    context->PSSetShader(nullptr, nullptr, 0);
  } else {
    ID3D11RenderTargetView* rtv = render_target.rtv_load();
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->OMSetDepthStencilState(clear_color_depth_stencil_state_.Get(), 0);
    uint32_t constants[4] = {uint32_t(clear_value), uint32_t(clear_value >> 32)};
    context->UpdateSubresource(clear_uint2_constants_.Get(), 0, nullptr, constants, 0, 0);
    ID3D11Buffer* constant_buffer = clear_uint2_constants_.Get();
    context->PSSetConstantBuffers(0, 1, &constant_buffer);
    context->PSSetShader(clear_uint2_ps_.Get(), nullptr, 0);
  }
  context->OMSetBlendState(clear_blend_state_.Get(), nullptr, UINT_MAX);
  context->RSSetState(clear_rasterizer_state_.Get());
  context->RSSetViewports(1, &viewport);
  context->RSSetScissorRects(1, &rect);
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(clear_vs_.Get(), nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);
  context->GSSetShader(nullptr, nullptr, 0);
  context->Draw(3, 0);

  are_bound_render_targets_valid_ = false;
  command_processor_.InvalidateDrawState();
}

}  // namespace rex::graphics::d3d11
