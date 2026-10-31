/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Ported for the ReXGlue fork: Direct3D 11 version of the
 *              D3D12 immediate drawer.
 */

#include <rex/ui/d3d11/d3d11_immediate_drawer.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d11/d3d11_presenter.h>

namespace rex::ui::d3d11 {

// Generated with tools/shaders/build_shaders.py (Shader Model 5.0 set).
namespace shaders {
#include "../shaders/bytecode/d3d11_5_0/immediate_ps.h"
#include "../shaders/bytecode/d3d11_5_0/immediate_vs.h"
}  // namespace shaders

bool D3D11ImmediateDrawer::Initialize() {
  ID3D11Device* device = provider_.GetDevice();

  if (FAILED(device->CreateVertexShader(shaders::immediate_vs, sizeof(shaders::immediate_vs),
                                        nullptr, &vertex_shader_)) ||
      FAILED(device->CreatePixelShader(shaders::immediate_ps, sizeof(shaders::immediate_ps),
                                       nullptr, &pixel_shader_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the shaders");
    return false;
  }

  D3D11_INPUT_ELEMENT_DESC input_elements[3] = {};
  input_elements[0].SemanticName = "POSITION";
  input_elements[0].Format = DXGI_FORMAT_R32G32_FLOAT;
  input_elements[0].AlignedByteOffset = offsetof(ImmediateVertex, x);
  input_elements[1].SemanticName = "TEXCOORD";
  input_elements[1].Format = DXGI_FORMAT_R32G32_FLOAT;
  input_elements[1].AlignedByteOffset = offsetof(ImmediateVertex, u);
  input_elements[2].SemanticName = "COLOR";
  input_elements[2].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  input_elements[2].AlignedByteOffset = offsetof(ImmediateVertex, color);
  if (FAILED(device->CreateInputLayout(input_elements, UINT(rex::countof(input_elements)),
                                       shaders::immediate_vs, sizeof(shaders::immediate_vs),
                                       &input_layout_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the input layout");
    return false;
  }

  D3D11_BLEND_DESC blend_desc = {};
  D3D11_RENDER_TARGET_BLEND_DESC& blend_rt = blend_desc.RenderTarget[0];
  blend_rt.BlendEnable = TRUE;
  blend_rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend_rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend_rt.BlendOp = D3D11_BLEND_OP_ADD;
  blend_rt.SrcBlendAlpha = D3D11_BLEND_ONE;
  blend_rt.DestBlendAlpha = D3D11_BLEND_ONE;
  blend_rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend_rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  rasterizer_desc.ScissorEnable = TRUE;
  D3D11_DEPTH_STENCIL_DESC depth_stencil_desc = {};
  if (FAILED(device->CreateBlendState(&blend_desc, &blend_state_)) ||
      FAILED(device->CreateRasterizerState(&rasterizer_desc, &rasterizer_state_)) ||
      FAILED(device->CreateDepthStencilState(&depth_stencil_desc, &depth_stencil_state_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the fixed-function states");
    return false;
  }

  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.MaxAnisotropy = 1;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  for (size_t i = 0; i < size_t(SamplerIndex::kCount); ++i) {
    SamplerIndex index = SamplerIndex(i);
    bool linear = index == SamplerIndex::kLinearClamp || index == SamplerIndex::kLinearRepeat;
    bool repeat = index == SamplerIndex::kNearestRepeat || index == SamplerIndex::kLinearRepeat;
    sampler_desc.Filter = linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW =
        repeat ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&sampler_desc, &samplers_[i]))) {
      REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the samplers");
      return false;
    }
  }

  // The coordinate space size inverse, a float2 padded to 16 bytes.
  D3D11_BUFFER_DESC constant_buffer_desc = {};
  constant_buffer_desc.ByteWidth = 16;
  constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
  constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&constant_buffer_desc, nullptr, &constant_buffer_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the constant buffer");
    return false;
  }

  const uint32_t white = UINT32_MAX;
  std::unique_ptr<ImmediateTexture> white_texture =
      CreateTexture(1, 1, ImmediateTextureFilter::kNearest, false,
                    reinterpret_cast<const uint8_t*>(&white));
  if (!white_texture) {
    return false;
  }
  white_texture_srv_ = static_cast<D3D11ImmediateTexture*>(white_texture.get())->srv();
  return true;
}

std::unique_ptr<ImmediateTexture> D3D11ImmediateDrawer::CreateTexture(uint32_t width,
                                                                      uint32_t height,
                                                                      ImmediateTextureFilter filter,
                                                                      bool is_repeated,
                                                                      const uint8_t* data) {
  ID3D11Device* device = provider_.GetDevice();
  D3D11_TEXTURE2D_DESC texture_desc = {};
  texture_desc.Width = width;
  texture_desc.Height = height;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = D3D11ImmediateTexture::kFormat;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial_data = {};
  initial_data.pSysMem = data;
  initial_data.SysMemPitch = sizeof(uint32_t) * width;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  if (FAILED(device->CreateTexture2D(&texture_desc, &initial_data, &texture)) ||
      FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create a {}x{} texture", width, height);
    // A texture without a view draws in the vertex color, as on the other
    // backends when the creation fails.
  }
  SamplerIndex sampler_index;
  if (filter == ImmediateTextureFilter::kLinear) {
    sampler_index = is_repeated ? SamplerIndex::kLinearRepeat : SamplerIndex::kLinearClamp;
  } else {
    sampler_index = is_repeated ? SamplerIndex::kNearestRepeat : SamplerIndex::kNearestClamp;
  }
  return std::make_unique<D3D11ImmediateTexture>(width, height, srv.Get(), sampler_index);
}

void* D3D11ImmediateDrawer::MapDynamicBuffer(Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer,
                                             UINT& buffer_size, UINT size_needed,
                                             UINT bind_flags) {
  if (!size_needed) {
    return nullptr;
  }
  if (buffer_size < size_needed) {
    buffer.Reset();
    buffer_size = 0;
    D3D11_BUFFER_DESC desc = {};
    // Grow in powers of two so a growing UI doesn't recreate it every frame.
    desc.ByteWidth = std::max(UINT(1) << 16, UINT(rex::next_pow2(size_needed)));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = bind_flags;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(provider_.GetDevice()->CreateBuffer(&desc, nullptr, &buffer))) {
      return nullptr;
    }
    buffer_size = desc.ByteWidth;
  }
  D3D11_MAPPED_SUBRESOURCE mapping;
  if (FAILED(provider_.GetImmediateContext()->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                                  &mapping))) {
    return nullptr;
  }
  return mapping.pData;
}

void D3D11ImmediateDrawer::Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
                                 float coordinate_space_height) {
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
  assert_false(batch_open_);

  ID3D11DeviceContext* context = provider_.GetImmediateContext();

  D3D11_MAPPED_SUBRESOURCE mapping;
  if (SUCCEEDED(context->Map(constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapping))) {
    float* coordinate_space_size_inv = reinterpret_cast<float*>(mapping.pData);
    coordinate_space_size_inv[0] = 1.0f / coordinate_space_width;
    coordinate_space_size_inv[1] = 1.0f / coordinate_space_height;
    context->Unmap(constant_buffer_.Get(), 0);
  }

  D3D11_VIEWPORT viewport = {};
  viewport.Width = float(ui_draw_context.render_target_width());
  viewport.Height = float(ui_draw_context.render_target_height());
  viewport.MaxDepth = 1.0f;
  context->RSSetViewports(1, &viewport);
  context->RSSetState(rasterizer_state_.Get());
  context->OMSetBlendState(blend_state_.Get(), nullptr, UINT_MAX);
  context->OMSetDepthStencilState(depth_stencil_state_.Get(), 0);
  context->IASetInputLayout(input_layout_.Get());
  context->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context->VSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());
  context->GSSetShader(nullptr, nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);
  context->PSSetShader(pixel_shader_.Get(), nullptr, 0);

  current_scissor_ = {};
  current_primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  current_srv_ = nullptr;
  current_sampler_ = nullptr;
}

