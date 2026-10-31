/**
 * @file        graphics/d3d11_shader_bytecode_test.cpp
 * @brief       Tests for the Shader Model 5.1 to 5.0 bytecode conversion of
 *              the Direct3D 11 backend, on built-in shaders compiled by FXC
 *              both ways
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rex/graphics/d3d11/shader_bytecode.h>
#include <rex/graphics/format/dxbc.h>

#include "thirdparty/dxbc/DXBCChecksum.h"

#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>
#endif

using BYTE = uint8_t;

// The same built-in shaders compiled by FXC as SM 5.1 (D3D12) and SM 5.0 (D3D11).
namespace sm51 {
#include "../../../src/graphics/shaders/bytecode/d3d12_5_1/fullscreen_cw_vs.h"
#include "../../../src/graphics/shaders/bytecode/d3d12_5_1/tessellation_adaptive_vs.h"
#include "../../../src/graphics/shaders/bytecode/d3d12_5_1/adaptive_quad_hs.h"
#include "../../../src/graphics/shaders/bytecode/d3d12_5_1/apply_gamma_table_ps.h"
#include "../../../src/graphics/shaders/bytecode/d3d12_5_1/float24_round_ps.h"
#include "../../../src/graphics/shaders/bytecode/d3d12_5_1/apply_gamma_table_cs.h"
#include "../../../src/graphics/shaders/bytecode/d3d12_5_1/texture_load_32bpb_cs.h"
#include "../../../src/graphics/shaders/bytecode/d3d12_5_1/host_depth_store_4xmsaa_cs.h"
#include "../../../src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_32bpp_cs.h"
#include "../../../src/graphics/shaders/bytecode/d3d12_5_1/smaa_edge_detection_cs.h"
#include "../../../src/ui/shaders/bytecode/d3d12_5_1/immediate_vs.h"
#include "../../../src/ui/shaders/bytecode/d3d12_5_1/immediate_ps.h"
#include "../../../src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_easu_ps.h"
#include "../../../src/ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_ps.h"
}  // namespace sm51

namespace sm50 {
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/fullscreen_cw_vs.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/tessellation_adaptive_vs.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/adaptive_quad_hs.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/apply_gamma_table_ps.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/float24_round_ps.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/apply_gamma_table_cs.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/texture_load_32bpb_cs.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/host_depth_store_4xmsaa_cs.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/resolve_full_32bpp_cs.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/smaa_edge_detection_cs.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/smaa_blending_weight_calculation_cs.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/smaa_neighborhood_blending_cs.h"
#include "../../../src/graphics/shaders/bytecode/d3d11_5_0/fxaa_cs.h"
#include "../../../src/ui/shaders/bytecode/d3d11_5_0/immediate_vs.h"
#include "../../../src/ui/shaders/bytecode/d3d11_5_0/immediate_ps.h"
#include "../../../src/ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_fsr_easu_ps.h"
#include "../../../src/ui/shaders/bytecode/d3d11_5_0/guest_output_triangle_strip_rect_vs.h"
}  // namespace sm50

struct BuiltinShader {
  const char* name;
  std::span<const uint8_t> sm51;
  std::span<const uint8_t> sm50;
};

const BuiltinShader kBuiltinShaders[] = {
    {"fullscreen_cw_vs", sm51::fullscreen_cw_vs, sm50::fullscreen_cw_vs},
    {"tessellation_adaptive_vs", sm51::tessellation_adaptive_vs, sm50::tessellation_adaptive_vs},
    {"adaptive_quad_hs", sm51::adaptive_quad_hs, sm50::adaptive_quad_hs},
    {"apply_gamma_table_ps", sm51::apply_gamma_table_ps, sm50::apply_gamma_table_ps},
    {"float24_round_ps", sm51::float24_round_ps, sm50::float24_round_ps},
    {"apply_gamma_table_cs", sm51::apply_gamma_table_cs, sm50::apply_gamma_table_cs},
    {"texture_load_32bpb_cs", sm51::texture_load_32bpb_cs, sm50::texture_load_32bpb_cs},
    {"host_depth_store_4xmsaa_cs", sm51::host_depth_store_4xmsaa_cs,
     sm50::host_depth_store_4xmsaa_cs},
    {"resolve_full_32bpp_cs", sm51::resolve_full_32bpp_cs, sm50::resolve_full_32bpp_cs},
    {"smaa_edge_detection_cs", sm51::smaa_edge_detection_cs, sm50::smaa_edge_detection_cs},
    {"immediate_vs", sm51::immediate_vs, sm50::immediate_vs},
    {"immediate_ps", sm51::immediate_ps, sm50::immediate_ps},
    {"guest_output_ffx_fsr_easu_ps", sm51::guest_output_ffx_fsr_easu_ps,
     sm50::guest_output_ffx_fsr_easu_ps},
};

namespace {

using rex::graphics::d3d11::ConvertShaderBytecode;
using rex::graphics::d3d11::ResourceKind;
using rex::graphics::d3d11::ShaderBytecode;
using rex::graphics::d3d11::ShaderBytecodeOptions;

std::vector<uint32_t> Words(std::span<const uint8_t> bytes) {
  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), words.size() * 4);
  return words;
}

std::span<const uint32_t> Chunk(const std::vector<uint32_t>& container, const char* fourcc) {
  uint32_t want;
  std::memcpy(&want, fourcc, 4);
  for (uint32_t i = 0; i < container[7]; ++i) {
    uint32_t offset = container[8 + i] / 4;
    if (container[offset] == want) {
      return std::span<const uint32_t>(container).subspan(offset + 2, container[offset + 1] / 4);
    }
  }
  return {};
}

// (opcode, register) of every SM 5.0 resource declaration.
std::vector<std::pair<uint32_t, uint32_t>> Declarations(std::span<const uint8_t> bytes) {
  std::vector<uint32_t> container = Words(bytes);
  std::span<const uint32_t> program = Chunk(container, "SHEX");
  if (program.empty()) {
    program = Chunk(container, "SHDR");
  }
  std::vector<std::pair<uint32_t, uint32_t>> result;
  for (size_t position = 2; position < program.size();) {
    uint32_t opcode = program[position] & 0x7FF;
    uint32_t length = opcode == 35 ? program[position + 1] : (program[position] >> 24) & 127;
    REQUIRE(length);
    switch (opcode) {
      case 88:   // dcl_resource
      case 89:   // dcl_constantbuffer
      case 90:   // dcl_sampler
      case 156:  // dcl_uav_typed
      case 157:  // dcl_uav_raw
      case 158:  // dcl_uav_structured
      case 161:  // dcl_resource_raw
      case 162:  // dcl_resource_structured
        result.emplace_back(opcode, program[position + 2]);
        break;
    }
    position += length;
  }
  std::sort(result.begin(), result.end());
  return result;
}

bool ChecksumValid(std::span<const uint8_t> bytes) {
  std::vector<uint32_t> container = Words(bytes);
  unsigned int checksum[4];
  CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(container.data()), uint32_t(bytes.size()),
                        checksum);
  return std::memcmp(checksum, container.data() + 1, 16) == 0;
}

// Changes the program type of a container and fixes the checksum, to put
// declarations into a stage where they are restricted.
std::vector<uint8_t> WithProgramType(std::span<const uint8_t> bytes, uint32_t program_type) {
  std::vector<uint32_t> container = Words(bytes);
  for (uint32_t i = 0; i < container[7]; ++i) {
    uint32_t offset = container[8 + i] / 4;
    if (container[offset] == 0x58454853) {  // SHEX
      uint32_t& version = container[offset + 2];
      version = (version & 0xFFFF) | (program_type << 16);
    }
  }
  unsigned int checksum[4];
  CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(container.data()),
                        uint32_t(container.size() * 4), checksum);
  std::memcpy(container.data() + 1, checksum, 16);
  std::vector<uint8_t> result(container.size() * 4);
  std::memcpy(result.data(), container.data(), result.size());
  return result;
}

}  // namespace

TEST_CASE("Built-in shaders convert to the declarations FXC emits for SM 5.0",
          "[graphics][d3d11]") {
  ShaderBytecodeOptions options;
  // The built-in shaders bind their UAVs from u0, like FXC's SM 5.0 output.
  options.graphics_uav_register_offset = 0;
  for (const BuiltinShader& shader : kBuiltinShaders) {
    INFO(shader.name);
    ShaderBytecode output;
    std::string error;
    REQUIRE(ConvertShaderBytecode(shader.sm51, options, output, error));
    CHECK(error.empty());
    CHECK(ChecksumValid(output.data));
    std::vector<uint32_t> container = Words(output.data);
    std::span<const uint32_t> program = Chunk(container, "SHEX");
    REQUIRE(!program.empty());
    CHECK((program[0] & 0xFFFF) == 0x50);
    CHECK(Declarations(output.data) == Declarations(shader.sm50));
  }
}

TEST_CASE("SM 5.0 input is refused", "[graphics][d3d11]") {
  ShaderBytecode output;
  std::string error;
  CHECK_FALSE(ConvertShaderBytecode(kBuiltinShaders[0].sm50, {}, output, error));
  CHECK(error.find("5.1") != std::string::npos);
  CHECK(output.data.empty());
}

TEST_CASE("A corrupt container fails and clears the output", "[graphics][d3d11]") {
  std::vector<uint8_t> bytes(kBuiltinShaders[0].sm51.begin(), kBuiltinShaders[0].sm51.end());
  bytes[bytes.size() / 2] ^= 0x5A;
  ShaderBytecode output;
  output.data = {1, 2, 3};
  std::string error;
  CHECK_FALSE(ConvertShaderBytecode(bytes, {}, output, error));
  CHECK_FALSE(error.empty());
  CHECK(output.data.empty());
  CHECK(output.bindings.empty());
}

namespace {
// texture_load_32bpb_cs: raw SRV t0 (source), raw UAV u0 (written).
std::span<const uint8_t> RawLoadStoreShader() {
  for (const BuiltinShader& shader : kBuiltinShaders) {
    if (std::string(shader.name) == "texture_load_32bpb_cs") {
      return shader.sm51;
    }
  }
  return {};
}
}  // namespace

TEST_CASE("Vertex shader UAVs need feature level 11.1", "[graphics][d3d11]") {
  std::vector<uint8_t> vertex = WithProgramType(
      RawLoadStoreShader(), uint32_t(rex::graphics::dxbc::ProgramType::kVertexShader));
  ShaderBytecode output;
  std::string error;
  CHECK_FALSE(ConvertShaderBytecode(vertex, {}, output, error));
  CHECK(error.find("UAVs in this shader stage") != std::string::npos);

  ShaderBytecodeOptions options;
  options.supported_feature_flags = rex::graphics::dxbc::kShaderFeature0_UAVsAtEveryStage |
                                    rex::graphics::dxbc::kShaderFeature0_64UAVs;
  options.uav_register_count = 64;
  REQUIRE(ConvertShaderBytecode(vertex, options, output, error));
  bool found_uav = false;
  for (const auto& binding : output.bindings) {
    if (binding.kind == ResourceKind::kUnorderedAccess) {
      found_uav = true;
      // Above the four guest render targets.
      CHECK(binding.host_register == binding.source_register + 4);
    }
  }
  CHECK(found_uav);
}

TEST_CASE("A written UAV is never aliased to the read-only SRV", "[graphics][d3d11]") {
  ShaderBytecodeOptions options;
  options.read_only_uav_aliases.push_back({0, 0});
  ShaderBytecode output;
  std::string error;
  // Compute shaders keep their UAVs.
  CHECK_FALSE(ConvertShaderBytecode(RawLoadStoreShader(), options, output, error));
  CHECK(error.find("graphics-stage") != std::string::npos);
  // A graphics stage that writes the aliased UAV would lose the write.
  std::vector<uint8_t> vertex = WithProgramType(
      RawLoadStoreShader(), uint32_t(rex::graphics::dxbc::ProgramType::kVertexShader));
  CHECK_FALSE(ConvertShaderBytecode(vertex, options, output, error));
  CHECK(error.find("write") != std::string::npos);
}

#ifdef _WIN32
TEST_CASE("Converted built-in shaders are accepted at feature level 11.0 (WARP)",
          "[graphics][d3d11]") {
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ID3D11Device* device = nullptr;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                               D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
    SKIP("WARP is not available");
  }
  ShaderBytecodeOptions options;
  for (const BuiltinShader& shader : kBuiltinShaders) {
    INFO(shader.name);
    ShaderBytecode output;
    std::string error;
    REQUIRE(ConvertShaderBytecode(shader.sm51, options, output, error));
    const void* code = output.data.data();
    SIZE_T size = output.data.size();
    HRESULT result = E_FAIL;
    using rex::graphics::dxbc::ProgramType;
    switch (ProgramType(output.program_type)) {
      case ProgramType::kVertexShader: {
        ID3D11VertexShader* created = nullptr;
        result = device->CreateVertexShader(code, size, nullptr, &created);
        if (created)
          created->Release();
      } break;
      case ProgramType::kHullShader: {
        ID3D11HullShader* created = nullptr;
        result = device->CreateHullShader(code, size, nullptr, &created);
        if (created)
          created->Release();
      } break;
      case ProgramType::kPixelShader: {
        ID3D11PixelShader* created = nullptr;
        result = device->CreatePixelShader(code, size, nullptr, &created);
        if (created)
          created->Release();
      } break;
      case ProgramType::kComputeShader: {
        ID3D11ComputeShader* created = nullptr;
        result = device->CreateComputeShader(code, size, nullptr, &created);
        if (created)
          created->Release();
      } break;
      default:
        break;
    }
    CHECK(SUCCEEDED(result));
  }
  device->Release();
}

// D3D11TextureCache passes the textures' channel mappings to the conversion
// (Direct3D 11 views have none): the bilinear output shader's fetch from t0
// (sample_l to .xyz) gets a permutation, or constant zero and one.
TEST_CASE("Texture channel mappings are applied by the conversion (WARP)", "[graphics][d3d11]") {
  using Microsoft::WRL::ComPtr;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                               D3D11_SDK_VERSION, &device, nullptr, &context))) {
    SKIP("WARP is not available");
  }
  constexpr uint32_t kSize = 4;
  // R 51, G 102, B 153, A 255.
  std::vector<uint32_t> texels(kSize * kSize, 0xFF996633u);
  D3D11_TEXTURE2D_DESC texture_desc = {};
  texture_desc.Width = kSize;
  texture_desc.Height = kSize;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA texture_data = {texels.data(), kSize * sizeof(uint32_t)};
  ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&texture_desc, &texture_data, &texture)));
  ComPtr<ID3D11ShaderResourceView> srv;
  REQUIRE(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv)));
  texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> target;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&texture_desc, nullptr, &target)));
  ComPtr<ID3D11RenderTargetView> rtv;
  REQUIRE(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
  texture_desc.BindFlags = 0;
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&texture_desc, nullptr, &staging)));

  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> sampler;
  REQUIRE(SUCCEEDED(device->CreateSamplerState(&sampler_desc, &sampler)));

  // Vertex shader: rectangle offset and size in NDC; pixel shader: output
  // offset and inverse size.
  const float vs_constants[4] = {-1.0f, 1.0f, 2.0f, -2.0f};
  uint32_t ps_constants[4] = {0, 0, 0, 0};
  float size_inv = 1.0f / float(kSize);
  std::memcpy(&ps_constants[2], &size_inv, sizeof(size_inv));
  std::memcpy(&ps_constants[3], &size_inv, sizeof(size_inv));
  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.ByteWidth = 16;
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA vs_constants_data = {vs_constants};
  D3D11_SUBRESOURCE_DATA ps_constants_data = {ps_constants};
  ComPtr<ID3D11Buffer> vs_constant_buffer, ps_constant_buffer;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&constants_desc, &vs_constants_data, &vs_constant_buffer)));
  REQUIRE(SUCCEEDED(device->CreateBuffer(&constants_desc, &ps_constants_data, &ps_constant_buffer)));
  ComPtr<ID3D11VertexShader> vertex_shader;
  REQUIRE(SUCCEEDED(device->CreateVertexShader(
      sm50::guest_output_triangle_strip_rect_vs, sizeof(sm50::guest_output_triangle_strip_rect_vs),
      nullptr, &vertex_shader)));

  struct Case {
    const char* name;
    uint32_t swizzle;
    uint8_t expected[4];
  };
  const Case cases[] = {
      // B, G, R, A.
      {"permutation", 2 | (1 << 3) | (0 << 6) | (3 << 9), {153, 102, 51, 255}},
      // R, 0, 1, A.
      {"constants", 0 | (4 << 3) | (5 << 6) | (3 << 9), {51, 0, 255, 255}},
  };
  for (const Case& test_case : cases) {
    INFO(test_case.name);
    ShaderBytecodeOptions options;
    options.texture_swizzles.push_back({0, test_case.swizzle});
    ShaderBytecode output;
    std::string error;
    REQUIRE(ConvertShaderBytecode(
        std::span<const uint8_t>(sm51::guest_output_bilinear_ps,
                                 sizeof(sm51::guest_output_bilinear_ps)),
        options, output, error));
    ComPtr<ID3D11PixelShader> pixel_shader;
    REQUIRE(SUCCEEDED(device->CreatePixelShader(output.data.data(), output.data.size(), nullptr,
                                                &pixel_shader)));

    ID3D11RenderTargetView* rtv_pointer = rtv.Get();
    context->OMSetRenderTargets(1, &rtv_pointer, nullptr);
    D3D11_VIEWPORT viewport = {0.0f, 0.0f, float(kSize), float(kSize), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11Buffer* buffer = vs_constant_buffer.Get();
    context->VSSetConstantBuffers(0, 1, &buffer);
    buffer = ps_constant_buffer.Get();
    context->PSSetConstantBuffers(0, 1, &buffer);
    context->VSSetShader(vertex_shader.Get(), nullptr, 0);
    context->PSSetShader(pixel_shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv_pointer = srv.Get();
    context->PSSetShaderResources(0, 1, &srv_pointer);
    ID3D11SamplerState* sampler_pointer = sampler.Get();
    context->PSSetSamplers(0, 1, &sampler_pointer);
    context->Draw(4, 0);
    context->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapping;
    REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
    uint8_t pixel[4];
    std::memcpy(pixel, static_cast<const uint8_t*>(mapping.pData) + mapping.RowPitch + 4, 4);
    context->Unmap(staging.Get(), 0);
    for (uint32_t i = 0; i < 4; ++i) {
      CHECK(pixel[i] == test_case.expected[i]);
    }
  }
}

// The D3D11 swap: apply_gamma_table_ps converted with the front buffer's
// channel mapping (a load from t1), the 256-entry gamma ramp at t0 as an
// R10G10B10A2 buffer, drawn with fullscreen_cw_vs.
TEST_CASE("The swap gamma shader applies the front buffer's mapping (WARP)",
          "[graphics][d3d11]") {
  using Microsoft::WRL::ComPtr;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                               D3D11_SDK_VERSION, &device, nullptr, &context))) {
    SKIP("WARP is not available");
  }
  constexpr uint32_t kSize = 4;
  // R 51, G 102, B 153.
  std::vector<uint32_t> texels(kSize * kSize, 0xFF996633u);
  D3D11_TEXTURE2D_DESC texture_desc = {};
  texture_desc.Width = kSize;
  texture_desc.Height = kSize;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA texture_data = {texels.data(), kSize * sizeof(uint32_t)};
  ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&texture_desc, &texture_data, &texture)));
  ComPtr<ID3D11ShaderResourceView> srv;
  REQUIRE(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv)));
  texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> target;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&texture_desc, nullptr, &target)));
  ComPtr<ID3D11RenderTargetView> rtv;
  REQUIRE(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
  texture_desc.BindFlags = 0;
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&texture_desc, nullptr, &staging)));

  // Identity ramp, the same in all three 10-bit components.
  std::vector<uint32_t> ramp(256);
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = (i * 1023 + 127) / 255;
    ramp[i] = value | (value << 10) | (value << 20);
  }
  D3D11_BUFFER_DESC ramp_desc = {};
  ramp_desc.ByteWidth = 256 * sizeof(uint32_t);
  ramp_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA ramp_data = {ramp.data()};
  ComPtr<ID3D11Buffer> ramp_buffer;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&ramp_desc, &ramp_data, &ramp_buffer)));
  D3D11_SHADER_RESOURCE_VIEW_DESC ramp_srv_desc = {};
  ramp_srv_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  ramp_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  ramp_srv_desc.Buffer.NumElements = 256;
  ComPtr<ID3D11ShaderResourceView> ramp_srv;
  REQUIRE(SUCCEEDED(device->CreateShaderResourceView(ramp_buffer.Get(), &ramp_srv_desc, &ramp_srv)));

  ShaderBytecodeOptions options;
  // B, G, R, A.
  options.texture_swizzles.push_back({1, 2 | (1 << 3) | (0 << 6) | (3 << 9)});
  ShaderBytecode output;
  std::string error;
  REQUIRE(ConvertShaderBytecode(
      std::span<const uint8_t>(sm51::apply_gamma_table_ps, sizeof(sm51::apply_gamma_table_ps)),
      options, output, error));
  ComPtr<ID3D11PixelShader> pixel_shader;
  REQUIRE(SUCCEEDED(
      device->CreatePixelShader(output.data.data(), output.data.size(), nullptr, &pixel_shader)));
  ComPtr<ID3D11VertexShader> vertex_shader;
  REQUIRE(SUCCEEDED(device->CreateVertexShader(sm50::fullscreen_cw_vs,
                                               sizeof(sm50::fullscreen_cw_vs), nullptr,
                                               &vertex_shader)));

  ID3D11RenderTargetView* rtv_pointer = rtv.Get();
  context->OMSetRenderTargets(1, &rtv_pointer, nullptr);
  D3D11_VIEWPORT viewport = {0.0f, 0.0f, float(kSize), float(kSize), 0.0f, 1.0f};
  context->RSSetViewports(1, &viewport);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vertex_shader.Get(), nullptr, 0);
  context->PSSetShader(pixel_shader.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srvs[] = {ramp_srv.Get(), srv.Get()};
  context->PSSetShaderResources(0, 2, srvs);
  context->Draw(3, 0);
  context->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE mapping;
  REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
  uint8_t pixel[4];
  std::memcpy(pixel, static_cast<const uint8_t*>(mapping.pData) + mapping.RowPitch + 4, 4);
  context->Unmap(staging.Get(), 0);
  CHECK(pixel[0] == 153);
  CHECK(pixel[1] == 102);
  CHECK(pixel[2] == 51);
  CHECK(pixel[3] == 255);
}

// The D3D11 swap post effects: the SM 5.0 FXAA and SMAA compute shaders with
// the size at b0 and a linear clamp sampler at s0, as
// D3D11CommandProcessor::DrawSwapTexture binds them. A flat color has no edges,
// so both leave it as it is.
namespace {
struct PostEffectTexture {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
};

PostEffectTexture CreatePostEffectTexture(ID3D11Device* device, DXGI_FORMAT format,
                                          uint32_t size, const void* data, uint32_t pitch) {
  PostEffectTexture texture;
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = size;
  desc.Height = size;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  D3D11_SUBRESOURCE_DATA initial_data = {data, pitch};
  REQUIRE(SUCCEEDED(
      device->CreateTexture2D(&desc, data ? &initial_data : nullptr, &texture.texture)));
  REQUIRE(SUCCEEDED(device->CreateShaderResourceView(texture.texture.Get(), nullptr, &texture.srv)));
  REQUIRE(
      SUCCEEDED(device->CreateUnorderedAccessView(texture.texture.Get(), nullptr, &texture.uav)));
  return texture;
}

uint32_t ReadTexel7x5(ID3D11Device* device, ID3D11DeviceContext* context,
                        ID3D11Texture2D* texture) {
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  desc.BindFlags = 0;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
  context->CopyResource(staging.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapping;
  REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
  uint32_t texel;
  std::memcpy(&texel, static_cast<const uint8_t*>(mapping.pData) + mapping.RowPitch * 5 + 4 * 7,
              sizeof(texel));
  context->Unmap(staging.Get(), 0);
  return texel;
}
}  // namespace

TEST_CASE("The swap post effect shaders keep a flat color (WARP)", "[graphics][d3d11]") {
  using Microsoft::WRL::ComPtr;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                               D3D11_SDK_VERSION, &device, nullptr, &context))) {
    SKIP("WARP is not available");
  }
  constexpr uint32_t kSize = 16;
  // R10G10B10A2: R 256, G 512, B 768, A 3.
  constexpr uint32_t kColor = 256 | (512 << 10) | (768u << 20) | (3u << 30);
  std::vector<uint32_t> color_texels(kSize * kSize, kColor);
  PostEffectTexture output = CreatePostEffectTexture(device.Get(), DXGI_FORMAT_R10G10B10A2_UNORM,
                                                     kSize, nullptr, 0);

  uint32_t constants[4] = {kSize, kSize};
  float size_inv = 1.0f / float(kSize);
  std::memcpy(constants + 2, &size_inv, sizeof(size_inv));
  std::memcpy(constants + 3, &size_inv, sizeof(size_inv));
  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.ByteWidth = sizeof(constants);
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA constants_data = {constants};
  ComPtr<ID3D11Buffer> constant_buffer;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&constants_desc, &constants_data, &constant_buffer)));
  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> sampler;
  REQUIRE(SUCCEEDED(device->CreateSamplerState(&sampler_desc, &sampler)));
  ID3D11Buffer* constant_buffer_pointer = constant_buffer.Get();
  context->CSSetConstantBuffers(0, 1, &constant_buffer_pointer);
  ID3D11SamplerState* sampler_pointer = sampler.Get();
  context->CSSetSamplers(0, 1, &sampler_pointer);
  ID3D11ShaderResourceView* null_srvs[3] = {};
  ID3D11UnorderedAccessView* null_uav = nullptr;

  SECTION("SMAA") {
    PostEffectTexture color = CreatePostEffectTexture(
        device.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, kSize, color_texels.data(),
        kSize * sizeof(uint32_t));
    PostEffectTexture edges =
        CreatePostEffectTexture(device.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, kSize, nullptr, 0);
    PostEffectTexture weights =
        CreatePostEffectTexture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kSize, nullptr, 0);
    struct Pass {
      std::span<const uint8_t> code;
      ID3D11ShaderResourceView* sources[3];
      ID3D11UnorderedAccessView* dest;
    };
    // No lookup textures: without edges, the weights pass doesn't read them.
    const Pass passes[] = {
        {sm50::smaa_edge_detection_cs, {color.srv.Get()}, edges.uav.Get()},
        {sm50::smaa_blending_weight_calculation_cs, {edges.srv.Get()}, weights.uav.Get()},
        {sm50::smaa_neighborhood_blending_cs,
         {color.srv.Get(), weights.srv.Get()},
         output.uav.Get()},
    };
    for (const Pass& pass : passes) {
      ComPtr<ID3D11ComputeShader> shader;
      REQUIRE(SUCCEEDED(device->CreateComputeShader(pass.code.data(), pass.code.size(), nullptr,
                                                    &shader)));
      context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
      context->CSSetShaderResources(0, 3, pass.sources);
      context->CSSetUnorderedAccessViews(0, 1, &pass.dest, nullptr);
      context->CSSetShader(shader.Get(), nullptr, 0);
      context->Dispatch(1, 2, 1);
    }
    context->CSSetShaderResources(0, 3, null_srvs);
    context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    uint32_t texel = ReadTexel7x5(device.Get(), context.Get(), output.texture.Get());
    CHECK((texel & 0x3FFFFFFF) == (kColor & 0x3FFFFFFF));
  }

  SECTION("FXAA") {
    // R16G16B16A16_UNORM with the luma in alpha, as apply_gamma_*_fxaa_luma
    // write it: R 0.25, G 0.5, B 0.75.
    std::vector<uint16_t> fxaa_texels(kSize * kSize * 4);
    for (uint32_t i = 0; i < kSize * kSize; ++i) {
      fxaa_texels[i * 4 + 0] = 0x4000;
      fxaa_texels[i * 4 + 1] = 0x8000;
      fxaa_texels[i * 4 + 2] = 0xC000;
      fxaa_texels[i * 4 + 3] = 0x7000;
    }
    PostEffectTexture source = CreatePostEffectTexture(
        device.Get(), DXGI_FORMAT_R16G16B16A16_UNORM, kSize, fxaa_texels.data(),
        kSize * 4 * sizeof(uint16_t));
    ComPtr<ID3D11ComputeShader> shader;
    REQUIRE(SUCCEEDED(
        device->CreateComputeShader(sm50::fxaa_cs, sizeof(sm50::fxaa_cs), nullptr, &shader)));
    ID3D11ShaderResourceView* source_srv = source.srv.Get();
    context->CSSetShaderResources(0, 1, &source_srv);
    ID3D11UnorderedAccessView* output_uav = output.uav.Get();
    context->CSSetUnorderedAccessViews(0, 1, &output_uav, nullptr);
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->Dispatch(1, 2, 1);
    context->CSSetShaderResources(0, 1, null_srvs);
    context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    uint32_t texel = ReadTexel7x5(device.Get(), context.Get(), output.texture.Get());
    uint32_t r = texel & 0x3FF, g = (texel >> 10) & 0x3FF, b = (texel >> 20) & 0x3FF;
    CHECK(r >= 255);
    CHECK(r <= 257);
    CHECK(g >= 511);
    CHECK(g <= 513);
    CHECK(b >= 767);
    CHECK(b <= 769);
  }
}

// VIZ predication on D3D11 (D3D11CommandProcessor::IssueDraw): a draw under
// SetPredication(survey predicate, FALSE) is skipped only when the survey's
// draws passed no samples, and occlusion queries count what is drawn.
TEST_CASE("VIZ predicates skip draws only after an empty survey (WARP)", "[graphics][d3d11]") {
  using Microsoft::WRL::ComPtr;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                               D3D11_SDK_VERSION, &device, nullptr, &context))) {
    SKIP("WARP is not available");
  }
  constexpr uint32_t kSize = 8;
  D3D11_TEXTURE2D_DESC target_desc = {};
  target_desc.Width = kSize;
  target_desc.Height = kSize;
  target_desc.MipLevels = 1;
  target_desc.ArraySize = 1;
  target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  target_desc.SampleDesc.Count = 1;
  target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> target;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&target_desc, nullptr, &target)));
  ComPtr<ID3D11RenderTargetView> rtv;
  REQUIRE(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
  ComPtr<ID3D11VertexShader> vertex_shader;
  REQUIRE(SUCCEEDED(device->CreateVertexShader(sm50::fullscreen_cw_vs,
                                               sizeof(sm50::fullscreen_cw_vs), nullptr,
                                               &vertex_shader)));
  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  rasterizer_desc.ScissorEnable = TRUE;
  ComPtr<ID3D11RasterizerState> rasterizer_state;
  REQUIRE(SUCCEEDED(device->CreateRasterizerState(&rasterizer_desc, &rasterizer_state)));

  ID3D11RenderTargetView* rtv_pointer = rtv.Get();
  context->OMSetRenderTargets(1, &rtv_pointer, nullptr);
  D3D11_VIEWPORT viewport = {0.0f, 0.0f, float(kSize), float(kSize), 0.0f, 1.0f};
  context->RSSetViewports(1, &viewport);
  context->RSSetState(rasterizer_state.Get());
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vertex_shader.Get(), nullptr, 0);
  const D3D11_RECT kEmptyScissor = {0, 0, 0, 0};
  const D3D11_RECT kFullScissor = {0, 0, LONG(kSize), LONG(kSize)};

  // Surveys: one whose draw is fully scissored out, one that covers the target.
  D3D11_QUERY_DESC predicate_desc = {D3D11_QUERY_OCCLUSION_PREDICATE, 0};
  ComPtr<ID3D11Predicate> survey_empty, survey_visible;
  REQUIRE(SUCCEEDED(device->CreatePredicate(&predicate_desc, &survey_empty)));
  REQUIRE(SUCCEEDED(device->CreatePredicate(&predicate_desc, &survey_visible)));
  context->RSSetScissorRects(1, &kEmptyScissor);
  context->Begin(survey_empty.Get());
  context->Draw(3, 0);
  context->End(survey_empty.Get());
  context->RSSetScissorRects(1, &kFullScissor);
  context->Begin(survey_visible.Get());
  context->Draw(3, 0);
  context->End(survey_visible.Get());

  // Consumer draws under each predicate, counted by occlusion queries.
  D3D11_QUERY_DESC query_desc = {D3D11_QUERY_OCCLUSION, 0};
  auto count_samples = [&](ID3D11Predicate* predicate) {
    ComPtr<ID3D11Query> query;
    REQUIRE(SUCCEEDED(device->CreateQuery(&query_desc, &query)));
    context->Begin(query.Get());
    context->SetPredication(predicate, FALSE);
    context->Draw(3, 0);
    context->SetPredication(nullptr, FALSE);
    context->End(query.Get());
    UINT64 samples = 0;
    while (context->GetData(query.Get(), &samples, sizeof(samples), 0) == S_FALSE) {
    }
    return samples;
  };
  CHECK(count_samples(survey_empty.Get()) == 0);
  CHECK(count_samples(survey_visible.Get()) == kSize * kSize);
  CHECK(count_samples(nullptr) == kSize * kSize);
}
#endif
