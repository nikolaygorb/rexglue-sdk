/**
 * @file        graphics/d3d11_render_target_shaders_test.cpp
 * @brief       Tests for the Direct3D 11 backend's render target ownership
 *              transfer and EDRAM dump shaders (Windows: WARP)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <d3d11.h>

#include <rex/graphics/d3d11/render_target_shaders.h>
#include <rex/graphics/d3d11/shader_bytecode.h>
#include <rex/graphics/format/dxbc.h>
#include <rex/graphics/xenos.h>

namespace {

using rex::graphics::d3d11::ConvertShaderBytecode;
using rex::graphics::d3d11::RenderTargetShaders;
using rex::graphics::d3d11::ShaderBytecode;
namespace xenos = rex::graphics::xenos;

using Key = RenderTargetShaders::TransferShaderKey;
using Mode = RenderTargetShaders::TransferMode;
using Output = RenderTargetShaders::TransferOutput;

constexpr uint32_t kColorFormats[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 12, 14, 15};
constexpr xenos::MsaaSamples kMsaa[] = {xenos::MsaaSamples::k1X, xenos::MsaaSamples::k2X,
                                        xenos::MsaaSamples::k4X};

bool IsColorSourceMode(Mode mode) {
  return mode == Mode::kColorToDepth || mode == Mode::kColorToColor ||
         mode == Mode::kColorToStencilBit || mode == Mode::kColorAndHostDepthToDepth;
}

bool IsHostDepthMode(Mode mode) {
  return mode == Mode::kColorAndHostDepthToDepth || mode == Mode::kDepthAndHostDepthToDepth;
}

// Every mode, both MSAA sides and every relevant format, plus the host depth and
// scale-class variants.
std::vector<Key> TransferKeys() {
  std::vector<Key> keys;
  for (uint32_t mode_index = 0; mode_index < uint32_t(Mode::kCount); ++mode_index) {
    Mode mode = Mode(mode_index);
    Output output = RenderTargetShaders::kTransferModes[mode_index].output;
    std::vector<uint32_t> dest_formats;
    if (output == Output::kColor) {
      dest_formats.assign(std::begin(kColorFormats), std::end(kColorFormats));
    } else {
      dest_formats = {0, 1};
    }
    std::vector<uint32_t> source_formats;
    if (IsColorSourceMode(mode)) {
      source_formats.assign(std::begin(kColorFormats), std::end(kColorFormats));
    } else {
      source_formats = {0, 1};
    }
    for (xenos::MsaaSamples dest_msaa : kMsaa) {
      for (xenos::MsaaSamples source_msaa : kMsaa) {
        for (uint32_t dest_format : dest_formats) {
          for (uint32_t source_format : source_formats) {
            for (uint32_t variant = 0; variant < (IsHostDepthMode(mode) ? 4u : 2u); ++variant) {
              Key key;
              key.mode = mode;
              key.dest_msaa_samples = dest_msaa;
              key.source_msaa_samples = source_msaa;
              key.dest_resource_format = dest_format;
              key.source_resource_format = source_format;
              if (IsHostDepthMode(mode)) {
                key.host_depth_source_is_copy = variant & 1;
                key.host_depth_source_msaa_samples =
                    key.host_depth_source_is_copy ? xenos::MsaaSamples::k1X : source_msaa;
                key.dest_scale_native = (variant >> 1) & 1;
              } else {
                key.source_scale_native = variant & 1;
              }
              keys.push_back(key);
            }
          }
        }
      }
    }
  }
  return keys;
}

std::vector<RenderTargetShaders::DumpPipelineKey> DumpKeys() {
  std::vector<RenderTargetShaders::DumpPipelineKey> keys;
  for (xenos::MsaaSamples msaa : kMsaa) {
    for (uint32_t is_depth = 0; is_depth < 2; ++is_depth) {
      std::vector<uint32_t> formats;
      if (is_depth) {
        formats = {0, 1};
      } else {
        formats.assign(std::begin(kColorFormats), std::end(kColorFormats));
      }
      for (uint32_t format : formats) {
        for (uint32_t layout = 0; layout < 3; ++layout) {
          RenderTargetShaders::DumpPipelineKey key;
          key.msaa_samples = msaa;
          key.resource_format = format;
          key.is_depth = is_depth;
          key.source_scale_native = layout != 0;
          key.native_layout = layout == 2;
          keys.push_back(key);
        }
      }
    }
  }
  return keys;
}

std::span<const uint8_t> Bytes(const std::vector<uint32_t>& words) {
  return {reinterpret_cast<const uint8_t*>(words.data()), words.size() * sizeof(uint32_t)};
}

std::vector<RenderTargetShaders::Config> Configs() {
  RenderTargetShaders::Config native;
  RenderTargetShaders::Config scaled;
  scaled.draw_resolution_scale_x = 2;
  scaled.draw_resolution_scale_y = 2;
  scaled.edram_tile_count = xenos::kEdramTileCountMax;
  scaled.depth_float24_round = true;
  return {native, scaled};
}

}  // namespace

TEST_CASE("Render target transfer and dump shaders are accepted at feature level 11.0 (WARP)",
          "[graphics][d3d11]") {
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ID3D11Device* device = nullptr;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                               D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
    SKIP("WARP is not available");
  }
  std::vector<uint32_t> dxbc;
  size_t transfer_count = 0, dump_count = 0;
  for (const RenderTargetShaders::Config& config : Configs()) {
    RenderTargetShaders shaders(config);
    for (Key key : TransferKeys()) {
      INFO("scale " << config.draw_resolution_scale_x << ", transfer key " << key.key);
      REQUIRE(shaders.BuildTransferShader(key, dxbc));
      ShaderBytecode output;
      std::string error;
      INFO(error);
      REQUIRE(ConvertShaderBytecode(Bytes(dxbc), {}, output, error));
      ID3D11PixelShader* shader = nullptr;
      REQUIRE(SUCCEEDED(
          device->CreatePixelShader(output.data.data(), output.data.size(), nullptr, &shader)));
      shader->Release();
      ++transfer_count;
    }
    for (RenderTargetShaders::DumpPipelineKey key : DumpKeys()) {
      INFO("scale " << config.draw_resolution_scale_x << ", dump key " << key.key);
      REQUIRE(shaders.BuildDumpShader(key, dxbc));
      ShaderBytecode output;
      std::string error;
      REQUIRE(ConvertShaderBytecode(Bytes(dxbc), {}, output, error));
      ID3D11ComputeShader* shader = nullptr;
      REQUIRE(SUCCEEDED(
          device->CreateComputeShader(output.data.data(), output.data.size(), nullptr, &shader)));
      shader->Release();
      ++dump_count;
    }
  }
  CHECK(transfer_count > 0);
  CHECK(dump_count > 0);
  device->Release();
}

TEST_CASE("Host render target formats match the D3D12 backend's choice", "[graphics][d3d11]") {
  RenderTargetShaders::Config config;
  RenderTargetShaders shaders(config);
  using F = xenos::ColorRenderTargetFormat;
  CHECK(shaders.GetColorResourceDXGIFormat(F::k_8_8_8_8) == DXGI_FORMAT_R8G8B8A8_UNORM);
  CHECK(shaders.GetColorDrawDXGIFormat(F::k_16_16) == DXGI_FORMAT_R16G16_SNORM);
  bool is_integer = false;
  CHECK(shaders.GetColorOwnershipTransferDXGIFormat(F::k_32_FLOAT, &is_integer) ==
        DXGI_FORMAT_R32_UINT);
  CHECK(is_integer);
  config.gamma_render_target_as_unorm16 = true;
  RenderTargetShaders unorm16(config);
  CHECK(unorm16.GetColorResourceDXGIFormat(F::k_8_8_8_8_GAMMA) ==
        DXGI_FORMAT_R16G16B16A16_UNORM);
}
