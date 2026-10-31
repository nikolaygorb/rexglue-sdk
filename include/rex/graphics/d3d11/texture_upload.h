/**
 * @file        graphics/d3d11/texture_upload.h
 * @brief       Constants of the Direct3D 11 texture upload shaders
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <dxgiformat.h>

namespace rex::graphics::d3d11 {

// d3d11_texture_upload_2d_array_cs and d3d11_texture_upload_3d_cs store texels
// from a raw buffer (t0) through a UINT view (u0) of a typeless texture, with
// these constants at b0 (d3d11_texture_upload.hlsli), in 8x8x1 groups.
struct TextureUploadConstants {
  uint32_t offset;
  uint32_t row_pitch;
  // Between 3D slices or array layers.
  uint32_t slice_pitch;
  uint32_t texel_bytes;
  uint32_t size[3];
  uint32_t dest_z;
  uint32_t components[4];
};
static_assert(sizeof(TextureUploadConstants) == 48);

constexpr uint32_t kTextureUploadGroupSize = 8;

// A component of TextureUploadConstants: source bit offset, source bit count
// (0 for all ones), destination bit count (0 for none). A destination wider
// than the source repeats the source bits.
constexpr uint32_t TextureUploadComponent(uint32_t source_offset, uint32_t source_bits,
                                          uint32_t dest_bits) {
  return source_offset | (source_bits << 8) | (dest_bits << 16);
}

struct TextureUploadLayout {
  uint32_t texel_bytes;
  uint32_t components[4];
};

// Texels written as they are, with the components of the view's format.
// Zero texel_bytes for a format without an upload.
constexpr TextureUploadLayout GetTextureUploadLayout(DXGI_FORMAT upload_view) {
  uint32_t component_count = 0, component_bits = 0;
  switch (upload_view) {
    case DXGI_FORMAT_R8_UINT:
      component_count = 1;
      component_bits = 8;
      break;
    case DXGI_FORMAT_R8G8_UINT:
      component_count = 2;
      component_bits = 8;
      break;
    case DXGI_FORMAT_R8G8B8A8_UINT:
      component_count = 4;
      component_bits = 8;
      break;
    case DXGI_FORMAT_R10G10B10A2_UINT:
      return {4,
              {TextureUploadComponent(0, 10, 10), TextureUploadComponent(10, 10, 10),
               TextureUploadComponent(20, 10, 10), TextureUploadComponent(30, 2, 2)}};
    case DXGI_FORMAT_R16_UINT:
      component_count = 1;
      component_bits = 16;
      break;
    case DXGI_FORMAT_R16G16_UINT:
      component_count = 2;
      component_bits = 16;
      break;
    case DXGI_FORMAT_R16G16B16A16_UINT:
      component_count = 4;
      component_bits = 16;
      break;
    case DXGI_FORMAT_R32_UINT:
      component_count = 1;
      component_bits = 32;
      break;
    case DXGI_FORMAT_R32G32_UINT:
      component_count = 2;
      component_bits = 32;
      break;
    case DXGI_FORMAT_R32G32B32A32_UINT:
      component_count = 4;
      component_bits = 32;
      break;
    default:
      return {};
  }
  TextureUploadLayout layout = {};
  layout.texel_bytes = component_count * component_bits / 8;
  for (uint32_t i = 0; i < component_count; ++i) {
    layout.components[i] =
        TextureUploadComponent(i * component_bits, component_bits, component_bits);
  }
  return layout;
}

// Guest 16-bit packed texels as the 16bpb load shader writes them (untiled and
// byte-swapped, red in the low bits), widened to R8G8B8A8_UINT.
constexpr TextureUploadLayout kTextureUploadLayoutR5G5B5A1 = {
    2,
    {TextureUploadComponent(0, 5, 8), TextureUploadComponent(5, 5, 8),
     TextureUploadComponent(10, 5, 8), TextureUploadComponent(15, 1, 8)}};
constexpr TextureUploadLayout kTextureUploadLayoutR5G6B5 = {
    2,
    {TextureUploadComponent(0, 5, 8), TextureUploadComponent(5, 6, 8),
     TextureUploadComponent(11, 5, 8), TextureUploadComponent(0, 0, 8)}};
constexpr TextureUploadLayout kTextureUploadLayoutR5G5B6 = {
    2,
    {TextureUploadComponent(0, 5, 8), TextureUploadComponent(5, 5, 8),
     TextureUploadComponent(10, 6, 8), TextureUploadComponent(0, 0, 8)}};
constexpr TextureUploadLayout kTextureUploadLayoutR4G4B4A4 = {
    2,
    {TextureUploadComponent(0, 4, 8), TextureUploadComponent(4, 4, 8),
     TextureUploadComponent(8, 4, 8), TextureUploadComponent(12, 4, 8)}};
// The DXT3A as 1:1:1:1 load shader's B4G4R4A4 output.
constexpr TextureUploadLayout kTextureUploadLayoutB4G4R4A4 = {
    2,
    {TextureUploadComponent(8, 4, 8), TextureUploadComponent(4, 4, 8),
     TextureUploadComponent(0, 4, 8), TextureUploadComponent(12, 4, 8)}};

}  // namespace rex::graphics::d3d11
