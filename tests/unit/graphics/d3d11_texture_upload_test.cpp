/**
 * @file        graphics/d3d11_texture_upload_test.cpp
 * @brief       WARP checks of the Direct3D 11 texture load and upload passes
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <rex/graphics/d3d11/texture_upload.h>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

namespace {

namespace shaders {
#include "graphics/shaders/bytecode/d3d11_5_0/d3d11_texture_upload_2d_array_cs.h"
#include "graphics/shaders/bytecode/d3d11_5_0/d3d11_texture_upload_3d_cs.h"
#include "graphics/shaders/bytecode/d3d11_5_0/texture_load_128bpb_cs.h"
#include "graphics/shaders/bytecode/d3d11_5_0/texture_load_16bpb_cs.h"
#include "graphics/shaders/bytecode/d3d11_5_0/texture_load_32bpb_cs.h"
#include "graphics/shaders/bytecode/d3d11_5_0/texture_load_64bpb_cs.h"
}  // namespace shaders

using Microsoft::WRL::ComPtr;
using namespace rex::graphics::d3d11;

ComPtr<ID3D11Device> CreateWarpDevice(ComPtr<ID3D11DeviceContext>& context) {
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                               D3D11_SDK_VERSION, &device, nullptr, &context))) {
    return nullptr;
  }
  return device;
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
  device->CreateBuffer(&desc, &initial_data, &buffer);
  return buffer;
}

struct Pass {
  // Linear guest data (pitch of 256 bytes, slices of 32 rows) and its load.
  const std::vector<uint8_t>* guest;
  const void* load_shader;
  size_t load_shader_size;
  uint32_t bytes_per_block;
  // xenos::Endian.
  uint32_t endian;
  uint32_t width, height, depth;
  bool is_3d;
  DXGI_FORMAT resource_format;
  DXGI_FORMAT upload_view;
  TextureUploadLayout upload_layout;
  // Bytes of a texel of the resource format.
  uint32_t resource_texel_bytes;
};

constexpr uint32_t kGuestPitch = 256;
constexpr uint32_t kGuestSliceRows = 32;

// The two passes of D3D11TextureCache::LoadTextureDataFromResidentMemoryImpl
// for a single level: the load shader from the guest data into a buffer, then
// the upload into the texture through its UINT view. Returns the texture's
// texels, row by row and slice by slice.
std::vector<uint8_t> LoadAndUpload(ID3D11Device* device, ID3D11DeviceContext* context,
                                   const Pass& pass) {
  // Source (the shared memory).
  D3D11_BUFFER_DESC buffer_desc = {};
  buffer_desc.ByteWidth = uint32_t(std::max(pass.guest->size(), size_t(4096)));
  buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  buffer_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  std::vector<uint8_t> guest_padded(buffer_desc.ByteWidth);
  std::memcpy(guest_padded.data(), pass.guest->data(), pass.guest->size());
  D3D11_SUBRESOURCE_DATA guest_data = {guest_padded.data()};
  ComPtr<ID3D11Buffer> source;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&buffer_desc, &guest_data, &source)));
  D3D11_SHADER_RESOURCE_VIEW_DESC raw_srv_desc = {};
  raw_srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  raw_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  raw_srv_desc.BufferEx.NumElements = buffer_desc.ByteWidth / 4;
  raw_srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  ComPtr<ID3D11ShaderResourceView> source_srv;
  REQUIRE(SUCCEEDED(device->CreateShaderResourceView(source.Get(), &raw_srv_desc, &source_srv)));

  // Decode buffer, rows padded to 256 bytes as in the texture cache.
  uint32_t host_pitch = (pass.width * pass.upload_layout.texel_bytes + 255) & ~uint32_t(255);
  uint32_t host_plane = host_pitch * pass.height;
  buffer_desc.ByteWidth = std::max(host_plane * pass.depth, uint32_t(4096));
  ComPtr<ID3D11Buffer> decoded;
  REQUIRE(SUCCEEDED(device->CreateBuffer(&buffer_desc, nullptr, &decoded)));
  raw_srv_desc.BufferEx.NumElements = buffer_desc.ByteWidth / 4;
  ComPtr<ID3D11ShaderResourceView> decoded_srv;
  REQUIRE(SUCCEEDED(device->CreateShaderResourceView(decoded.Get(), &raw_srv_desc, &decoded_srv)));
  D3D11_UNORDERED_ACCESS_VIEW_DESC raw_uav_desc = {};
  raw_uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  raw_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  raw_uav_desc.Buffer.NumElements = buffer_desc.ByteWidth / 4;
  raw_uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ComPtr<ID3D11UnorderedAccessView> decoded_uav;
  REQUIRE(SUCCEEDED(device->CreateUnorderedAccessView(decoded.Get(), &raw_uav_desc, &decoded_uav)));

  // TextureCache::LoadConstants.
  uint32_t load_constants[10] = {};
  load_constants[0] = (uint32_t(pass.is_3d) << 1) | (pass.endian << 2) | (1 << 4) | (1 << 7);
  load_constants[1] = 0;
  load_constants[2] = kGuestPitch / pass.bytes_per_block;
  load_constants[3] = kGuestSliceRows;
  load_constants[4] = pass.width;
  load_constants[5] = pass.height;
  load_constants[6] = pass.depth;
  load_constants[7] = 0;
  load_constants[8] = host_pitch;
  load_constants[9] = pass.height;
  ComPtr<ID3D11Buffer> load_constant_buffer =
      CreateConstants(device, load_constants, sizeof(load_constants));
  REQUIRE(load_constant_buffer);
  ComPtr<ID3D11ComputeShader> load_shader;
  REQUIRE(SUCCEEDED(
      device->CreateComputeShader(pass.load_shader, pass.load_shader_size, nullptr, &load_shader)));
  context->CSSetShader(load_shader.Get(), nullptr, 0);
  ID3D11Buffer* constant_buffer = load_constant_buffer.Get();
  context->CSSetConstantBuffers(0, 1, &constant_buffer);
  ID3D11UnorderedAccessView* uav = decoded_uav.Get();
  context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ID3D11ShaderResourceView* srv = source_srv.Get();
  context->CSSetShaderResources(0, 1, &srv);
  // More groups than needed: the load shaders check the size.
  context->Dispatch((pass.width + 3) / 4, (pass.height + 31) / 32, pass.depth);

  // The texture and the upload.
  ComPtr<ID3D11Resource> texture;
  D3D11_UNORDERED_ACCESS_VIEW_DESC texture_uav_desc = {};
  texture_uav_desc.Format = pass.upload_view;
  if (pass.is_3d) {
    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = pass.width;
    desc.Height = pass.height;
    desc.Depth = pass.depth;
    desc.MipLevels = 1;
    desc.Format = pass.resource_format;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture3D> texture_3d;
    REQUIRE(SUCCEEDED(device->CreateTexture3D(&desc, nullptr, &texture_3d)));
    texture = texture_3d;
    texture_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
    texture_uav_desc.Texture3D.WSize = UINT(-1);
  } else {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = pass.width;
    desc.Height = pass.height;
    desc.MipLevels = 1;
    desc.ArraySize = pass.depth;
    desc.Format = pass.resource_format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> texture_2d;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture_2d)));
    texture = texture_2d;
    texture_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    texture_uav_desc.Texture2DArray.ArraySize = pass.depth;
  }
  ComPtr<ID3D11UnorderedAccessView> texture_uav;
  REQUIRE(SUCCEEDED(
      device->CreateUnorderedAccessView(texture.Get(), &texture_uav_desc, &texture_uav)));

  TextureUploadConstants upload_constants = {};
  upload_constants.row_pitch = host_pitch;
  upload_constants.slice_pitch = host_plane;
  upload_constants.texel_bytes = pass.upload_layout.texel_bytes;
  upload_constants.size[0] = pass.width;
  upload_constants.size[1] = pass.height;
  upload_constants.size[2] = pass.depth;
  std::memcpy(upload_constants.components, pass.upload_layout.components,
              sizeof(upload_constants.components));
  ComPtr<ID3D11Buffer> upload_constant_buffer =
      CreateConstants(device, &upload_constants, sizeof(upload_constants));
  REQUIRE(upload_constant_buffer);
  ComPtr<ID3D11ComputeShader> upload_shader;
  if (pass.is_3d) {
    REQUIRE(SUCCEEDED(device->CreateComputeShader(shaders::d3d11_texture_upload_3d_cs,
                                                  sizeof(shaders::d3d11_texture_upload_3d_cs),
                                                  nullptr, &upload_shader)));
  } else {
    REQUIRE(SUCCEEDED(device->CreateComputeShader(
        shaders::d3d11_texture_upload_2d_array_cs,
        sizeof(shaders::d3d11_texture_upload_2d_array_cs), nullptr, &upload_shader)));
  }
  context->CSSetShader(upload_shader.Get(), nullptr, 0);
  constant_buffer = upload_constant_buffer.Get();
  context->CSSetConstantBuffers(0, 1, &constant_buffer);
  // The UAV first: the decode buffer is still bound as an output.
  uav = texture_uav.Get();
  context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  srv = decoded_srv.Get();
  context->CSSetShaderResources(0, 1, &srv);
  context->Dispatch((pass.width + kTextureUploadGroupSize - 1) / kTextureUploadGroupSize,
                    (pass.height + kTextureUploadGroupSize - 1) / kTextureUploadGroupSize,
                    pass.depth);
  ID3D11UnorderedAccessView* null_uav = nullptr;
  context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
  ID3D11ShaderResourceView* null_srv = nullptr;
  context->CSSetShaderResources(0, 1, &null_srv);

  // Read back.
  std::vector<uint8_t> texels(size_t(pass.width) * pass.height * pass.depth *
                              pass.resource_texel_bytes);
  ComPtr<ID3D11Resource> staging;
  if (pass.is_3d) {
    D3D11_TEXTURE3D_DESC desc;
    static_cast<ID3D11Texture3D*>(texture.Get())->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture3D> staging_3d;
    REQUIRE(SUCCEEDED(device->CreateTexture3D(&desc, nullptr, &staging_3d)));
    staging = staging_3d;
  } else {
    D3D11_TEXTURE2D_DESC desc;
    static_cast<ID3D11Texture2D*>(texture.Get())->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging_2d;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging_2d)));
    staging = staging_2d;
  }
  context->CopyResource(staging.Get(), texture.Get());
  uint32_t row_bytes = pass.width * pass.resource_texel_bytes;
  for (uint32_t z = 0; z < pass.depth; ++z) {
    D3D11_MAPPED_SUBRESOURCE mapping;
    uint32_t subresource = pass.is_3d ? 0 : z;
    REQUIRE(SUCCEEDED(context->Map(staging.Get(), subresource, D3D11_MAP_READ, 0, &mapping)));
    const uint8_t* plane =
        static_cast<const uint8_t*>(mapping.pData) + (pass.is_3d ? z * mapping.DepthPitch : 0);
    for (uint32_t y = 0; y < pass.height; ++y) {
      std::memcpy(texels.data() + (size_t(z) * pass.height + y) * row_bytes,
                  plane + y * mapping.RowPitch, row_bytes);
    }
    context->Unmap(staging.Get(), subresource);
  }
  return texels;
}

uint32_t Widen(uint32_t value, uint32_t bits) {
  if (bits == 1) {
    return value ? 255 : 0;
  }
  return (value << (8 - bits)) | (value >> (2 * bits - 8));
}

}  // namespace

// The k_8_8_8_8 path: 32bpb load with the 8-in-32 swap, R8G8B8A8_UINT upload
// of a 2D array.
TEST_CASE("D3D11 texture upload stores 32bpb texels exactly (WARP)", "[graphics][d3d11]") {
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device = CreateWarpDevice(context);
  if (!device) {
    SKIP("WARP is not available");
  }
  constexpr uint32_t kWidth = 40, kHeight = 12, kLayers = 2;
  std::vector<uint8_t> guest(kGuestPitch * kGuestSliceRows * kLayers);
  std::vector<uint32_t> expected;
  for (uint32_t z = 0; z < kLayers; ++z) {
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        uint32_t texel = (x * 0x01020305u) ^ (y * 0x0A0B0C0Du) ^ (z * 0x55000011u);
        expected.push_back(texel);
        uint8_t* stored = guest.data() + (z * kGuestSliceRows + y) * kGuestPitch + x * 4;
        // Big-endian in guest memory (xenos::Endian::k8in32).
        stored[0] = uint8_t(texel >> 24);
        stored[1] = uint8_t(texel >> 16);
        stored[2] = uint8_t(texel >> 8);
        stored[3] = uint8_t(texel);
      }
    }
  }
  Pass pass = {&guest,
               shaders::texture_load_32bpb_cs,
               sizeof(shaders::texture_load_32bpb_cs),
               4,
               2,
               kWidth,
               kHeight,
               kLayers,
               false,
               DXGI_FORMAT_R8G8B8A8_TYPELESS,
               DXGI_FORMAT_R8G8B8A8_UINT,
               GetTextureUploadLayout(DXGI_FORMAT_R8G8B8A8_UINT),
               4};
  std::vector<uint8_t> texels = LoadAndUpload(device.Get(), context.Get(), pass);
  REQUIRE(texels.size() == expected.size() * 4);
  CHECK(std::memcmp(texels.data(), expected.data(), texels.size()) == 0);
}

// k_1_5_5_5, k_5_6_5, k_6_5_5 and k_4_4_4_4 are loaded as 16bpb and widened to
// R8G8B8A8 by repeating the bits (constant one for the missing alpha).
TEST_CASE("D3D11 texture upload widens 16-bit packed texels (WARP)", "[graphics][d3d11]") {
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device = CreateWarpDevice(context);
  if (!device) {
    SKIP("WARP is not available");
  }
  struct Packed {
    const char* name;
    TextureUploadLayout layout;
    uint32_t bits[4];
  };
  const Packed packed_formats[] = {
      {"1_5_5_5", kTextureUploadLayoutR5G5B5A1, {5, 5, 5, 1}},
      {"5_6_5", kTextureUploadLayoutR5G6B5, {5, 6, 5, 0}},
      {"6_5_5", kTextureUploadLayoutR5G5B6, {5, 5, 6, 0}},
      {"4_4_4_4", kTextureUploadLayoutR4G4B4A4, {4, 4, 4, 4}},
  };
  constexpr uint32_t kWidth = 33, kHeight = 9;
  for (const Packed& format : packed_formats) {
    INFO(format.name);
    std::vector<uint8_t> guest(kGuestPitch * kGuestSliceRows);
    std::vector<uint8_t> expected;
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        uint16_t texel = uint16_t((x * 0x9E37u) ^ (y * 0x3C1Bu) ^ 0x5A5Au);
        uint8_t* stored = guest.data() + y * kGuestPitch + x * 2;
        // xenos::Endian::k8in16.
        stored[0] = uint8_t(texel >> 8);
        stored[1] = uint8_t(texel);
        uint32_t shift = 0;
        for (uint32_t i = 0; i < 4; ++i) {
          uint32_t bits = format.bits[i];
          if (!bits) {
            expected.push_back(255);
            continue;
          }
          expected.push_back(uint8_t(Widen((texel >> shift) & ((1u << bits) - 1), bits)));
          shift += bits;
        }
      }
    }
    Pass pass = {&guest,
                 shaders::texture_load_16bpb_cs,
                 sizeof(shaders::texture_load_16bpb_cs),
                 2,
                 1,
                 kWidth,
                 kHeight,
                 1,
                 false,
                 DXGI_FORMAT_R8G8B8A8_TYPELESS,
                 DXGI_FORMAT_R8G8B8A8_UINT,
                 format.layout,
                 4};
    std::vector<uint8_t> texels = LoadAndUpload(device.Get(), context.Get(), pass);
    REQUIRE(texels.size() == expected.size());
    CHECK(texels == expected);
  }
}

// 3D upload and the R10G10B10A2 component split.
TEST_CASE("D3D11 texture upload stores 3D R10G10B10A2 texels (WARP)", "[graphics][d3d11]") {
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device = CreateWarpDevice(context);
  if (!device) {
    SKIP("WARP is not available");
  }
  constexpr uint32_t kWidth = 9, kHeight = 10, kDepth = 3;
  std::vector<uint8_t> guest(kGuestPitch * kGuestSliceRows * kDepth);
  std::vector<uint32_t> expected;
  for (uint32_t z = 0; z < kDepth; ++z) {
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        uint32_t texel = (x * 0x12345679u) ^ (y * 0x0F0E0D0Bu) ^ (z * 0xC0000401u);
        expected.push_back(texel);
        uint8_t* stored = guest.data() + (z * kGuestSliceRows + y) * kGuestPitch + x * 4;
        stored[0] = uint8_t(texel >> 24);
        stored[1] = uint8_t(texel >> 16);
        stored[2] = uint8_t(texel >> 8);
        stored[3] = uint8_t(texel);
      }
    }
  }
  Pass pass = {&guest,
               shaders::texture_load_32bpb_cs,
               sizeof(shaders::texture_load_32bpb_cs),
               4,
               2,
               kWidth,
               kHeight,
               kDepth,
               true,
               DXGI_FORMAT_R10G10B10A2_TYPELESS,
               DXGI_FORMAT_R10G10B10A2_UINT,
               GetTextureUploadLayout(DXGI_FORMAT_R10G10B10A2_UINT),
               4};
  std::vector<uint8_t> texels = LoadAndUpload(device.Get(), context.Get(), pass);
  REQUIRE(texels.size() == expected.size() * 4);
  CHECK(std::memcmp(texels.data(), expected.data(), texels.size()) == 0);
}

// The first half of a block-compressed texture's load: DXT blocks (8in16, as
// DXT textures usually are) loaded as they are by the 64bpb and 128bpb load
// shaders, with a host block per texel, and uploaded into the UINT texture of
// blocks (layers of a 2D array here, one at a time in the texture cache). The
// copy into the BC texture is checked by the next test.
TEST_CASE("D3D11 texture upload stores DXT blocks as they are (WARP)", "[graphics][d3d11]") {
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device = CreateWarpDevice(context);
  if (!device) {
    SKIP("WARP is not available");
  }
  for (uint32_t block_bytes : {8u, 16u}) {
    // In blocks.
    constexpr uint32_t kWidth = 6, kHeight = 3, kLayers = 2;
    std::vector<uint8_t> guest(kGuestPitch * kGuestSliceRows * kLayers);
    std::vector<uint8_t> expected;
    for (uint32_t z = 0; z < kLayers; ++z) {
      for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
          uint8_t* stored =
              guest.data() + (z * kGuestSliceRows + y) * kGuestPitch + x * block_bytes;
          for (uint32_t i = 0; i < block_bytes; ++i) {
            uint8_t value = uint8_t(x * 37 + y * 11 + z * 101 + i * 3 + 1);
            expected.push_back(value);
            // xenos::Endian::k8in16.
            stored[i ^ 1] = value;
          }
        }
      }
    }
    DXGI_FORMAT block_format =
        block_bytes == 16 ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32_UINT;
    Pass pass = {&guest,
                 block_bytes == 16 ? shaders::texture_load_128bpb_cs : shaders::texture_load_64bpb_cs,
                 block_bytes == 16 ? sizeof(shaders::texture_load_128bpb_cs)
                                   : sizeof(shaders::texture_load_64bpb_cs),
                 block_bytes,
                 1,
                 kWidth,
                 kHeight,
                 kLayers,
                 false,
                 block_format,
                 block_format,
                 GetTextureUploadLayout(block_format),
                 block_bytes};
    std::vector<uint8_t> texels = LoadAndUpload(device.Get(), context.Get(), pass);
    INFO("block bytes " << block_bytes);
    REQUIRE(texels.size() == expected.size());
    CHECK(texels == expected);
  }
}

// d3d11_resolve_into_textures: a k_8_8_8_8 render target (R8G8B8A8_UNORM) is
// copied from into a texture of the same guest format (R8G8B8A8_TYPELESS, with
// the texture cache's bindings), a part of it at an offset, texel for texel,
// with the debug layer clean when it's installed.
TEST_CASE("D3D11 render targets are copied into k_8_8_8_8 textures (WARP)", "[graphics][d3d11]") {
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  bool debug = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                           D3D11_CREATE_DEVICE_DEBUG, &level, 1,
                                           D3D11_SDK_VERSION, &device, nullptr, &context));
  if (!debug) {
    device = CreateWarpDevice(context);
    if (!device) {
      SKIP("WARP is not available");
    }
  }
  ComPtr<ID3D11InfoQueue> info_queue;
  if (debug) {
    device.As(&info_queue);
  }
  // Two 80x16 tiles wide; the surface is at (80, 16), 60x13.
  constexpr uint32_t kRtWidth = 160, kRtHeight = 32;
  constexpr uint32_t kX = 80, kY = 16, kWidth = 60, kHeight = 13;
  std::vector<uint32_t> rt_texels(kRtWidth * kRtHeight);
  for (uint32_t i = 0; i < rt_texels.size(); ++i) {
    rt_texels[i] = i * 0x9E3779B9u;
  }
  D3D11_TEXTURE2D_DESC rt_desc = {};
  rt_desc.Width = kRtWidth;
  rt_desc.Height = kRtHeight;
  rt_desc.MipLevels = 1;
  rt_desc.ArraySize = 1;
  rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rt_desc.SampleDesc.Count = 1;
  rt_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA rt_data = {rt_texels.data(), kRtWidth * sizeof(uint32_t), 0};
  ComPtr<ID3D11Texture2D> rt;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&rt_desc, &rt_data, &rt)));
  D3D11_TEXTURE2D_DESC texture_desc = {};
  texture_desc.Width = kWidth;
  texture_desc.Height = kHeight;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&texture_desc, nullptr, &texture)));

  D3D11_BOX box = {kX, kY, 0, kX + kWidth, kY + kHeight, 1};
  context->CopySubresourceRegion(texture.Get(), 0, 0, 0, 0, rt.Get(), 0, &box);

  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&texture_desc, nullptr, &staging)));
  context->CopyResource(staging.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapping;
  REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
  uint32_t mismatches = 0;
  for (uint32_t y = 0; y < kHeight; ++y) {
    mismatches += std::memcmp(static_cast<const uint8_t*>(mapping.pData) + y * mapping.RowPitch,
                              rt_texels.data() + (kY + y) * kRtWidth + kX,
                              kWidth * sizeof(uint32_t))
                      ? 1
                      : 0;
  }
  context->Unmap(staging.Get(), 0);
  CHECK(mismatches == 0);
  if (info_queue) {
    uint64_t errors = 0;
    for (uint64_t i = 0; i < info_queue->GetNumStoredMessages(); ++i) {
      SIZE_T size = 0;
      info_queue->GetMessage(i, nullptr, &size);
      std::vector<uint8_t> message_data(size);
      auto message = reinterpret_cast<D3D11_MESSAGE*>(message_data.data());
      info_queue->GetMessage(i, message, &size);
      if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
        UNSCOPED_INFO(std::string(message->pDescription, message->DescriptionByteLength));
        ++errors;
      }
    }
    CHECK(errors == 0);
  }
}

// D3D11TextureCache creates cube maps with unordered access for the upload,
// written through a 2D array view of the six faces.
TEST_CASE("D3D11 cube textures accept the upload view (WARP)", "[graphics][d3d11]") {
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device> device = CreateWarpDevice(context);
  if (!device) {
    SKIP("WARP is not available");
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = 16;
  desc.Height = 16;
  desc.MipLevels = 5;
  desc.ArraySize = 6;
  desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
  ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
  uav_desc.Texture2DArray.MipSlice = 4;
  uav_desc.Texture2DArray.ArraySize = 6;
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(SUCCEEDED(device->CreateUnorderedAccessView(texture.Get(), &uav_desc, &uav)));
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
  srv_desc.TextureCube.MipLevels = 5;
  ComPtr<ID3D11ShaderResourceView> srv;
  CHECK(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), &srv_desc, &srv)));
}

// Block-compressed host textures (D3D11TextureCache with
// d3d11_block_compressed_textures): the upload writes the guest blocks into a
// single-layer UINT texture with a texel per block, which is copied into each
// level and layer of the BC texture, also into the levels smaller than a
// block. Checked with the debug layer when it's installed.
TEST_CASE("D3D11 BC levels are copied from a texture of blocks (WARP)", "[graphics][d3d11]") {
  struct Case {
    DXGI_FORMAT bc_format;
    DXGI_FORMAT block_format;
    uint32_t block_bytes;
    uint32_t width, height, levels, layers;
  };
  const Case cases[] = {
      {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_R32G32_UINT, 8, 8, 8, 4, 2},
      {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_R32G32B32A32_UINT, 16, 12, 8, 4, 1},
      {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_R32G32_UINT, 8, 4, 20, 5, 1},
      {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_R32G32B32A32_UINT, 16, 16, 4, 5, 3},
  };
  for (const Case& test_case : cases) {
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    bool debug = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                             D3D11_CREATE_DEVICE_DEBUG, &level, 1,
                                             D3D11_SDK_VERSION, &device, nullptr, &context));
    if (!debug) {
      device = CreateWarpDevice(context);
      if (!device) {
        SKIP("WARP is not available");
      }
    }
    ComPtr<ID3D11InfoQueue> info_queue;
    if (debug) {
      device.As(&info_queue);
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = test_case.width;
    desc.Height = test_case.height;
    desc.MipLevels = test_case.levels;
    desc.ArraySize = test_case.layers;
    desc.Format = test_case.bc_format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));

    uint32_t blocks_x = (test_case.width + 3) / 4, blocks_y = (test_case.height + 3) / 4;
    D3D11_TEXTURE2D_DESC blocks_desc = {};
    blocks_desc.Width = blocks_x;
    blocks_desc.Height = blocks_y;
    blocks_desc.MipLevels = 1;
    blocks_desc.ArraySize = 1;
    blocks_desc.Format = test_case.block_format;
    blocks_desc.SampleDesc.Count = 1;
    blocks_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> blocks;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&blocks_desc, nullptr, &blocks)));

    std::vector<std::vector<uint8_t>> expected;
    for (uint32_t layer = 0; layer < test_case.layers; ++layer) {
      for (uint32_t mip = 0; mip < test_case.levels; ++mip) {
        uint32_t mip_blocks_x = (std::max(test_case.width >> mip, uint32_t(1)) + 3) / 4;
        uint32_t mip_blocks_y = (std::max(test_case.height >> mip, uint32_t(1)) + 3) / 4;
        std::vector<uint8_t> data(size_t(mip_blocks_x) * mip_blocks_y * test_case.block_bytes);
        for (size_t i = 0; i < data.size(); ++i) {
          data[i] = uint8_t(i * 7 + mip * 31 + layer * 101 + 1);
        }
        D3D11_BOX box = {0, 0, 0, mip_blocks_x, mip_blocks_y, 1};
        context->UpdateSubresource(blocks.Get(), 0, &box, data.data(),
                                   mip_blocks_x * test_case.block_bytes, 0);
        context->CopySubresourceRegion(texture.Get(),
                                       D3D11CalcSubresource(mip, layer, test_case.levels), 0, 0,
                                       0, blocks.Get(), 0, &box);
        expected.push_back(std::move(data));
      }
    }

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
    context->CopyResource(staging.Get(), texture.Get());
    for (uint32_t layer = 0; layer < test_case.layers; ++layer) {
      for (uint32_t mip = 0; mip < test_case.levels; ++mip) {
        uint32_t subresource = D3D11CalcSubresource(mip, layer, test_case.levels);
        const std::vector<uint8_t>& data = expected[subresource];
        uint32_t mip_blocks_x = (std::max(test_case.width >> mip, uint32_t(1)) + 3) / 4;
        uint32_t row_bytes = mip_blocks_x * test_case.block_bytes;
        D3D11_MAPPED_SUBRESOURCE mapping;
        REQUIRE(SUCCEEDED(context->Map(staging.Get(), subresource, D3D11_MAP_READ, 0, &mapping)));
        bool same = true;
        for (uint32_t row = 0; row * row_bytes < data.size(); ++row) {
          same &= !std::memcmp(static_cast<const uint8_t*>(mapping.pData) + row * mapping.RowPitch,
                               data.data() + row * row_bytes, row_bytes);
        }
        context->Unmap(staging.Get(), subresource);
        INFO("format " << uint32_t(test_case.bc_format) << ", level " << mip << ", layer "
                       << layer);
        CHECK(same);
      }
    }
    if (info_queue) {
      uint64_t errors = 0;
      for (uint64_t i = 0; i < info_queue->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0;
        info_queue->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> message_data(size);
        auto message = reinterpret_cast<D3D11_MESSAGE*>(message_data.data());
        info_queue->GetMessage(i, message, &size);
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
          UNSCOPED_INFO(std::string(message->pDescription, message->DescriptionByteLength));
          ++errors;
        }
      }
      CHECK(errors == 0);
    }
  }
}

#endif  // _WIN32
