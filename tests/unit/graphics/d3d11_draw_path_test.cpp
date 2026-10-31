/**
 * @file        graphics/d3d11_draw_path_test.cpp
 * @brief       WARP checks of techniques used by the Direct3D 11 draw path
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32

#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <rex/graphics/d3d11/render_target_shaders.h>
#include <rex/graphics/d3d11/shader_bytecode.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/xenos_zpd_report.h>

#include <d3d11.h>
#include <wrl/client.h>

namespace {

namespace shaders {
#include "graphics/shaders/bytecode/d3d11_5_0/fullscreen_cw_vs.h"
#include "graphics/shaders/bytecode/d3d11_5_0/passthrough_position_xy_vs.h"
}  // namespace shaders

using Microsoft::WRL::ComPtr;

ComPtr<ID3D11Device> CreateWarpDevice(ComPtr<ID3D11DeviceContext>& context) {
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                               D3D11_SDK_VERSION, &device, nullptr, &context))) {
    return nullptr;
  }
  return device;
}

}  // namespace

// D3D11RenderTargetCache clears a rectangle of a depth buffer, which
// ClearDepthStencilView can't, by drawing the fullscreen triangle (at Z 0)
// with the viewport depth range collapsed to the clear value, scissored to the
// rectangle, without a pixel shader.
TEST_CASE("Depth rectangles clear by drawing with a collapsed depth range (WARP)",
          "[graphics][d3d11]") {
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device = CreateWarpDevice(context);
  if (!device) {
    SKIP("WARP is not available");
  }
  constexpr uint32_t kWidth = 64, kHeight = 32;
  constexpr float kDepth = 0.75f;
  constexpr uint8_t kStencil = 0x5A;
  const D3D11_RECT rect = {8, 4, 40, 20};

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
  ComPtr<ID3D11DepthStencilView> dsv;
  REQUIRE(SUCCEEDED(device->CreateDepthStencilView(texture.Get(), nullptr, &dsv)));
  context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);

  ComPtr<ID3D11VertexShader> vertex_shader;
  REQUIRE(SUCCEEDED(device->CreateVertexShader(shaders::fullscreen_cw_vs,
                                               sizeof(shaders::fullscreen_cw_vs), nullptr,
                                               &vertex_shader)));
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
  ComPtr<ID3D11DepthStencilState> depth_stencil_state;
  REQUIRE(SUCCEEDED(device->CreateDepthStencilState(&depth_stencil_desc, &depth_stencil_state)));
  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  rasterizer_desc.ScissorEnable = TRUE;
  ComPtr<ID3D11RasterizerState> rasterizer_state;
  REQUIRE(SUCCEEDED(device->CreateRasterizerState(&rasterizer_desc, &rasterizer_state)));

  D3D11_VIEWPORT viewport = {float(rect.left),
                             float(rect.top),
                             float(rect.right - rect.left),
                             float(rect.bottom - rect.top),
                             kDepth,
                             kDepth};
  context->OMSetRenderTargets(0, nullptr, dsv.Get());
  context->OMSetDepthStencilState(depth_stencil_state.Get(), kStencil);
  context->RSSetState(rasterizer_state.Get());
  context->RSSetViewports(1, &viewport);
  context->RSSetScissorRects(1, &rect);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vertex_shader.Get(), nullptr, 0);
  context->PSSetShader(nullptr, nullptr, 0);
  context->Draw(3, 0);

  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
  context->CopyResource(staging.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapping;
  REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
  const uint32_t expected_depth = uint32_t(std::lround(double(kDepth) * 0xFFFFFF));
  uint32_t mismatches = 0;
  for (uint32_t y = 0; y < kHeight; ++y) {
    const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapping.pData) +
                                                        y * mapping.RowPitch);
    for (uint32_t x = 0; x < kWidth; ++x) {
      bool inside = LONG(x) >= rect.left && LONG(x) < rect.right && LONG(y) >= rect.top &&
                    LONG(y) < rect.bottom;
      uint32_t depth = row[x] & 0xFFFFFF;
      uint32_t stencil = row[x] >> 24;
      bool ok = inside ? (depth == expected_depth && stencil == kStencil)
                       : (depth == 0 && stencil == 0);
      mismatches += ok ? 0 : 1;
    }
  }
  context->Unmap(staging.Get(), 0);
  CHECK(mismatches == 0);
}

// The pipeline cache's conversion options (shared memory UAV reads aliased to
// the SRV, graphics UAVs after the four render targets, the UAV slot count of
// feature level 11.0) with the translator's depth-only pixel shader.
TEST_CASE("The translator's depth-only pixel shader converts for feature level 11.0 (WARP)",
          "[graphics][d3d11]") {
  using rex::graphics::DxbcShaderTranslator;
  DxbcShaderTranslator translator(rex::ui::GraphicsProvider::GpuVendorID::kNvidia, false, false);
  std::vector<uint8_t> source = translator.CreateDepthOnlyPixelShader();
  REQUIRE(!source.empty());
  rex::graphics::d3d11::ShaderBytecodeOptions options =
      rex::graphics::d3d11::GetBackendShaderBytecodeOptions(0, 8, true);
  rex::graphics::d3d11::ShaderBytecode output;
  std::string error;
  REQUIRE(rex::graphics::d3d11::ConvertShaderBytecode(source, options, output, error));

  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device = CreateWarpDevice(context);
  if (!device) {
    SKIP("WARP is not available");
  }
  ComPtr<ID3D11PixelShader> pixel_shader;
  CHECK(SUCCEEDED(
      device->CreatePixelShader(output.data.data(), output.data.size(), nullptr, &pixel_shader)));
}

// Hybrid occlusion queries (occlusion_query_full_counters): the translator's
// counting depth-only pixel shader, converted with the pipeline cache's
// options, adds each pixel's coverage before the depth test to the Total of
// the slot zpd_counter_index selects, through u2 of the translator, bound
// after the four render targets and the shared memory (output merger slot
// 6). Every pixel fails the depth test, so the native query counts none.
TEST_CASE("Hybrid occlusion queries count the coverage before the depth test (WARP)",
          "[graphics][d3d11]") {
  using rex::graphics::DxbcShaderTranslator;
  using rex::graphics::XenosZPDReport;
  DxbcShaderTranslator translator(rex::ui::GraphicsProvider::GpuVendorID::kNvidia, false, false);
  std::vector<uint8_t> source = translator.CreateDepthOnlyPixelShader(true);
  REQUIRE(!source.empty());
  rex::graphics::d3d11::ShaderBytecode output;
  std::string error;
  REQUIRE(rex::graphics::d3d11::ConvertShaderBytecode(
      source, rex::graphics::d3d11::GetBackendShaderBytecodeOptions(0, 8, true), output, error));

  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device = CreateWarpDevice(context);
  if (!device) {
    SKIP("WARP is not available");
  }
  ComPtr<ID3D11PixelShader> pixel_shader;
  REQUIRE(SUCCEEDED(
      device->CreatePixelShader(output.data.data(), output.data.size(), nullptr, &pixel_shader)));
  ComPtr<ID3D11VertexShader> vertex_shader;
  REQUIRE(SUCCEEDED(device->CreateVertexShader(shaders::fullscreen_cw_vs,
                                               sizeof(shaders::fullscreen_cw_vs), nullptr,
                                               &vertex_shader)));

  constexpr uint32_t kWidth = 16, kHeight = 8, kCounterSlot = 3;
  D3D11_TEXTURE2D_DESC depth_desc = {};
  depth_desc.Width = kWidth;
  depth_desc.Height = kHeight;
  depth_desc.MipLevels = 1;
  depth_desc.ArraySize = 1;
  depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  depth_desc.SampleDesc.Count = 1;
  depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ComPtr<ID3D11Texture2D> depth;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&depth_desc, nullptr, &depth)));
  ComPtr<ID3D11DepthStencilView> dsv;
  REQUIRE(SUCCEEDED(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv)));
  // The triangle is at Z 0.
  context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
  D3D11_DEPTH_STENCIL_DESC depth_stencil_desc = {};
  depth_stencil_desc.DepthEnable = TRUE;
  depth_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  depth_stencil_desc.DepthFunc = D3D11_COMPARISON_GREATER;
  ComPtr<ID3D11DepthStencilState> depth_stencil_state;
  REQUIRE(SUCCEEDED(device->CreateDepthStencilState(&depth_stencil_desc, &depth_stencil_state)));

  // The counter buffer and the system constants, as the command processor
  // creates them.
  D3D11_BUFFER_DESC counter_desc = {};
  counter_desc.ByteWidth = 8 * XenosZPDReport::kCounterSizeBytes;
  counter_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  counter_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  std::vector<uint32_t> counter_initial(counter_desc.ByteWidth / sizeof(uint32_t), 0);
  D3D11_SUBRESOURCE_DATA counter_data = {counter_initial.data(), 0, 0};
  ComPtr<ID3D11Buffer> counter;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&counter_desc, &counter_data, &counter)));
  D3D11_UNORDERED_ACCESS_VIEW_DESC counter_uav_desc = {};
  counter_uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  counter_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  counter_uav_desc.Buffer.NumElements = counter_desc.ByteWidth / sizeof(uint32_t);
  counter_uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ComPtr<ID3D11UnorderedAccessView> counter_uav;
  REQUIRE(SUCCEEDED(
      device->CreateUnorderedAccessView(counter.Get(), &counter_uav_desc, &counter_uav)));
  DxbcShaderTranslator::SystemConstants system_constants = {};
  system_constants.zpd_counter_index = kCounterSlot;
  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.ByteWidth = (sizeof(system_constants) + 15) & ~uint32_t(15);
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  std::vector<uint8_t> constants_initial(constants_desc.ByteWidth);
  std::memcpy(constants_initial.data(), &system_constants, sizeof(system_constants));
  D3D11_SUBRESOURCE_DATA constants_data = {constants_initial.data(), 0, 0};
  ComPtr<ID3D11Buffer> constants;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&constants_desc, &constants_data, &constants)));

  D3D11_QUERY_DESC query_desc = {D3D11_QUERY_OCCLUSION, 0};
  ComPtr<ID3D11Query> query;
  REQUIRE(SUCCEEDED(device->CreateQuery(&query_desc, &query)));

  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> rasterizer_state;
  REQUIRE(SUCCEEDED(device->CreateRasterizerState(&rasterizer_desc, &rasterizer_state)));
  D3D11_VIEWPORT viewport = {0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f};
  ID3D11UnorderedAccessView* uavs[] = {nullptr, nullptr, counter_uav.Get()};
  context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, dsv.Get(), 4, 3, uavs, nullptr);
  context->OMSetDepthStencilState(depth_stencil_state.Get(), 0);
  context->RSSetState(rasterizer_state.Get());
  context->RSSetViewports(1, &viewport);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vertex_shader.Get(), nullptr, 0);
  context->PSSetShader(pixel_shader.Get(), nullptr, 0);
  ID3D11Buffer* constant_buffer = constants.Get();
  context->PSSetConstantBuffers(0, 1, &constant_buffer);
  context->Begin(query.Get());
  context->Draw(3, 0);
  context->End(query.Get());

  UINT64 passed = UINT64_MAX;
  while (context->GetData(query.Get(), &passed, sizeof(passed), 0) == S_FALSE) {
  }
  D3D11_BUFFER_DESC readback_desc = {};
  readback_desc.ByteWidth = counter_desc.ByteWidth;
  readback_desc.Usage = D3D11_USAGE_STAGING;
  readback_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Buffer> readback;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&readback_desc, nullptr, &readback)));
  context->CopyResource(readback.Get(), counter.Get());
  D3D11_MAPPED_SUBRESOURCE mapping;
  REQUIRE(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
  std::vector<uint32_t> counts(counter_initial.size());
  std::memcpy(counts.data(), mapping.pData, counter_desc.ByteWidth);
  context->Unmap(readback.Get(), 0);

  CHECK(passed == 0);
  for (uint32_t slot = 0; slot < 8; ++slot) {
    INFO("slot " << slot);
    CHECK(counts[slot * XenosZPDReport::kCount + XenosZPDReport::kTotal] ==
          (slot == kCounterSlot ? kWidth * kHeight : 0));
  }
  XenosZPDReport report = XenosZPDReport::FromNativeQueryAndTotal(
      passed, counts[kCounterSlot * XenosZPDReport::kCount + XenosZPDReport::kTotal]);
  CHECK(report.z_pass == 0);
  CHECK(report.z_fail == kWidth * kHeight);
}

// D3D11RenderTargetCache::PerformTransfers binds a color transfer's source at
// t0 and the address constant at b1 of the converted D3D12 transfer shader, and
// draws rectangles in NDC of a 16384x16384 viewport. A transfer between two
// 32bpp render targets with the same base and pitch copies the pixels as they
// are.
TEST_CASE("A color ownership transfer copies between render targets (WARP)",
          "[graphics][d3d11]") {
  namespace xenos = rex::graphics::xenos;
  using rex::graphics::d3d11::RenderTargetShaders;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device = CreateWarpDevice(context);
  if (!device) {
    SKIP("WARP is not available");
  }
  // One 80x16-sample tile at 32bpp.
  constexpr uint32_t kWidth = 80, kHeight = 16;

  RenderTargetShaders shaders(RenderTargetShaders::Config{});
  RenderTargetShaders::TransferShaderKey key;
  key.mode = RenderTargetShaders::TransferMode::kColorToColor;
  key.dest_msaa_samples = xenos::MsaaSamples::k1X;
  key.source_msaa_samples = xenos::MsaaSamples::k1X;
  key.dest_resource_format = uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
  key.source_resource_format = uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
  std::vector<uint32_t> code;
  REQUIRE(shaders.BuildTransferShader(key, code));
  rex::graphics::d3d11::ShaderBytecode bytecode;
  std::string error;
  REQUIRE(rex::graphics::d3d11::ConvertShaderBytecode(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(code.data()),
                               code.size() * sizeof(uint32_t)),
      rex::graphics::d3d11::GetBackendShaderBytecodeOptions(0, 8, false), bytecode, error));
  ComPtr<ID3D11PixelShader> pixel_shader;
  REQUIRE(SUCCEEDED(device->CreatePixelShader(bytecode.data.data(), bytecode.data.size(), nullptr,
                                              &pixel_shader)));
  ComPtr<ID3D11VertexShader> vertex_shader;
  REQUIRE(SUCCEEDED(device->CreateVertexShader(shaders::passthrough_position_xy_vs,
                                               sizeof(shaders::passthrough_position_xy_vs),
                                               nullptr, &vertex_shader)));
  D3D11_INPUT_ELEMENT_DESC input_element = {
      "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
  ComPtr<ID3D11InputLayout> input_layout;
  REQUIRE(SUCCEEDED(device->CreateInputLayout(&input_element, 1,
                                              shaders::passthrough_position_xy_vs,
                                              sizeof(shaders::passthrough_position_xy_vs),
                                              &input_layout)));

  std::vector<uint32_t> pattern(kWidth * kHeight);
  for (uint32_t i = 0; i < pattern.size(); ++i) {
    pattern[i] = i * 0x9E3779B9u;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA source_data = {pattern.data(), kWidth * sizeof(uint32_t), 0};
  ComPtr<ID3D11Texture2D> source, dest;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, &source_data, &source)));
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &dest)));
  ComPtr<ID3D11ShaderResourceView> source_srv;
  REQUIRE(SUCCEEDED(device->CreateShaderResourceView(source.Get(), nullptr, &source_srv)));
  ComPtr<ID3D11RenderTargetView> dest_rtv;
  REQUIRE(SUCCEEDED(device->CreateRenderTargetView(dest.Get(), nullptr, &dest_rtv)));
  const float zero[4] = {};
  context->ClearRenderTargetView(dest_rtv.Get(), zero);

  RenderTargetShaders::TransferAddressConstant address_constant;
  address_constant.dest_pitch = 1;
  address_constant.source_pitch = 1;
  uint32_t constant_data[4] = {address_constant.constant};
  D3D11_BUFFER_DESC constant_desc = {};
  constant_desc.ByteWidth = sizeof(constant_data);
  constant_desc.Usage = D3D11_USAGE_DEFAULT;
  constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA constant_init = {constant_data, 0, 0};
  ComPtr<ID3D11Buffer> constant_buffer;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&constant_desc, &constant_init, &constant_buffer)));

  constexpr float kPixelsToNdc = 2.0f / float(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
  float x1 = -1.0f + kWidth * kPixelsToNdc, y1 = 1.0f - kHeight * kPixelsToNdc;
  const float vertices[] = {-1.0f, 1.0f, x1, 1.0f, -1.0f, y1, -1.0f, y1, x1, 1.0f, x1, y1};
  D3D11_BUFFER_DESC vertex_desc = {};
  vertex_desc.ByteWidth = sizeof(vertices);
  vertex_desc.Usage = D3D11_USAGE_DEFAULT;
  vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA vertex_init = {vertices, 0, 0};
  ComPtr<ID3D11Buffer> vertex_buffer;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&vertex_desc, &vertex_init, &vertex_buffer)));

  D3D11_VIEWPORT viewport = {0.0f,
                             0.0f,
                             float(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION),
                             float(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION),
                             0.0f,
                             1.0f};
  ID3D11RenderTargetView* rtv = dest_rtv.Get();
  context->OMSetRenderTargets(1, &rtv, nullptr);
  context->RSSetViewports(1, &viewport);
  context->IASetInputLayout(input_layout.Get());
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11Buffer* vb = vertex_buffer.Get();
  UINT stride = sizeof(float) * 2, offset = 0;
  context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  context->VSSetShader(vertex_shader.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srvs[RenderTargetShaders::kTransferSRVRegisterCount] = {};
  srvs[RenderTargetShaders::kTransferSRVRegisterColor] = source_srv.Get();
  context->PSSetShaderResources(0, RenderTargetShaders::kTransferSRVRegisterCount, srvs);
  ID3D11Buffer* constant_buffers[3] = {nullptr, constant_buffer.Get(), nullptr};
  context->PSSetConstantBuffers(0, 3, constant_buffers);
  context->PSSetShader(pixel_shader.Get(), nullptr, 0);
  context->Draw(6, 0);

  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
  context->CopyResource(staging.Get(), dest.Get());
  D3D11_MAPPED_SUBRESOURCE mapping;
  REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
  uint32_t mismatches = 0;
  for (uint32_t y = 0; y < kHeight; ++y) {
    const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapping.pData) +
                                                        y * mapping.RowPitch);
    for (uint32_t x = 0; x < kWidth; ++x) {
      mismatches += row[x] == pattern[y * kWidth + x] ? 0 : 1;
    }
  }
  context->Unmap(staging.Get(), 0);
  CHECK(mismatches == 0);
}

// D3D11RenderTargetCache::DumpRenderTargets binds the render target at t0, the
// EDRAM buffer as a raw UAV at u0 and the offsets and pitches at b0 and b1 of
// the converted D3D12 dump shader, two groups per 32bpp tile. A single-sampled
// 32bpp tile is stored in the EDRAM buffer row by row.
TEST_CASE("A color render target dumps to the EDRAM buffer (WARP)", "[graphics][d3d11]") {
  namespace xenos = rex::graphics::xenos;
  using rex::graphics::d3d11::RenderTargetShaders;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device = CreateWarpDevice(context);
  if (!device) {
    SKIP("WARP is not available");
  }
  constexpr uint32_t kWidth = 80, kHeight = 16;

  RenderTargetShaders shaders(RenderTargetShaders::Config{});
  RenderTargetShaders::DumpPipelineKey key;
  key.msaa_samples = xenos::MsaaSamples::k1X;
  key.resource_format = uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
  std::vector<uint32_t> code;
  REQUIRE(shaders.BuildDumpShader(key, code));
  rex::graphics::d3d11::ShaderBytecode bytecode;
  std::string error;
  REQUIRE(rex::graphics::d3d11::ConvertShaderBytecode(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(code.data()),
                               code.size() * sizeof(uint32_t)),
      rex::graphics::d3d11::GetBackendShaderBytecodeOptions(0, 8, false), bytecode, error));
  // Its u0 is the EDRAM buffer it writes, which may not be aliased to an SRV
  // (the options of translated guest shaders; Fuse, 2026-10-28).
  rex::graphics::d3d11::ShaderBytecode aliased_bytecode;
  CHECK_FALSE(rex::graphics::d3d11::ConvertShaderBytecode(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(code.data()),
                               code.size() * sizeof(uint32_t)),
      rex::graphics::d3d11::GetBackendShaderBytecodeOptions(0, 8, true), aliased_bytecode, error));
  ComPtr<ID3D11ComputeShader> shader;
  REQUIRE(SUCCEEDED(
      device->CreateComputeShader(bytecode.data.data(), bytecode.data.size(), nullptr, &shader)));

  std::vector<uint32_t> pattern(kWidth * kHeight);
  for (uint32_t i = 0; i < pattern.size(); ++i) {
    pattern[i] = i * 0x9E3779B9u;
  }
  D3D11_TEXTURE2D_DESC texture_desc = {};
  texture_desc.Width = kWidth;
  texture_desc.Height = kHeight;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_DEFAULT;
  texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA texture_data = {pattern.data(), kWidth * sizeof(uint32_t), 0};
  ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&texture_desc, &texture_data, &texture)));
  ComPtr<ID3D11ShaderResourceView> texture_srv;
  REQUIRE(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &texture_srv)));

  D3D11_BUFFER_DESC edram_desc = {};
  edram_desc.ByteWidth = kWidth * kHeight * sizeof(uint32_t);
  edram_desc.Usage = D3D11_USAGE_DEFAULT;
  edram_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  edram_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  ComPtr<ID3D11Buffer> edram;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&edram_desc, nullptr, &edram)));
  D3D11_UNORDERED_ACCESS_VIEW_DESC edram_uav_desc = {};
  edram_uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  edram_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  edram_uav_desc.Buffer.NumElements = kWidth * kHeight;
  edram_uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ComPtr<ID3D11UnorderedAccessView> edram_uav;
  REQUIRE(SUCCEEDED(device->CreateUnorderedAccessView(edram.Get(), &edram_uav_desc, &edram_uav)));

  RenderTargetShaders::DumpOffsets offsets;
  RenderTargetShaders::DumpPitches pitches;
  pitches.dest_pitch = 1;
  pitches.source_pitch = 1;
  uint32_t offsets_data[4] = {offsets.offsets}, pitches_data[4] = {pitches.pitches};
  D3D11_BUFFER_DESC constant_desc = {};
  constant_desc.ByteWidth = 16;
  constant_desc.Usage = D3D11_USAGE_DEFAULT;
  constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA offsets_init = {offsets_data, 0, 0}, pitches_init = {pitches_data, 0, 0};
  ComPtr<ID3D11Buffer> offsets_buffer, pitches_buffer;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&constant_desc, &offsets_init, &offsets_buffer)));
  REQUIRE(SUCCEEDED(device->CreateBuffer(&constant_desc, &pitches_init, &pitches_buffer)));

  ID3D11Buffer* constant_buffers[RenderTargetShaders::kDumpCbufferCount] = {};
  constant_buffers[RenderTargetShaders::kDumpCbufferOffsets] = offsets_buffer.Get();
  constant_buffers[RenderTargetShaders::kDumpCbufferPitches] = pitches_buffer.Get();
  context->CSSetConstantBuffers(0, RenderTargetShaders::kDumpCbufferCount, constant_buffers);
  ID3D11ShaderResourceView* srv = texture_srv.Get();
  context->CSSetShaderResources(0, 1, &srv);
  ID3D11UnorderedAccessView* uav = edram_uav.Get();
  context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  context->CSSetShader(shader.Get(), nullptr, 0);
  context->Dispatch(2, 1, 1);

  edram_desc.Usage = D3D11_USAGE_STAGING;
  edram_desc.BindFlags = 0;
  edram_desc.MiscFlags = 0;
  edram_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Buffer> staging;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&edram_desc, nullptr, &staging)));
  context->CopyResource(staging.Get(), edram.Get());
  D3D11_MAPPED_SUBRESOURCE mapping;
  REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
  CHECK(!std::memcmp(mapping.pData, pattern.data(), pattern.size() * sizeof(uint32_t)));
  context->Unmap(staging.Get(), 0);
}

#endif  // _WIN32