void D3D11ImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  assert_false(batch_open_);
  ID3D11DeviceContext* context = provider_.GetImmediateContext();

  UINT vertices_size = UINT(sizeof(ImmediateVertex)) * UINT(batch.vertex_count);
  void* vertices = MapDynamicBuffer(vertex_buffer_, vertex_buffer_size_, vertices_size,
                                    D3D11_BIND_VERTEX_BUFFER);
  if (!vertices) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to get a buffer for {} vertices",
                 batch.vertex_count);
    return;
  }
  std::memcpy(vertices, batch.vertices, vertices_size);
  context->Unmap(vertex_buffer_.Get(), 0);
  UINT stride = UINT(sizeof(ImmediateVertex)), offset = 0;
  context->IASetVertexBuffers(0, 1, vertex_buffer_.GetAddressOf(), &stride, &offset);

  batch_has_index_buffer_ = batch.indices != nullptr;
  if (batch_has_index_buffer_) {
    UINT indices_size = UINT(sizeof(uint16_t)) * UINT(batch.index_count);
    void* indices = MapDynamicBuffer(index_buffer_, index_buffer_size_, indices_size,
                                     D3D11_BIND_INDEX_BUFFER);
    if (!indices) {
      REXLOG_ERROR("D3D11ImmediateDrawer: Failed to get a buffer for {} indices",
                   batch.index_count);
      return;
    }
    std::memcpy(indices, batch.indices, indices_size);
    context->Unmap(index_buffer_.Get(), 0);
    context->IASetIndexBuffer(index_buffer_.Get(), DXGI_FORMAT_R16_UINT, 0);
  }

  batch_open_ = true;
}

