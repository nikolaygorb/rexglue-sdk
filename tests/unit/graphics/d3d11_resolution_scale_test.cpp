/**
 * @file        graphics/d3d11_resolution_scale_test.cpp
 * @brief       WARP checks of the Direct3D 11 reads of scaled resolve memory
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32

#include <cstdint>
#include <cstring>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace {

namespace shaders {
#include "graphics/shaders/bytecode/d3d11_5_0/resolve_downscale_cs.h"
#include "graphics/shaders/bytecode/d3d11_5_0/texture_load_32bpb_cs.h"
#include "graphics/shaders/bytecode/d3d11_5_0/texture_load_32bpb_scaled_cs.h"
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

struct RawBuffer {
  ComPtr<ID3D11Buffer> buffer;
  ComPtr<ID3D11ShaderResourceView> srv;
  ComPtr<ID3D11UnorderedAccessView> uav;
};

RawBuffer CreateRawBuffer(ID3D11Device* device, uint32_t size, const void* data = nullptr) {
  RawBuffer raw;
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = size;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  D3D11_SUBRESOURCE_DATA initial_data = {data};
  REQUIRE(SUCCEEDED(device->CreateBuffer(&desc, data ? &initial_data : nullptr, &raw.buffer)));
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srv_desc.BufferEx.NumElements = size / 4;
  srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  REQUIRE(SUCCEEDED(device->CreateShaderResourceView(raw.buffer.Get(), &srv_desc, &raw.srv)));
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.NumElements = size / 4;
  uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  REQUIRE(SUCCEEDED(device->CreateUnorderedAccessView(raw.buffer.Get(), &uav_desc, &raw.uav)));
  return raw;
}

ComPtr<ID3D11Buffer> CreateConstants(ID3D11Device* device, const void* data, uint32_t size) {
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = (size + 15) & ~uint32_t(15);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  std::vector<uint8_t> padded(desc.ByteWidth);
  std::memcpy(padded.data(), data, size);
  D3D11_SUBRESOURCE_DATA initial_data = {padded.data()};
  ComPtr<ID3D11Buffer> buffer;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&desc, &initial_data, &buffer)));
  return buffer;
}

void RunCompute(ID3D11DeviceContext* context, ID3D11Device* device, const void* shader_code,
                size_t shader_size, const void* constants, uint32_t constants_size,
                ID3D11ShaderResourceView* source, ID3D11UnorderedAccessView* dest,
                uint32_t groups_x, uint32_t groups_y) {
  ComPtr<ID3D11ComputeShader> shader;
  REQUIRE(SUCCEEDED(device->CreateComputeShader(shader_code, shader_size, nullptr, &shader)));
  ComPtr<ID3D11Buffer> constant_buffer = CreateConstants(device, constants, constants_size);
  context->CSSetShader(shader.Get(), nullptr, 0);
  ID3D11Buffer* constant_buffer_ptr = constant_buffer.Get();
  context->CSSetConstantBuffers(0, 1, &constant_buffer_ptr);
  context->CSSetUnorderedAccessViews(0, 1, &dest, nullptr);
  context->CSSetShaderResources(0, 1, &source);
  context->Dispatch(groups_x, groups_y, 1);
  ID3D11ShaderResourceView* null_srv = nullptr;
  ID3D11UnorderedAccessView* null_uav = nullptr;
  context->CSSetShaderResources(0, 1, &null_srv);
  context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
}

std::vector<uint8_t> ReadBuffer(ID3D11Device* device, ID3D11DeviceContext* context,
                                ID3D11Buffer* buffer, uint32_t size) {
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = size;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Buffer> staging;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &staging)));
  D3D11_BOX box = {0, 0, 0, size, 1, 1};
  context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, buffer, 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapping;
  REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
  std::vector<uint8_t> data(size);
  std::memcpy(data.data(), mapping.pData, size);
  context->Unmap(staging.Get(), 0);
  return data;
}

// TextureCache::LoadConstants of a tiled 2D 32bpb level.
struct LoadConstants {
  uint32_t is_tiled_3d_endian_scale;
  uint32_t guest_offset;
  uint32_t guest_pitch_aligned;
  uint32_t guest_z_stride_block_rows_aligned;
  uint32_t size_blocks[3];
  uint32_t host_offset;
  uint32_t host_pitch;
  uint32_t height_texels;
};

}  // namespace

// The scaled resolve memory has two readers in the D3D11 backend: the scaled
// texture load shaders (a scaled texture, bound with the whole scaled resolve
// buffer and the range's offset as guest_offset) and the readback's
// resolve_downscale_cs (the whole buffer, the offset as source_offset_bytes).
// Both reverse the group-packed scaled layout: the top-left host texel of
// each guest texel of the scaled load must be what the downscale keeps, also
// read back by the unscaled load.
TEST_CASE("D3D11 scaled texture loads and the readback downscale agree (WARP)",
          "[graphics][d3d11]") {
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device = CreateWarpDevice(context);
  if (!device) {
    SKIP("WARP is not available");
  }
  // Two 32x32 macro tiles of 32bpp at 1x.
  constexpr uint32_t kWidth = 64, kHeight = 32;
  constexpr uint32_t kSizeUnscaled = kWidth * kHeight * 4;
  // The range doesn't start at the start of its buffer.
  constexpr uint32_t kRangeOffset = 0x10000;
  const uint32_t scales[][2] = {{2, 2}, {3, 2}, {1, 3}};
  for (const auto& scale : scales) {
    uint32_t scale_x = scale[0], scale_y = scale[1];
    uint32_t scale_area = scale_x * scale_y;
    uint32_t size_scaled = kSizeUnscaled * scale_area;
    std::vector<uint32_t> scaled_data((kRangeOffset + size_scaled + 0x1000) / 4);
    uint32_t seed = 0x12345678u ^ scale_area;
    for (uint32_t& dword : scaled_data) {
      seed = seed * 1664525u + 1013904223u;
      dword = seed;
    }
    RawBuffer scaled = CreateRawBuffer(device.Get(), uint32_t(scaled_data.size() * 4),
                                       scaled_data.data());

    // Scaled load.
    uint32_t scaled_width = kWidth * scale_x, scaled_height = kHeight * scale_y;
    uint32_t scaled_pitch = (scaled_width * 4 + 255) & ~uint32_t(255);
    RawBuffer scaled_decoded = CreateRawBuffer(device.Get(), scaled_pitch * scaled_height);
    LoadConstants load = {};
    load.is_tiled_3d_endian_scale = 1 | (scale_x << 4) | (scale_y << 7);
    load.guest_offset = kRangeOffset;
    load.guest_pitch_aligned = kWidth;
    load.guest_z_stride_block_rows_aligned = kHeight;
    load.size_blocks[0] = scaled_width;
    load.size_blocks[1] = scaled_height;
    load.size_blocks[2] = 1;
    load.host_pitch = scaled_pitch;
    load.height_texels = kHeight;
    // More groups than needed: the load shaders check the size.
    RunCompute(context.Get(), device.Get(), shaders::texture_load_32bpb_scaled_cs,
               sizeof(shaders::texture_load_32bpb_scaled_cs), &load, sizeof(load),
               scaled.srv.Get(), scaled_decoded.uav.Get(), 16, 16);

    // Downscale, then the unscaled load of the result.
    RawBuffer downscaled = CreateRawBuffer(device.Get(), kSizeUnscaled);
    const uint32_t downscale[] = {scale_x, scale_y, 2, kSizeUnscaled / 4096, kRangeOffset, 0};
    RunCompute(context.Get(), device.Get(), shaders::resolve_downscale_cs,
               sizeof(shaders::resolve_downscale_cs), downscale, sizeof(downscale),
               scaled.srv.Get(), downscaled.uav.Get(), kSizeUnscaled / 4096, 1);
    uint32_t unscaled_pitch = (kWidth * 4 + 255) & ~uint32_t(255);
    RawBuffer unscaled_decoded = CreateRawBuffer(device.Get(), unscaled_pitch * kHeight);
    load.is_tiled_3d_endian_scale = 1 | (1 << 4) | (1 << 7);
    load.guest_offset = 0;
    load.size_blocks[0] = kWidth;
    load.size_blocks[1] = kHeight;
    load.host_pitch = unscaled_pitch;
    RunCompute(context.Get(), device.Get(), shaders::texture_load_32bpb_cs,
               sizeof(shaders::texture_load_32bpb_cs), &load, sizeof(load),
               downscaled.srv.Get(), unscaled_decoded.uav.Get(), 16, 16);

    std::vector<uint8_t> scaled_texels = ReadBuffer(device.Get(), context.Get(),
                                                    scaled_decoded.buffer.Get(),
                                                    scaled_pitch * scaled_height);
    std::vector<uint8_t> unscaled_texels = ReadBuffer(device.Get(), context.Get(),
                                                      unscaled_decoded.buffer.Get(),
                                                      unscaled_pitch * kHeight);
    // Each 1x texel is in the scaled data exactly once, so all must match.
    uint32_t mismatches = 0;
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        uint32_t unscaled_texel, scaled_texel;
        std::memcpy(&unscaled_texel, unscaled_texels.data() + y * unscaled_pitch + x * 4, 4);
        std::memcpy(&scaled_texel,
                    scaled_texels.data() + y * scale_y * scaled_pitch + x * scale_x * 4, 4);
        mismatches += uint32_t(unscaled_texel != scaled_texel);
      }
    }
    INFO("scale " << scale_x << "x" << scale_y);
    CHECK(mismatches == 0);
  }
}

#endif  // _WIN32
