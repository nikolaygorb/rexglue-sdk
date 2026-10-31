/**
 * @file        graphics/d3d11_geometry_shader_test.cpp
 * @brief       Tests for the Direct3D 11 backend's point, rectangle and quad
 *              list expansion geometry shaders
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

#include <rex/graphics/d3d11/geometry_shader.h>
#include <rex/graphics/d3d11/shader_bytecode.h>
#include <rex/graphics/format/dxbc.h>

#ifdef _WIN32
#include <d3d11.h>
#endif

namespace {

using rex::graphics::d3d11::ConvertShaderBytecode;
using rex::graphics::d3d11::CreateDxbcGeometryShader;
using rex::graphics::d3d11::GeometryShaderKey;
using rex::graphics::d3d11::GeometryShaderType;
using rex::graphics::d3d11::ShaderBytecode;

// Every expansion type with a spread of the other key fields.
std::vector<GeometryShaderKey> TestKeys() {
  std::vector<GeometryShaderKey> keys;
  for (uint32_t type = 1; type <= 3; ++type) {
    for (uint32_t interpolators : {0u, 1u, 8u, 16u}) {
      for (uint32_t clip_planes : {0u, 3u, 6u}) {
        for (uint32_t flags = 0; flags < 16; ++flags) {
          for (uint32_t ps_ucp_mode = 0; ps_ucp_mode < 4; ++ps_ucp_mode) {
            GeometryShaderKey key;
            key.type = GeometryShaderType(type);
            key.interpolator_count = interpolators;
            key.user_clip_plane_count = clip_planes;
            key.user_clip_plane_cull = flags & 1;
            key.has_vertex_kill_and = (flags >> 1) & 1;
            key.has_point_size = (flags >> 2) & 1;
            key.has_point_coordinates = (flags >> 3) & 1;
            key.point_ps_ucp_mode = ps_ucp_mode;
            keys.push_back(key);
          }
        }
      }
    }
  }
  return keys;
}

std::span<const uint8_t> Bytes(const std::vector<uint32_t>& words) {
  return {reinterpret_cast<const uint8_t*>(words.data()), words.size() * sizeof(uint32_t)};
}

}  // namespace

TEST_CASE("Expansion geometry shaders convert to SM 5.0", "[graphics][d3d11]") {
  std::vector<uint32_t> dxbc;
  for (GeometryShaderKey key : TestKeys()) {
    INFO("key " << key.key);
    CreateDxbcGeometryShader(key, dxbc);
    REQUIRE(!dxbc.empty());
    ShaderBytecode output;
    std::string error;
    REQUIRE(ConvertShaderBytecode(Bytes(dxbc), {}, output, error));
    CHECK(output.program_type == uint32_t(rex::graphics::dxbc::ProgramType::kGeometryShader));
  }
}

#ifdef _WIN32
TEST_CASE("Expansion geometry shaders are accepted at feature level 11.0 (WARP)",
          "[graphics][d3d11]") {
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ID3D11Device* device = nullptr;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                               D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
    SKIP("WARP is not available");
  }
  std::vector<uint32_t> dxbc;
  for (GeometryShaderKey key : TestKeys()) {
    INFO("key " << key.key);
    CreateDxbcGeometryShader(key, dxbc);
    ShaderBytecode output;
    std::string error;
    REQUIRE(ConvertShaderBytecode(Bytes(dxbc), {}, output, error));
    ID3D11GeometryShader* shader = nullptr;
    CHECK(SUCCEEDED(
        device->CreateGeometryShader(output.data.data(), output.data.size(), nullptr, &shader)));
    if (shader) {
      shader->Release();
    }
  }
  device->Release();
}
#endif