void D3D11ImmediateDrawer::Draw(const ImmediateDraw& draw) {
  if (!batch_open_) {
    // Could be an error while obtaining the vertex and index buffers.
    return;
  }
  ID3D11DeviceContext* context = provider_.GetImmediateContext();

  uint32_t scissor_left, scissor_top, scissor_width, scissor_height;
  if (!ScissorToRenderTarget(draw, scissor_left, scissor_top, scissor_width, scissor_height)) {
    return;
  }
  D3D11_RECT scissor;
  scissor.left = LONG(scissor_left);
  scissor.top = LONG(scissor_top);
  scissor.right = LONG(scissor_left + scissor_width);
  scissor.bottom = LONG(scissor_top + scissor_height);
  if (std::memcmp(&current_scissor_, &scissor, sizeof(scissor))) {
    current_scissor_ = scissor;
    context->RSSetScissorRects(1, &scissor);
  }

  auto texture = static_cast<D3D11ImmediateTexture*>(draw.texture);
  ID3D11ShaderResourceView* srv = texture && texture->srv() ? texture->srv() : nullptr;
  ID3D11SamplerState* sampler =
      samplers_[size_t(srv ? texture->sampler_index() : SamplerIndex::kNearestClamp)].Get();
  if (!srv) {
    srv = white_texture_srv_.Get();
  }
  if (current_srv_ != srv) {
    current_srv_ = srv;
    context->PSSetShaderResources(0, 1, &srv);
  }
  if (current_sampler_ != sampler) {
    current_sampler_ = sampler;
    context->PSSetSamplers(0, 1, &sampler);
  }

  D3D_PRIMITIVE_TOPOLOGY primitive_topology;
  switch (draw.primitive_type) {
    case ImmediatePrimitiveType::kLines:
      primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      break;
    case ImmediatePrimitiveType::kTriangles:
      primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    default:
      assert_unhandled_case(draw.primitive_type);
      return;
  }
  if (current_primitive_topology_ != primitive_topology) {
    current_primitive_topology_ = primitive_topology;
    context->IASetPrimitiveTopology(primitive_topology);
  }

  if (batch_has_index_buffer_) {
    context->DrawIndexed(UINT(draw.count), UINT(draw.index_offset), INT(draw.base_vertex));
  } else {
    context->Draw(UINT(draw.count), UINT(draw.base_vertex));
  }
}

void D3D11ImmediateDrawer::EndDrawBatch() { batch_open_ = false; }

void D3D11ImmediateDrawer::End() {
  assert_false(batch_open_);
  ImmediateDrawer::End();
}

}  // namespace rex::ui::d3d11
