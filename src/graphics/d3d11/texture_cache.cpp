/**
 * @file        graphics/d3d11/texture_cache.cpp
 * @brief       Texture cache of the experimental Direct3D 11 backend
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *              Formats and loading follow the D3D12 texture cache; the upload
 *              through a typeless texture's UINT view follows
 *              TheSimpsonsGameRecomp (tools/rexglue-sdk/src/graphics/d3d11/
 *              texture_upload.cpp, commit 4ff637bc, BSD 3-Clause).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/texture_cache.h>

#include <algorithm>
#include <cstring>

#include <rex/assert.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/replacement.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/math.h>

REXCVAR_DEFINE_BOOL(d3d11_block_compressed_textures, true, "GPU/D3D11",
                    "Keep DXT and DXN textures compressed in video memory when their size "
                    "allows (as Direct3D 12 does), instead of decompressing them. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::d3d11 {

namespace shaders {
#include "../shaders/bytecode/d3d11_5_0/d3d11_texture_upload_2d_array_cs.h"
#include "../shaders/bytecode/d3d11_5_0/d3d11_texture_upload_3d_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_128bpb_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_128bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_16bpb_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_16bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_32bpb_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_32bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_64bpb_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_64bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_8bpb_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_8bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_bgrg8_rgb8_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_ctx1_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_depth_float_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_depth_float_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_depth_unorm_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_depth_unorm_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_dxn_rg8_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_dxt1_rgba8_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_dxt3_rgba8_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_dxt3a_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_dxt3aas1111_bgra4_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_dxt5_rgba8_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_dxt5a_r8_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_gbgr8_rgb8_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_r10g11b11_rgba16_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_r10g11b11_rgba16_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_r10g11b11_rgba16_snorm_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_r11g11b10_rgba16_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_r11g11b10_rgba16_scaled_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "../shaders/bytecode/d3d11_5_0/texture_load_r11g11b10_rgba16_snorm_scaled_cs.h"
}  // namespace shaders

namespace {
// D3D12TextureCache::AreDimensionsCompatible without 3D textures for 2D
// bindings, which read the 2D copy of a slice instead (IsRead3DAs2D).
constexpr bool IsRead3DAs2D(xenos::FetchOpDimension binding_dimension,
                            xenos::DataDimension resource_dimension) {
  return resource_dimension == xenos::DataDimension::k3D &&
         (binding_dimension == xenos::FetchOpDimension::k1D ||
          binding_dimension == xenos::FetchOpDimension::k2D);
}

constexpr bool AreDimensionsCompatible(xenos::FetchOpDimension binding_dimension,
                                       xenos::DataDimension resource_dimension) {
  switch (binding_dimension) {
    case xenos::FetchOpDimension::k1D:
    case xenos::FetchOpDimension::k2D:
      return resource_dimension == xenos::DataDimension::k1D ||
             resource_dimension == xenos::DataDimension::k2DOrStacked;
    case xenos::FetchOpDimension::k3DOrStacked:
      return resource_dimension == xenos::DataDimension::k3D;
    case xenos::FetchOpDimension::kCube:
      return resource_dimension == xenos::DataDimension::kCube;
    default:
      return false;
  }
}
}  // namespace

#define REX_D3D11_UNSUPPORTED_FORMAT(swizzle)                                            \
  {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, \
   kLoadShaderIndexUnknown, kLoadShaderIndexUnknown, nullptr, xenos::swizzle}

// In the order of xenos::TextureFormat, with the swizzles of the D3D12 table
// (except for the formats widened to R8G8B8A8, which have their natural
// component order there).
const D3D11TextureCache::HostFormat D3D11TextureCache::host_formats_[64] = {
    // k_1_REVERSE
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_1
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_8
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_UINT,
     kLoadShaderIndex8bpb, kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1_5_5_5
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndex16bpb, kLoadShaderIndexUnknown,
     &kTextureUploadLayoutR5G5B5A1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_5_6_5
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndex16bpb, kLoadShaderIndexUnknown,
     &kTextureUploadLayoutR5G6B5, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_6_5_5
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndex16bpb, kLoadShaderIndexUnknown,
     &kTextureUploadLayoutR5G5B6, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_8_8_8_8
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndex32bpb, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R10G10B10A2_UINT, kLoadShaderIndex32bpb, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_A
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_UINT,
     kLoadShaderIndex8bpb, kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_B
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_UINT,
     kLoadShaderIndex8bpb, kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_8
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM,
     DXGI_FORMAT_R8G8_UINT, kLoadShaderIndex16bpb, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_Cr_Y1_Cb_Y0_REP
    // Always decompressed (no typed UAV for G8R8_G8B8).
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndexGBGR8ToRGB8, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_Y1_Cr_Y0_Cb_REP
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndexBGRG8ToRGB8, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_8_8_8_8_A
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndex32bpb, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_4_4_4_4
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndex16bpb, kLoadShaderIndexUnknown,
     &kTextureUploadLayoutR4G4B4A4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT,
     kLoadShaderIndexR11G11B10ToRGBA16, kLoadShaderIndexR11G11B10ToRGBA16SNorm, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT,
     kLoadShaderIndexR10G11B11ToRGBA16, kLoadShaderIndexR10G11B11ToRGBA16SNorm, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT1
    // Decompressed unless IsHostBlockCompressed (no UAV for BC).
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndexDXT1ToRGBA8, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndexDXT3ToRGBA8, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndexDXT5ToRGBA8, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_16_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RGBA),
    // R32_FLOAT for depth because shaders would require an additional SRV to
    // sample stencil, which we don't provide.
    // k_24_8
    {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT,
     kLoadShaderIndexDepthUnorm, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_24_8_FLOAT
    {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT,
     kLoadShaderIndexDepthFloat, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16_UINT,
     kLoadShaderIndex16bpb, kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM,
     DXGI_FORMAT_R16G16_UINT, kLoadShaderIndex32bpb, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT, kLoadShaderIndex64bpb,
     kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_EXPAND
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_UINT,
     kLoadShaderIndex16bpb, kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_EXPAND
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
     DXGI_FORMAT_R16G16_UINT, kLoadShaderIndex32bpb, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_EXPAND
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT,
     DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UINT, kLoadShaderIndex64bpb,
     kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_FLOAT
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_UINT,
     kLoadShaderIndex16bpb, kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_FLOAT
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
     DXGI_FORMAT_R16G16_UINT, kLoadShaderIndex32bpb, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_FLOAT
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT,
     DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UINT, kLoadShaderIndex64bpb,
     kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_32_32
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_32_32_32_32
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RGBA),
    // k_32_FLOAT
    {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT,
     kLoadShaderIndex32bpb, kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32_FLOAT
    {DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
     DXGI_FORMAT_R32G32_UINT, kLoadShaderIndex64bpb, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32_FLOAT
    {DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT,
     DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT, kLoadShaderIndex128bpb,
     kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_AS_8
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_32_AS_8_8
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_16_MPEG
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_16_16_MPEG
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_8_INTERLACED
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_32_AS_8_INTERLACED
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_32_AS_8_8_INTERLACED
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_16_INTERLACED
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_16_MPEG_INTERLACED
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_16_16_MPEG_INTERLACED
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_DXN
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8_UINT, kLoadShaderIndexDXNToRG8, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_AS_16_16_16_16
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndex32bpb, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT1_AS_16_16_16_16
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndexDXT1ToRGBA8, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3_AS_16_16_16_16
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndexDXT3ToRGBA8, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5_AS_16_16_16_16
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndexDXT5ToRGBA8, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_AS_16_16_16_16
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R10G10B10A2_UINT, kLoadShaderIndex32bpb, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT,
     kLoadShaderIndexR11G11B10ToRGBA16, kLoadShaderIndexR11G11B10ToRGBA16SNorm, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT,
     kLoadShaderIndexR10G11B11ToRGBA16, kLoadShaderIndexR10G11B11ToRGBA16SNorm, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_32_32_32_FLOAT
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RGBB),
    // k_DXT3A
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8_UINT,
     kLoadShaderIndexDXT3A, kLoadShaderIndexUnknown, nullptr, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_DXT5A
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8_UINT,
     kLoadShaderIndexDXT5AToR8, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_CTX1
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8_UINT, kLoadShaderIndexCTX1, kLoadShaderIndexUnknown, nullptr,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXT3A_AS_1_1_1_1
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_R8G8B8A8_UINT, kLoadShaderIndexDXT3AAs1111ToBGRA4, kLoadShaderIndexUnknown,
     &kTextureUploadLayoutB4G4R4A4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_8_8_8_GAMMA_EDRAM
    // Not usable as a texture.
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RGBA),
    // k_2_10_10_10_FLOAT_EDRAM
    // Not usable as a texture.
    REX_D3D11_UNSUPPORTED_FORMAT(XE_GPU_TEXTURE_SWIZZLE_RGBA),
};

#undef REX_D3D11_UNSUPPORTED_FORMAT

namespace {
// The BC format of a replacement image's blocks for the DXT and DXN formats
// (the formats the host format table decompresses), UNKNOWN for the others.
DXGI_FORMAT GetReplacementBCFormat(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_DXT1:
      return DXGI_FORMAT_BC1_UNORM;
    case xenos::TextureFormat::k_DXT2_3:
      return DXGI_FORMAT_BC2_UNORM;
    case xenos::TextureFormat::k_DXT4_5:
      return DXGI_FORMAT_BC3_UNORM;
    case xenos::TextureFormat::k_DXN:
      return DXGI_FORMAT_BC5_UNORM;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}
}  // namespace

D3D11TextureCache::D3D11TextureCache(const RegisterFile& register_file,
                                     D3D11SharedMemory& shared_memory,
                                     uint32_t draw_resolution_scale_x,
                                     uint32_t draw_resolution_scale_y,
                                     D3D11CommandProcessor& command_processor)
    : TextureCache(register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      d3d11_shared_memory_(shared_memory) {}

D3D11TextureCache::~D3D11TextureCache() {
  // The textures reference this object in their destructors.
  DestroyAllTextures(true);
}

ID3D11Device* D3D11TextureCache::GetDevice() const {
  return command_processor_.GetD3D11Provider().GetDevice();
}

ID3D11DeviceContext* D3D11TextureCache::GetContext() const {
  return command_processor_.GetD3D11Provider().GetImmediateContext();
}

bool D3D11TextureCache::Initialize() {
  ID3D11Device* device = GetDevice();
  block_compressed_textures_ = REXCVAR_GET(d3d11_block_compressed_textures);

  // GetSamplerParameters drops filtering for non-filterable formats.
  host_filterable_unsigned_ = 0;
  host_filterable_signed_ = 0;
  for (uint32_t i = 0; i < rex::countof(host_formats_); ++i) {
    const HostFormat& host_format = host_formats_[i];
    UINT format_support;
    if (host_format.unsigned_view != DXGI_FORMAT_UNKNOWN &&
        SUCCEEDED(device->CheckFormatSupport(host_format.unsigned_view, &format_support)) &&
        (format_support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
      host_filterable_unsigned_ |= uint64_t(1) << i;
    }
    if (host_format.signed_view != DXGI_FORMAT_UNKNOWN &&
        SUCCEEDED(device->CheckFormatSupport(host_format.signed_view, &format_support)) &&
        (format_support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
      host_filterable_signed_ |= uint64_t(1) << i;
    }
  }

  struct LoadShaderCode {
    LoadShaderIndex index;
    const void* code;
    size_t size;
  };
#define REX_D3D11_LOAD_SHADER(index, name) {index, shaders::name, sizeof(shaders::name)}
  const LoadShaderCode load_shader_code[] = {
      REX_D3D11_LOAD_SHADER(kLoadShaderIndex8bpb, texture_load_8bpb_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndex16bpb, texture_load_16bpb_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndex32bpb, texture_load_32bpb_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndex64bpb, texture_load_64bpb_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndex128bpb, texture_load_128bpb_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexGBGR8ToRGB8, texture_load_gbgr8_rgb8_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexBGRG8ToRGB8, texture_load_bgrg8_rgb8_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexR10G11B11ToRGBA16, texture_load_r10g11b11_rgba16_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexR10G11B11ToRGBA16SNorm,
                            texture_load_r10g11b11_rgba16_snorm_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexR11G11B10ToRGBA16, texture_load_r11g11b10_rgba16_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexR11G11B10ToRGBA16SNorm,
                            texture_load_r11g11b10_rgba16_snorm_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexDXT1ToRGBA8, texture_load_dxt1_rgba8_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexDXT3ToRGBA8, texture_load_dxt3_rgba8_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexDXT5ToRGBA8, texture_load_dxt5_rgba8_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexDXNToRG8, texture_load_dxn_rg8_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexDXT3A, texture_load_dxt3a_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexDXT3AAs1111ToBGRA4, texture_load_dxt3aas1111_bgra4_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexDXT5AToR8, texture_load_dxt5a_r8_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexCTX1, texture_load_ctx1_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexDepthUnorm, texture_load_depth_unorm_cs),
      REX_D3D11_LOAD_SHADER(kLoadShaderIndexDepthFloat, texture_load_depth_float_cs),
  };
#undef REX_D3D11_LOAD_SHADER
  for (const LoadShaderCode& load_shader : load_shader_code) {
    if (FAILED(device->CreateComputeShader(load_shader.code, load_shader.size, nullptr,
                                           &load_shaders_[load_shader.index]))) {
      REXGPU_ERROR("D3D11TextureCache: Failed to create the texture load shader {}",
                   uint32_t(load_shader.index));
      return false;
    }
  }
  if (IsDrawResolutionScaled()) {
#define REX_D3D11_LOAD_SHADER(index, name) {index, shaders::name, sizeof(shaders::name)}
    // The formats of the format table that resolves can write.
    const LoadShaderCode load_shader_scaled_code[] = {
        REX_D3D11_LOAD_SHADER(kLoadShaderIndex8bpb, texture_load_8bpb_scaled_cs),
        REX_D3D11_LOAD_SHADER(kLoadShaderIndex16bpb, texture_load_16bpb_scaled_cs),
        REX_D3D11_LOAD_SHADER(kLoadShaderIndex32bpb, texture_load_32bpb_scaled_cs),
        REX_D3D11_LOAD_SHADER(kLoadShaderIndex64bpb, texture_load_64bpb_scaled_cs),
        REX_D3D11_LOAD_SHADER(kLoadShaderIndex128bpb, texture_load_128bpb_scaled_cs),
        REX_D3D11_LOAD_SHADER(kLoadShaderIndexR10G11B11ToRGBA16,
                              texture_load_r10g11b11_rgba16_scaled_cs),
        REX_D3D11_LOAD_SHADER(kLoadShaderIndexR10G11B11ToRGBA16SNorm,
                              texture_load_r10g11b11_rgba16_snorm_scaled_cs),
        REX_D3D11_LOAD_SHADER(kLoadShaderIndexR11G11B10ToRGBA16,
                              texture_load_r11g11b10_rgba16_scaled_cs),
        REX_D3D11_LOAD_SHADER(kLoadShaderIndexR11G11B10ToRGBA16SNorm,
                              texture_load_r11g11b10_rgba16_snorm_scaled_cs),
        REX_D3D11_LOAD_SHADER(kLoadShaderIndexDepthUnorm, texture_load_depth_unorm_scaled_cs),
        REX_D3D11_LOAD_SHADER(kLoadShaderIndexDepthFloat, texture_load_depth_float_scaled_cs),
    };
#undef REX_D3D11_LOAD_SHADER
    for (const LoadShaderCode& load_shader : load_shader_scaled_code) {
      if (FAILED(device->CreateComputeShader(load_shader.code, load_shader.size, nullptr,
                                             &load_shaders_scaled_[load_shader.index]))) {
        REXGPU_ERROR("D3D11TextureCache: Failed to create the scaled texture load shader {}",
                     uint32_t(load_shader.index));
        return false;
      }
    }
  }
  // Every load shader the format table uses must exist.
  for (const HostFormat& host_format : host_formats_) {
    for (LoadShaderIndex index : {host_format.load_shader, host_format.load_shader_signed}) {
      if (index != kLoadShaderIndexUnknown && !load_shaders_[index]) {
        REXGPU_ERROR("D3D11TextureCache: No texture load shader {} for the format table",
                     uint32_t(index));
        return false;
      }
    }
  }

  if (FAILED(device->CreateComputeShader(shaders::d3d11_texture_upload_2d_array_cs,
                                         sizeof(shaders::d3d11_texture_upload_2d_array_cs),
                                         nullptr, &upload_2d_array_shader_)) ||
      FAILED(device->CreateComputeShader(shaders::d3d11_texture_upload_3d_cs,
                                         sizeof(shaders::d3d11_texture_upload_3d_cs), nullptr,
                                         &upload_3d_shader_))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the texture upload shaders");
    return false;
  }

  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.Usage = D3D11_USAGE_DYNAMIC;
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  constants_desc.ByteWidth = UINT(rex::align(sizeof(LoadConstants), size_t(16)));
  if (FAILED(device->CreateBuffer(&constants_desc, nullptr, &load_constants_))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the texture load constant buffer");
    return false;
  }
  constants_desc.ByteWidth = sizeof(TextureUploadConstants);
  if (FAILED(device->CreateBuffer(&constants_desc, nullptr, &upload_constants_))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the texture upload constant buffer");
    return false;
  }
  return true;
}

ID3D11ShaderResourceView* D3D11TextureCache::GetActiveTextureSRV(
    const DxbcShader::TextureBinding& binding, uint32_t& host_swizzle_out) {
  host_swizzle_out = xenos::XE_GPU_TEXTURE_SWIZZLE_0000;
  const TextureBinding* texture_binding = GetValidTextureBinding(binding.fetch_constant);
  if (!texture_binding) {
    CountMissingTexture(binding, stats_.no_fetch_constant, "invalid fetch constant");
    return nullptr;
  }
  bool read_3d_as_2d = IsRead3DAs2D(binding.dimension, texture_binding->key.dimension) &&
                       REXCVAR_GET(gpu_3d_to_2d_texture);
  if (!read_3d_as_2d &&
      !AreDimensionsCompatible(binding.dimension, texture_binding->key.dimension)) {
    CountMissingTexture(binding, stats_.dimension_mismatch, "fetch dimension mismatch");
    return nullptr;
  }
  Texture* texture;
  if (binding.is_signed) {
    if (!texture_util::IsAnySignSigned(texture_binding->swizzled_signs)) {
      // The shader's other (unsigned) binding of the fetch is used.
      ++stats_.sign_unused;
      return nullptr;
    }
    texture = IsSignedVersionSeparateForFormat(texture_binding->key)
                  ? texture_binding->texture_signed
                  : texture_binding->texture;
  } else {
    if (!texture_util::IsAnySignNotSigned(texture_binding->swizzled_signs)) {
      ++stats_.sign_unused;
      return nullptr;
    }
    texture = texture_binding->texture;
  }
  if (!texture) {
    CountMissingTexture(binding, stats_.no_texture, "texture not created or loaded");
    return nullptr;
  }
  D3D11Texture* view_texture = static_cast<D3D11Texture*>(texture);
  if (read_3d_as_2d) {
    // Created or reloaded by RequestTextures.
    view_texture = view_texture->texture_3d_as_2d();
    if (!view_texture) {
      CountMissingTexture(binding, stats_.no_texture, "no 2D copy of the 3D texture");
      return nullptr;
    }
  }
  ID3D11ShaderResourceView* srv = view_texture->GetOrCreateSRV(binding.dimension,
                                                                binding.is_signed);
  if (!srv) {
    CountMissingTexture(binding, stats_.no_view, "no view for the signedness");
    return nullptr;
  }
  texture->MarkAsUsed();
  host_swizzle_out = static_cast<D3D11Texture*>(texture)->red_blue_swapped()
                         ? SwapRedAndBlueSources(texture_binding->host_swizzle)
                         : texture_binding->host_swizzle;
  return srv;
}

void D3D11TextureCache::RequestTextures(uint32_t used_texture_mask) {
  TextureCache::RequestTextures(used_texture_mask);

  // Before the draw state is set up, as the loads use the context.
  if (!REXCVAR_GET(gpu_3d_to_2d_texture)) {
    return;
  }
  uint32_t textures_3d = used_texture_mask;
  uint32_t index_3d;
  while (rex::bit_scan_forward(textures_3d, &index_3d)) {
    textures_3d &= ~(uint32_t(1) << index_3d);
    const TextureBinding* binding = GetValidTextureBinding(index_3d);
    if (!binding || binding->key.dimension != xenos::DataDimension::k3D) {
      continue;
    }
    for (Texture* texture : {binding->texture, binding->texture_signed}) {
      if (texture) {
        static_cast<D3D11Texture*>(texture)->GetOrCreate3DAs2D();
      }
    }
  }
}

void D3D11TextureCache::CountMissingTexture(const DxbcShader::TextureBinding& binding,
                                            uint32_t& counter, const char* reason) {
  ++counter;
  if (!log_missing_textures_) {
    return;
  }
  xenos::xe_gpu_texture_fetch_t fetch = register_file().GetTextureFetch(binding.fetch_constant);
  uint64_t log_key = (uint64_t(fetch.dword_0) ^ (uint64_t(fetch.dword_1) << 32) ^
                      (uint64_t(fetch.dword_2) << 7) ^ (uint64_t(fetch.dword_3) << 21)) +
                     uintptr_t(reason);
  if (!missing_textures_logged_.emplace(log_key, true).second) {
    return;
  }
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (key.is_valid) {
    REXGPU_INFO(
        "D3D11TextureCache: fetch {} ({} {}) reads zero: {}; {} {}x{}x{} {} texture, base at "
        "0x{:08X}",
        binding.fetch_constant, binding.is_signed ? "signed" : "unsigned",
        uint32_t(binding.dimension), reason, key.tiled ? "tiled" : "linear", key.GetWidth(),
        key.GetHeight(), key.GetDepthOrArraySize(), FormatInfo::Get(key.format)->name,
        key.base_page << 12);
  } else {
    REXGPU_INFO(
        "D3D11TextureCache: fetch {} reads zero: {}; fetch constant {:08X} {:08X} {:08X} "
        "{:08X} {:08X} {:08X}",
        binding.fetch_constant, reason, fetch.dword_0, fetch.dword_1, fetch.dword_2,
        fetch.dword_3, fetch.dword_4, fetch.dword_5);
  }
}

ID3D11ShaderResourceView* D3D11TextureCache::RequestSwapTexture(uint32_t& host_swizzle_out,
                                                                xenos::TextureFormat& format_out,
                                                                uint32_t& width_out,
                                                                uint32_t& height_out,
                                                                bool& scaled_out) {
  xenos::xe_gpu_texture_fetch_t fetch = register_file().GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 || key.dimension != xenos::DataDimension::k2DOrStacked ||
      key.GetDepthOrArraySize() != 1) {
    return nullptr;
  }
  auto* texture = static_cast<D3D11Texture*>(FindOrCreateTexture(key));
  if (!texture || !LoadTextureData(*texture)) {
    return nullptr;
  }
  ID3D11ShaderResourceView* srv = texture->GetOrCreateSwapSRV();
  if (!srv) {
    return nullptr;
  }
  texture->MarkAsUsed();
  host_swizzle_out = GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(key));
  if (texture->red_blue_swapped()) {
    host_swizzle_out = SwapRedAndBlueSources(host_swizzle_out);
  }
  format_out = key.format;
  width_out = key.GetWidth();
  height_out = key.GetHeight();
  // Only the texture's key, not the fetch constant's, says whether it's scaled.
  scaled_out = texture->key().scaled_resolve;
  return srv;
}

bool D3D11TextureCache::CopyResolveIntoTexture(uint32_t dest_base, uint32_t dest_pitch_texels,
                                               uint32_t width, uint32_t height,
                                               xenos::TextureFormat format,
                                               xenos::Endian endianness, bool red_blue_swap,
                                               ID3D11Resource* source, uint32_t source_x,
                                               uint32_t source_y) {
  if (IsDrawResolutionScaled() || !width || !height || (dest_base & 0xFFF) || !dest_pitch_texels ||
      (dest_pitch_texels & 31) || width > dest_pitch_texels) {
    return false;
  }
  // The key a fetch constant gives for the surface (BindingInfoFromFetchConstant
  // with one level: no mip page); packed mips may be either.
  TextureKey key;
  key.base_page = (dest_base & 0x1FFFFFFF) >> 12;
  key.dimension = xenos::DataDimension::k2DOrStacked;
  key.tiled = 1;
  key.pitch = dest_pitch_texels >> 5;
  key.format = format;
  key.endianness = endianness;
  key.is_valid = 1;
  auto find_at_size = [&](uint32_t texture_width, uint32_t texture_height) -> Texture* {
    key.width_minus_1 = texture_width - 1;
    key.height_minus_1 = texture_height - 1;
    for (uint32_t packed_mips = 0; packed_mips <= 1; ++packed_mips) {
      key.packed_mips = packed_mips;
      if (Texture* texture = FindExistingTexture(key)) {
        return texture;
      }
    }
    return nullptr;
  };
  Texture* found = find_at_size(width, height);
  if (!found) {
    // The surface's own size, which the resolve rounded up to whole 8x8
    // blocks, can be up to 7 texels smaller each way: the size found last time
    // first, else each of them (not again while no texture has been created
    // since none was found).
    constexpr uint32_t kBlock = xenos::kResolveAlignmentPixels;
    // Every field whole: 17-bit page, 11-bit block counts, 9-bit pitch, 6-bit
    // format, 2-bit endianness, and a bit for "used".
    const uint64_t resolve_id = (uint64_t(1) << 63) | (uint64_t(key.base_page) << 39) |
                                (uint64_t(width / kBlock) << 28) |
                                (uint64_t(height / kBlock) << 17) | (uint64_t(key.pitch) << 8) |
                                (uint64_t(format) << 2) | uint64_t(endianness);
    ResolvedSurfaceSize& last =
        resolved_surface_sizes_[(resolve_id * UINT64_C(0x9E3779B97F4A7C15)) >> 58];
    const bool same_resolve = last.resolve == resolve_id;
    if (same_resolve && last.width) {
      found = find_at_size(last.width, last.height);
    }
    if (!found && !(same_resolve && !last.width && last.textures_created == textures_created())) {
      for (uint32_t narrower = 0; narrower < kBlock && narrower < width && !found; ++narrower) {
        for (uint32_t shorter = narrower ? 0 : 1; shorter < kBlock && shorter < height && !found;
             ++shorter) {
          found = find_at_size(width - narrower, height - shorter);
        }
      }
      last.resolve = resolve_id;
      last.width = found ? key.GetWidth() : 0;
      last.height = found ? key.GetHeight() : 0;
      last.textures_created = textures_created();
    }
  }
  if (!found) {
    // Usually the surface isn't sampled (yet).
    return false;
  }
  // Why not, once for each reason.
  static uint32_t reasons_logged = 0;
  auto log_skip_once = [](uint32_t reason, const std::string& why) {
    if (!(reasons_logged & (UINT32_C(1) << reason))) {
      reasons_logged |= UINT32_C(1) << reason;
      REXGPU_INFO("d3d11_resolve_into_textures: a resolve not copied into its texture: {}", why);
    }
  };
  if (found->is_replacement() || found->GetGuestMipsSize() || !found->GetGuestBaseSize()) {
    log_skip_once(0, fmt::format("the texture at 0x{:08X}: replacement {}, mips {} bytes",
                                 dest_base, uint32_t(found->is_replacement()),
                                 found->GetGuestMipsSize()));
    return false;
  }
  // Only a format loaded as a plain copy of its texels holds what the render
  // target does, bit for bit, in the render target's format group.
  const TextureKey found_key = found->key();
  const HostFormat& host_format = host_formats_[uint32_t(found_key.format)];
  if (GetLoadShaderIndex(found_key) != kLoadShaderIndex32bpb || host_format.packed_upload ||
      host_format.resource != DXGI_FORMAT_R8G8B8A8_TYPELESS) {
    log_skip_once(1, fmt::format("format {} is converted when loaded", uint32_t(format)));
    return false;
  }
  D3D11Texture& texture = *static_cast<D3D11Texture*>(found);
  const uint32_t texture_width = found_key.GetWidth();
  const uint32_t texture_height = found_key.GetHeight();
  D3D11_BOX source_box = {source_x, source_y, 0, source_x + texture_width,
                          source_y + texture_height, 1};
  GetContext()->CopySubresourceRegion(texture.resource(), 0, 0, 0, 0, source, 0, &source_box);

  // The resolve has written the same texels to the guest memory (with red and
  // blue swapped if it swapped them), which outdated the texture; it now holds
  // them. The draws apply the host swizzle of GetActiveTextureSRV, so nothing
  // has to be bound again.
  texture.set_red_blue_swapped(red_blue_swap);
  texture.MarkAsUsed();
  MarkTextureUpToDate(texture);
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXGPU_INFO(
        "D3D11TextureCache: resolved surfaces copied straight into their textures "
        "(d3d11_resolve_into_textures), the first {}x{} at 0x{:08X}",
        texture_width, texture_height, dest_base);
  }
  return true;
}

ID3D11SamplerState* D3D11TextureCache::GetOrCreateSampler(SamplerParameters parameters) {
  auto it = samplers_.find(parameters.value);
  if (it != samplers_.end()) {
    return it->second.Get();
  }
  D3D11_SAMPLER_DESC desc;
  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    desc.Filter = D3D11_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (uint32_t(parameters.aniso_filter) - 1);
  } else {
    D3D11_FILTER_TYPE d3d_filter_min =
        parameters.min_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE d3d_filter_mag =
        parameters.mag_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE d3d_filter_mip =
        parameters.mip_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    desc.Filter = D3D11_ENCODE_BASIC_FILTER(d3d_filter_min, d3d_filter_mag, d3d_filter_mip,
                                            D3D11_FILTER_REDUCTION_TYPE_STANDARD);
    desc.MaxAnisotropy = 1;
  }
  static const D3D11_TEXTURE_ADDRESS_MODE kAddressModeMap[] = {
      /* kRepeat               */ D3D11_TEXTURE_ADDRESS_WRAP,
      /* kMirroredRepeat       */ D3D11_TEXTURE_ADDRESS_MIRROR,
      /* kClampToEdge          */ D3D11_TEXTURE_ADDRESS_CLAMP,
      /* kMirrorClampToEdge    */ D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
      /* kClampToHalfway       */ D3D11_TEXTURE_ADDRESS_CLAMP,
      /* kMirrorClampToHalfway */ D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
      /* kClampToBorder        */ D3D11_TEXTURE_ADDRESS_BORDER,
      /* kMirrorClampToBorder  */ D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
  };
  desc.AddressU = kAddressModeMap[uint32_t(parameters.clamp_x)];
  desc.AddressV = kAddressModeMap[uint32_t(parameters.clamp_y)];
  desc.AddressW = kAddressModeMap[uint32_t(parameters.clamp_z)];
  // LOD biasing is performed in shaders.
  desc.MipLODBias = 0.0f;
  desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  switch (parameters.border_color) {
    case xenos::BorderColor::k_ABGR_White:
      desc.BorderColor[0] = 1.0f;
      desc.BorderColor[1] = 1.0f;
      desc.BorderColor[2] = 1.0f;
      desc.BorderColor[3] = 1.0f;
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
      desc.BorderColor[0] = 0.5f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    case xenos::BorderColor::k_ACBCRY_Black:
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.5f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    default:
      assert_true(parameters.border_color == xenos::BorderColor::k_ABGR_Black);
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.0f;
      desc.BorderColor[3] = 0.0f;
      break;
  }
  if (parameters.force_bc_w_to_max) {
    desc.BorderColor[3] = 1.0f;
  }
  desc.MinLOD = float(parameters.mip_min_level);
  if (parameters.mip_base_map) {
    // See D3D12TextureCache::WriteSampler.
    desc.MaxLOD = desc.MinLOD;
    if (parameters.aniso_filter == xenos::AnisoFilter::kDisabled) {
      assert_false(parameters.mip_linear);
      desc.MaxLOD += 0.25f;
    }
  } else {
    // Maximum mip level is in the texture resource itself.
    desc.MaxLOD = D3D11_FLOAT32_MAX;
  }
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
  if (FAILED(GetDevice()->CreateSamplerState(&desc, &sampler))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create a sampler state");
    return nullptr;
  }
  ID3D11SamplerState* result = sampler.Get();
  samplers_.emplace(parameters.value, std::move(sampler));
  return result;
}

bool D3D11TextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  const HostFormat& host_format = host_formats_[uint32_t(key.format)];
  return host_format.load_shader_signed != kLoadShaderIndexUnknown &&
         host_format.load_shader_signed != host_format.load_shader;
}

bool D3D11TextureCache::CanCreateReplacementTexture(TextureKey key) const {
  if (key.signed_separate || key.scaled_resolve ||
      key.dimension != xenos::DataDimension::k2DOrStacked || key.GetDepthOrArraySize() != 1) {
    return false;
  }
  // k_8_8_8_8 texels are stored as they are in the typeless host format.
  return key.format == xenos::TextureFormat::k_8_8_8_8 ||
         GetReplacementBCFormat(key.format) != DXGI_FORMAT_UNKNOWN;
}

std::unique_ptr<TextureCache::Texture> D3D11TextureCache::CreateReplacementTexture(
    TextureKey key, const texture_replacement::Image& image) {
  if (image.levels.empty() || !CanCreateReplacementTexture(key)) {
    return nullptr;
  }
  DXGI_FORMAT bc_format = GetReplacementBCFormat(key.format);
  // Direct3D 11 needs whole blocks in the base level of a BC texture.
  if (bc_format != DXGI_FORMAT_UNKNOWN &&
      ((image.width | image.height) & (image.block_size - 1))) {
    return nullptr;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = image.width;
  desc.Height = image.height;
  desc.MipLevels = UINT(image.levels.size());
  desc.ArraySize = 1;
  desc.Format =
      bc_format != DXGI_FORMAT_UNKNOWN ? bc_format : host_formats_[uint32_t(key.format)].resource;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  std::vector<D3D11_SUBRESOURCE_DATA> level_data(image.levels.size());
  for (size_t i = 0; i < image.levels.size(); ++i) {
    level_data[i].pSysMem = image.level_data(i);
    level_data[i].SysMemPitch = image.levels[i].row_bytes;
  }
  Microsoft::WRL::ComPtr<ID3D11Texture2D> resource;
  if (FAILED(GetDevice()->CreateTexture2D(&desc, level_data.data(), &resource))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create a {}x{} replacement texture", image.width,
                 image.height);
    return nullptr;
  }
  auto texture = std::unique_ptr<D3D11Texture>(new D3D11Texture(*this, key, std::move(resource)));
  texture->SetReplacementHostData(bc_format, image.data.size());
  return texture;
}

bool D3D11TextureCache::IsScaledResolveSupportedForFormat(TextureKey key) const {
  LoadShaderIndex load_shader = GetLoadShaderIndex(key);
  return load_shader != kLoadShaderIndexUnknown && load_shaders_scaled_[load_shader];
}

uint32_t D3D11TextureCache::GetHostFormatSwizzle(TextureKey key) const {
  return host_formats_[uint32_t(key.format)].swizzle;
}

uint32_t D3D11TextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      // 1D and 2D are emulated as 2D arrays.
      return D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    case xenos::DataDimension::k3D:
      return D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    case xenos::DataDimension::kCube:
      return D3D11_REQ_TEXTURECUBE_DIMENSION;
    default:
      assert_unhandled_case(dimension);
      return 0;
  }
}

uint32_t D3D11TextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      return D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
    case xenos::DataDimension::k3D:
      return D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    case xenos::DataDimension::kCube:
      return D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION / 6 * 6;
    default:
      assert_unhandled_case(dimension);
      return 0;
  }
}

D3D11TextureCache::BlockCompressedFormat D3D11TextureCache::GetBlockCompressedFormat(
    xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      return {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_R32G32_UINT, kLoadShaderIndex64bpb};
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_R32G32B32A32_UINT, kLoadShaderIndex128bpb};
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_R32G32B32A32_UINT, kLoadShaderIndex128bpb};
    case xenos::TextureFormat::k_DXN:
      return {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_R32G32B32A32_UINT, kLoadShaderIndex128bpb};
    case xenos::TextureFormat::k_DXT5A:
      return {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_R32G32_UINT, kLoadShaderIndex64bpb};
    default:
      return {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown};
  }
}

bool D3D11TextureCache::IsHostBlockCompressed(const TextureKey& key) const {
  // The 2D copy of a 3D texture has a 3D key, and is decompressed as the 3D
  // texture is.
  if (!block_compressed_textures_ || key.signed_separate || key.scaled_resolve ||
      key.dimension == xenos::DataDimension::k3D ||
      GetBlockCompressedFormat(key.format).format == DXGI_FORMAT_UNKNOWN) {
    return false;
  }
  // Direct3D 11 needs whole blocks in the base level of a BC texture (it has
  // nothing like D3D12's UnalignedBlockTexturesSupported).
  return !((key.GetWidth() | key.GetHeight()) & 3);
}

TextureCache::LoadShaderIndex D3D11TextureCache::GetLoadShaderIndex(TextureKey key) const {
  const HostFormat& host_format = host_formats_[uint32_t(key.format)];
  if (key.signed_separate) {
    return host_format.load_shader_signed;
  }
  if (IsHostBlockCompressed(key)) {
    return GetBlockCompressedFormat(key.format).load_shader;
  }
  return host_format.load_shader;
}

D3D11TextureCache::D3D11Texture::D3D11Texture(D3D11TextureCache& texture_cache,
                                              const TextureKey& key,
                                              Microsoft::WRL::ComPtr<ID3D11Resource> resource,
                                              bool track_usage)
    : Texture(texture_cache, key, track_usage), resource_(std::move(resource)) {
  D3D11_RESOURCE_DIMENSION resource_dimension;
  resource_->GetType(&resource_dimension);
  is_host_3d_ = resource_dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
  host_block_compressed_ = texture_cache.IsHostBlockCompressed(key);
  const HostFormat& host_format = host_formats_[uint32_t(key.format)];
  uint64_t texel_bytes = GetUploadLayout(host_format).texel_bytes;
  if (host_format.packed_upload) {
    // Widened to R8G8B8A8.
    texel_bytes = 4;
  }
  // Blocks of 4x4 texels if block-compressed.
  uint32_t host_block_size_log2 = 0;
  if (host_block_compressed_) {
    BlockCompressedFormat block_compressed_format = GetBlockCompressedFormat(key.format);
    unsigned_view_format_override_ = block_compressed_format.format;
    texel_bytes = GetTextureUploadLayout(block_compressed_format.block_view).texel_bytes;
    host_block_size_log2 = 2;
  }
  bool is_3d = key.dimension == xenos::DataDimension::k3D;
  uint32_t width = key.GetWidth();
  uint32_t height = key.GetHeight();
  if (key.scaled_resolve) {
    width *= texture_cache.draw_resolution_scale_x();
    height *= texture_cache.draw_resolution_scale_y();
  }
  uint64_t size = 0;
  for (uint32_t level = 0; level <= key.mip_max_level; ++level) {
    size += uint64_t(rex::round_up(std::max(width >> level, uint32_t(1)),
                                   uint32_t(1) << host_block_size_log2) >>
                     host_block_size_log2) *
            (rex::round_up(std::max(height >> level, uint32_t(1)),
                           uint32_t(1) << host_block_size_log2) >>
             host_block_size_log2) *
            (is_3d ? std::max(key.GetDepthOrArraySize() >> level, uint32_t(1))
                   : key.GetDepthOrArraySize()) *
            texel_bytes;
  }
  SetHostMemoryUsage(size);
}

D3D11TextureCache::D3D11Texture::~D3D11Texture() = default;

D3D11TextureCache::D3D11Texture* D3D11TextureCache::D3D11Texture::GetOrCreate3DAs2D() {
  if (!REXCVAR_GET(gpu_3d_to_2d_texture) || !is_host_3d_) {
    return nullptr;
  }
  auto& d3d11_cache = static_cast<D3D11TextureCache&>(texture_cache());
  if (texture_3d_as_2d_) {
    // As in D3D12, only what became outdated is loaded again.
    if (texture_3d_as_2d_->outdated_mask() && !d3d11_cache.LoadTextureData(*texture_3d_as_2d_)) {
      REXGPU_ERROR("D3D11TextureCache: Failed to reload the 2D copy of a 3D texture");
    }
    return texture_3d_as_2d_.get();
  }
  TextureKey key_2d = key();
  key_2d.depth_or_array_size_minus_1 = 0;
  key_2d.mip_max_level = 0;
  Microsoft::WRL::ComPtr<ID3D11Resource> resource_2d =
      d3d11_cache.CreateHostResource(key_2d, false);
  if (!resource_2d) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the 2D copy of a 3D texture");
    return nullptr;
  }
  texture_3d_as_2d_.reset(new D3D11Texture(d3d11_cache, key_2d, std::move(resource_2d), false));
  texture_3d_as_2d_->SetForceLoad3DTiling(true);
  if (!d3d11_cache.LoadTextureData(*texture_3d_as_2d_)) {
    REXGPU_ERROR("D3D11TextureCache: Failed to load the 2D copy of a 3D texture");
    texture_3d_as_2d_.reset();
    return nullptr;
  }
  return texture_3d_as_2d_.get();
}

ID3D11ShaderResourceView* D3D11TextureCache::D3D11Texture::GetOrCreateSRV(
    xenos::FetchOpDimension dimension, bool is_signed) {
  size_t view_index = size_t(dimension) * 2 + size_t(is_signed);
  if (view_index >= srvs_.size()) {
    return nullptr;
  }
  if (srvs_[view_index]) {
    return srvs_[view_index].Get();
  }
  const HostFormat& host_format = host_formats_[uint32_t(key().format)];
  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  if (is_signed) {
    desc.Format = unsigned_view_format_override_ == DXGI_FORMAT_UNKNOWN
                      ? host_format.signed_view
                      : DXGI_FORMAT_UNKNOWN;
  } else {
    desc.Format = unsigned_view_format_override_ == DXGI_FORMAT_UNKNOWN
                      ? host_format.unsigned_view
                      : unsigned_view_format_override_;
  }
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    return nullptr;
  }
  // A replacement texture may have more levels than the guest texture.
  uint32_t level_count = GetHostLevelCount();
  // The 2D copy of a 3D texture (a 3D key, a Texture2D) is a single-layer 2D
  // array.
  xenos::DataDimension view_dimension = key().dimension;
  if (view_dimension == xenos::DataDimension::k3D && !is_host_3d_) {
    view_dimension = xenos::DataDimension::k2DOrStacked;
  }
  switch (view_dimension) {
    case xenos::DataDimension::k3D:
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
      desc.Texture3D.MipLevels = level_count;
      break;
    case xenos::DataDimension::kCube:
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
      desc.TextureCube.MipLevels = level_count;
      break;
    default:
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      desc.Texture2DArray.MipLevels = level_count;
      desc.Texture2DArray.ArraySize = key().GetDepthOrArraySize();
      break;
  }
  auto& d3d11_cache = static_cast<D3D11TextureCache&>(texture_cache());
  if (FAILED(d3d11_cache.GetDevice()->CreateShaderResourceView(resource_.Get(), &desc,
                                                               &srvs_[view_index]))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create a shader resource view of a {} texture",
                 key().GetLogDimensionName());
    return nullptr;
  }
  return srvs_[view_index].Get();
}

ID3D11ShaderResourceView* D3D11TextureCache::D3D11Texture::GetOrCreateSwapSRV() {
  if (swap_srv_) {
    return swap_srv_.Get();
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = unsigned_view_format_override_ == DXGI_FORMAT_UNKNOWN
                    ? host_formats_[uint32_t(key().format)].unsigned_view
                    : unsigned_view_format_override_;
  if (desc.Format == DXGI_FORMAT_UNKNOWN || key().dimension != xenos::DataDimension::k2DOrStacked ||
      key().GetDepthOrArraySize() != 1) {
    return nullptr;
  }
  desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipLevels = 1;
  auto& d3d11_cache = static_cast<D3D11TextureCache&>(texture_cache());
  if (FAILED(d3d11_cache.GetDevice()->CreateShaderResourceView(resource_.Get(), &desc,
                                                               &swap_srv_))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the view of a front buffer");
    return nullptr;
  }
  return swap_srv_.Get();
}

ID3D11UnorderedAccessView* D3D11TextureCache::D3D11Texture::GetOrCreateUploadUAV(
    uint32_t level) {
  if (level >= uavs_.size()) {
    return nullptr;
  }
  if (uavs_[level]) {
    return uavs_[level].Get();
  }
  D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
  desc.Format = host_formats_[uint32_t(key().format)].upload_view;
  if (is_host_3d_) {
    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
    desc.Texture3D.MipSlice = level;
    desc.Texture3D.WSize = UINT(-1);
  } else {
    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MipSlice = level;
    desc.Texture2DArray.ArraySize = key().GetDepthOrArraySize();
  }
  auto& d3d11_cache = static_cast<D3D11TextureCache&>(texture_cache());
  if (FAILED(d3d11_cache.GetDevice()->CreateUnorderedAccessView(resource_.Get(), &desc,
                                                                &uavs_[level]))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the upload view of a {} texture",
                 key().GetLogDimensionName());
    return nullptr;
  }
  return uavs_[level].Get();
}

std::unique_ptr<TextureCache::Texture> D3D11TextureCache::CreateTexture(TextureKey key) {
  const HostFormat& host_format = host_formats_[uint32_t(key.format)];
  if (host_format.resource == DXGI_FORMAT_UNKNOWN ||
      GetLoadShaderIndex(key) == kLoadShaderIndexUnknown) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<ID3D11Resource> resource =
      CreateHostResource(key, key.dimension == xenos::DataDimension::k3D);
  if (!resource) {
    return nullptr;
  }
  return std::unique_ptr<Texture>(new D3D11Texture(*this, key, std::move(resource)));
}

Microsoft::WRL::ComPtr<ID3D11Resource> D3D11TextureCache::CreateHostResource(
    const TextureKey& key, bool host_3d) {
  const HostFormat& host_format = host_formats_[uint32_t(key.format)];
  ID3D11Device* device = GetDevice();
  uint32_t width = key.GetWidth();
  uint32_t height = key.GetHeight();
  if (key.scaled_resolve) {
    width *= draw_resolution_scale_x();
    height *= draw_resolution_scale_y();
  }
  // Written by copies from a texture of blocks, not through a UAV.
  bool block_compressed = !host_3d && IsHostBlockCompressed(key);
  Microsoft::WRL::ComPtr<ID3D11Resource> resource;
  if (host_3d) {
    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Depth = key.GetDepthOrArraySize();
    desc.MipLevels = key.mip_max_level + 1;
    desc.Format = host_format.resource;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D11Texture3D> texture;
    if (FAILED(device->CreateTexture3D(&desc, nullptr, &texture))) {
      return nullptr;
    }
    resource = std::move(texture);
  } else {
    // 1D textures are treated as 2D for simplicity.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = key.mip_max_level + 1;
    desc.ArraySize = key.GetDepthOrArraySize();
    desc.Format =
        block_compressed ? GetBlockCompressedFormat(key.format).format : host_format.resource;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = block_compressed
                         ? D3D11_BIND_SHADER_RESOURCE
                         : D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (key.dimension == xenos::DataDimension::kCube) {
      desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) {
      return nullptr;
    }
    resource = std::move(texture);
  }
  return resource;
}

bool D3D11TextureCache::EnsureDecodeBuffer(uint32_t size) {
  if (decode_buffer_size_ >= size) {
    return true;
  }
  decode_buffer_srv_.Reset();
  decode_buffer_uav_.Reset();
  decode_buffer_.Reset();
  decode_buffer_size_ = 0;
  ID3D11Device* device = GetDevice();
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = std::max(uint32_t(1) << 20, rex::next_pow2(size));
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  if (FAILED(device->CreateBuffer(&desc, nullptr, &decode_buffer_))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create a {} byte texture decode buffer",
                 desc.ByteWidth);
    return false;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srv_desc.BufferEx.NumElements = desc.ByteWidth / 4;
  srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.NumElements = desc.ByteWidth / 4;
  uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(device->CreateShaderResourceView(decode_buffer_.Get(), &srv_desc,
                                              &decode_buffer_srv_)) ||
      FAILED(device->CreateUnorderedAccessView(decode_buffer_.Get(), &uav_desc,
                                               &decode_buffer_uav_))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the views of the texture decode buffer");
    decode_buffer_srv_.Reset();
    decode_buffer_uav_.Reset();
    decode_buffer_.Reset();
    return false;
  }
  decode_buffer_size_ = desc.ByteWidth;
  return true;
}

ID3D11UnorderedAccessView* D3D11TextureCache::GetBlockUploadTexture(
    uint32_t block_bytes, uint32_t width_blocks, uint32_t height_blocks,
    ID3D11Texture2D*& texture_out) {
  assert_true(block_bytes == 8 || block_bytes == 16);
  BlockUploadTexture& block_texture = block_upload_textures_[block_bytes == 16];
  if (block_texture.width_blocks < width_blocks || block_texture.height_blocks < height_blocks) {
    block_texture.uav.Reset();
    block_texture.texture.Reset();
    // Grown in both directions, so square and narrow textures don't cause
    // each other's recreation.
    uint32_t new_width = std::max(block_texture.width_blocks, width_blocks);
    uint32_t new_height = std::max(block_texture.height_blocks, height_blocks);
    block_texture.width_blocks = 0;
    block_texture.height_blocks = 0;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = new_width;
    desc.Height = new_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = block_bytes == 16 ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = desc.Format;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    uav_desc.Texture2DArray.ArraySize = 1;
    ID3D11Device* device = GetDevice();
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &block_texture.texture)) ||
        FAILED(device->CreateUnorderedAccessView(block_texture.texture.Get(), &uav_desc,
                                                 &block_texture.uav))) {
      REXGPU_ERROR("D3D11TextureCache: Failed to create a {}x{} texture of {}-byte blocks",
                   new_width, new_height, block_bytes);
      block_texture.uav.Reset();
      block_texture.texture.Reset();
      return nullptr;
    }
    block_texture.width_blocks = new_width;
    block_texture.height_blocks = new_height;
  }
  texture_out = block_texture.texture.Get();
  return block_texture.uav.Get();
}

bool D3D11TextureCache::EnsureScaledResolveMemoryCommitted(uint32_t start_unscaled,
                                                           uint32_t length_unscaled,
                                                           uint32_t length_scaled_alignment_log2) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }
  if (!length_unscaled) {
    return true;
  }
  if (start_unscaled > SharedMemory::kBufferSize ||
      SharedMemory::kBufferSize - start_unscaled < length_unscaled) {
    return false;
  }
  uint32_t draw_resolution_scale_area = draw_resolution_scale_x() * draw_resolution_scale_y();
  uint64_t start_scaled = uint64_t(start_unscaled) * draw_resolution_scale_area;
  uint64_t length_scaled_alignment_bits = (uint64_t(1) << length_scaled_alignment_log2) - 1;
  uint64_t end_scaled =
      start_scaled + ((uint64_t(length_unscaled) * draw_resolution_scale_area +
                       length_scaled_alignment_bits) &
                      ~length_scaled_alignment_bits);

  // Already in one buffer?
  auto it = scaled_resolve_buffers_.upper_bound(start_scaled);
  if (it != scaled_resolve_buffers_.begin()) {
    auto containing = std::prev(it);
    if (containing->first + containing->second.length_scaled >= end_scaled) {
      return true;
    }
  }

  // A new buffer for the aligned range and every buffer it overlaps.
  constexpr uint64_t kAlignment = uint64_t(1) << kScaledResolveBufferAlignmentLog2;
  uint64_t new_start = start_scaled & ~(kAlignment - 1);
  uint64_t new_end = std::min(rex::align(end_scaled, kAlignment),
                              uint64_t(SharedMemory::kBufferSize) * draw_resolution_scale_area);
  auto overlap_first = scaled_resolve_buffers_.upper_bound(new_start);
  if (overlap_first != scaled_resolve_buffers_.begin()) {
    auto previous = std::prev(overlap_first);
    if (previous->first + previous->second.length_scaled > new_start) {
      overlap_first = previous;
    }
  }
  auto overlap_end = overlap_first;
  while (overlap_end != scaled_resolve_buffers_.end() && overlap_end->first < new_end) {
    new_start = std::min(new_start, overlap_end->first);
    new_end = std::max(new_end, overlap_end->first + overlap_end->second.length_scaled);
    ++overlap_end;
  }
  uint64_t new_length = new_end - new_start;
  if (new_length > kMaxScaledResolveBufferSize) {
    if (!scaled_resolve_buffer_failure_logged_) {
      scaled_resolve_buffer_failure_logged_ = true;
      REXGPU_ERROR(
          "D3D11TextureCache: A scaled resolve range of {} MB is over the {} MB a buffer view can "
          "address; the resolves and textures using it are skipped",
          new_length >> 20, kMaxScaledResolveBufferSize >> 20);
    }
    return false;
  }

  ScaledResolveBuffer new_buffer;
  new_buffer.length_scaled = new_length;
  ID3D11Device* device = GetDevice();
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = UINT(new_length);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srv_desc.BufferEx.NumElements = UINT(new_length >> 2);
  srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.NumElements = UINT(new_length >> 2);
  uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(device->CreateBuffer(&desc, nullptr, &new_buffer.buffer)) ||
      FAILED(device->CreateShaderResourceView(new_buffer.buffer.Get(), &srv_desc,
                                              &new_buffer.srv)) ||
      FAILED(device->CreateUnorderedAccessView(new_buffer.buffer.Get(), &uav_desc,
                                               &new_buffer.uav))) {
    if (!scaled_resolve_buffer_failure_logged_) {
      scaled_resolve_buffer_failure_logged_ = true;
      REXGPU_ERROR(
          "D3D11TextureCache: Failed to create a {} MB scaled resolve buffer ({} MB in use); the "
          "resolves and textures using it are skipped",
          new_length >> 20, scaled_resolve_buffers_total_size_ >> 20);
    }
    return false;
  }

  // Zero where nothing was resolved yet, like the committed memory of D3D12,
  // then the old buffers' data.
  ID3D11DeviceContext* context = GetContext();
  const UINT zero[4] = {};
  context->ClearUnorderedAccessViewUint(new_buffer.uav.Get(), zero);
  for (auto old = overlap_first; old != overlap_end; ++old) {
    context->CopySubresourceRegion(new_buffer.buffer.Get(), 0, UINT(old->first - new_start), 0, 0,
                                   old->second.buffer.Get(), 0, nullptr);
    scaled_resolve_buffers_total_size_ -= old->second.length_scaled;
    if (scaled_resolve_current_buffer_ == &old->second) {
      scaled_resolve_current_buffer_ = nullptr;
    }
  }
  scaled_resolve_buffers_.erase(overlap_first, overlap_end);
  scaled_resolve_buffers_total_size_ += new_length;
  scaled_resolve_buffers_.emplace(new_start, std::move(new_buffer));
  return true;
}

bool D3D11TextureCache::MakeScaledResolveRangeCurrent(uint32_t start_unscaled,
                                                      uint32_t length_unscaled,
                                                      uint32_t length_scaled_alignment_log2) {
  scaled_resolve_current_buffer_ = nullptr;
  scaled_resolve_current_offset_ = 0;
  if (!length_unscaled ||
      !EnsureScaledResolveMemoryCommitted(start_unscaled, length_unscaled,
                                          length_scaled_alignment_log2)) {
    return false;
  }
  uint64_t start_scaled =
      uint64_t(start_unscaled) * (draw_resolution_scale_x() * draw_resolution_scale_y());
  auto it = scaled_resolve_buffers_.upper_bound(start_scaled);
  assert_true(it != scaled_resolve_buffers_.begin());
  --it;
  scaled_resolve_current_buffer_ = &it->second;
  scaled_resolve_current_offset_ = uint32_t(start_scaled - it->first);
  return true;
}

ID3D11UnorderedAccessView* D3D11TextureCache::GetCurrentScaledResolveRangeUAV() {
  assert_not_null(scaled_resolve_current_buffer_);
  ScaledResolveBuffer& buffer = *scaled_resolve_current_buffer_;
  // Raw views start at a dword.
  uint32_t offset = scaled_resolve_current_offset_ & ~uint32_t(3);
  if (!offset) {
    return buffer.uav.Get();
  }
  auto it = buffer.range_uavs.find(offset);
  if (it != buffer.range_uavs.end()) {
    return it->second.Get();
  }
  // Resolve destinations are few; don't keep views of ranges no longer used.
  if (buffer.range_uavs.size() >= 64) {
    buffer.range_uavs.clear();
  }
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.FirstElement = offset >> 2;
  uav_desc.Buffer.NumElements = UINT((buffer.length_scaled - offset) >> 2);
  uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
  if (FAILED(GetDevice()->CreateUnorderedAccessView(buffer.buffer.Get(), &uav_desc, &uav))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create a view of a scaled resolve range");
    return nullptr;
  }
  return buffer.range_uavs.emplace(offset, std::move(uav)).first->second.Get();
}

bool D3D11TextureCache::UpdateConstants(ID3D11Buffer* buffer, const void* data, size_t size) {
  ID3D11DeviceContext* context = GetContext();
  D3D11_MAPPED_SUBRESOURCE mapping;
  if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapping))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to map a constant buffer");
    return false;
  }
  std::memcpy(mapping.pData, data, size);
  context->Unmap(buffer, 0);
  return true;
}

// Follows D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl with host
// blocks of one texel (nothing is block-compressed on the host), then stores
// the buffer into the texture level by level instead of CopyTextureRegion.
bool D3D11TextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                              bool load_mips) {
  D3D11Texture& d3d11_texture = static_cast<D3D11Texture&>(texture);
  TextureKey texture_key = d3d11_texture.key();

  LoadShaderIndex load_shader = GetLoadShaderIndex(texture_key);
  if (load_shader == kLoadShaderIndexUnknown) {
    return false;
  }
  bool texture_resolution_scaled = texture_key.scaled_resolve;
  ID3D11ComputeShader* load_compute_shader = texture_resolution_scaled
                                                 ? load_shaders_scaled_[load_shader].Get()
                                                 : load_shaders_[load_shader].Get();
  if (!load_compute_shader) {
    return false;
  }
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(load_shader);
  const HostFormat& host_format = host_formats_[uint32_t(texture_key.format)];
  // Guest blocks stored as they are are uploaded into a texture with a texel
  // per block and copied from there.
  bool host_block_compressed = d3d11_texture.host_block_compressed();
  TextureUploadLayout upload_layout =
      host_block_compressed
          ? GetTextureUploadLayout(GetBlockCompressedFormat(texture_key.format).block_view)
          : GetUploadLayout(host_format);
  if (upload_layout.texel_bytes != load_shader_info.bytes_per_host_block) {
    assert_always();
    return false;
  }

  // Get the guest layout.
  const texture_util::TextureGuestLayout& guest_layout = d3d11_texture.guest_layout();
  xenos::DataDimension dimension = texture_key.dimension;
  bool is_3d = dimension == xenos::DataDimension::k3D;
  bool is_3d_tiling = is_3d || d3d11_texture.force_load_3d_tiling();
  uint32_t width = texture_key.GetWidth();
  uint32_t height = texture_key.GetHeight();
  uint32_t depth_or_array_size = texture_key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  xenos::TextureFormat guest_format = texture_key.format;
  const FormatInfo* guest_format_info = FormatInfo::Get(guest_format);
  uint32_t block_width = guest_format_info->block_width;
  uint32_t block_height = guest_format_info->block_height;
  uint32_t bytes_per_block = guest_format_info->bytes_per_block();
  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? texture_key.mip_max_level : 0;
  assert_true(level_first <= level_last);
  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);
  uint32_t texture_resolution_scale_x = texture_resolution_scaled ? draw_resolution_scale_x() : 1;
  uint32_t texture_resolution_scale_y = texture_resolution_scaled ? draw_resolution_scale_y() : 1;

  // The loop counter can mean two things depending on whether the packed mip
  // tail is stored as mip 0, because in this case, it would be ambiguous since
  // both the base and the mips would be on "level 0", but loaded using separate
  // layouts.
  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    // Loop iteration 0 - base packed mip tail.
    // Loop iteration 1 - mips packed mip tail.
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    // Loop iteration is the actual level being loaded.
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  // The host layout in the decode buffer: rows of host blocks (texels, or 4x4
  // blocks if block-compressed), padded to whole thread invocations along X.
  struct HostSliceLayout {
    uint32_t offset;
    uint32_t row_pitch;
    // In host blocks.
    uint32_t height;
    uint32_t depth;
    // One array layer.
    uint32_t size;
  };
  HostSliceLayout host_slice_layout_base = {};
  HostSliceLayout host_slice_layouts_mips[xenos::kTextureMaxMips] = {};
  uint32_t host_block_width = host_block_compressed ? block_width : 1;
  uint32_t host_block_height = host_block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread = uint32_t(1) << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_block_compressed) {
    // Decompressing guest blocks.
    host_x_blocks_per_thread *= block_width;
  }
  uint64_t decode_buffer_size = 0;
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;
    HostSliceLayout& level_host_slice_layout =
        is_base ? host_slice_layout_base : host_slice_layouts_mips[level];
    uint32_t level_width, level_height, level_depth;
    if (level == level_packed) {
      // Loading the packed tail for the base or the mips - load the whole tail
      // to copy regions out of it.
      const texture_util::TextureGuestLayout::Level& guest_layout_packed =
          is_base ? guest_layout.base : guest_layout.mips[level];
      level_width = guest_layout_packed.x_extent_blocks * block_width;
      level_height = guest_layout_packed.y_extent_blocks * block_height;
      level_depth = guest_layout_packed.z_extent;
    } else {
      level_width = std::max(width >> level, uint32_t(1));
      level_height = std::max(height >> level, uint32_t(1));
      level_depth = std::max(depth >> level, uint32_t(1));
    }
    uint32_t level_width_blocks =
        (level_width * texture_resolution_scale_x + (host_block_width - 1)) / host_block_width;
    uint32_t level_height_blocks =
        (level_height * texture_resolution_scale_y + (host_block_height - 1)) / host_block_height;
    // Planes of height_texels rows, as the decompressing load shaders address
    // them (and size_blocks[1] for the others, the same with 1x1 blocks, and
    // the height in blocks when not decompressing).
    level_host_slice_layout.offset = uint32_t(decode_buffer_size);
    level_host_slice_layout.row_pitch =
        rex::align(rex::round_up(level_width_blocks, host_x_blocks_per_thread) *
                       load_shader_info.bytes_per_host_block,
                   uint32_t(256));
    level_host_slice_layout.height = level_height_blocks;
    level_host_slice_layout.depth = level_depth;
    uint64_t level_host_slice_size =
        rex::align(uint64_t(level_host_slice_layout.row_pitch) * level_height_blocks * level_depth,
                   uint64_t(512));
    decode_buffer_size += level_host_slice_size * array_size;
    if (decode_buffer_size > UINT32_MAX) {
      REXGPU_ERROR("D3D11TextureCache: A texture is too large to decode ({} bytes)",
                   decode_buffer_size);
      return false;
    }
    level_host_slice_layout.size = uint32_t(level_host_slice_size);
  }
  if (!EnsureDecodeBuffer(uint32_t(decode_buffer_size))) {
    return false;
  }

  ID3D11DeviceContext* context = GetContext();
  command_processor_.InvalidateDrawState();
  D3D11CommandProcessor::ScopedGpuSection gpu_section(command_processor_,
                                                      D3D11CommandProcessor::GpuSection::kTextureLoads);
  // The shared memory is read as an SRV here, which Direct3D 11 refuses while
  // it's bound as an output (memexport); the draw path binds it again.
  ID3D11UnorderedAccessView* null_uav = nullptr;
  context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
                                                     nullptr, nullptr,
                                                     xenos::kMaxColorRenderTargets, 1, &null_uav,
                                                     nullptr);

  // Decode into the buffer.
  context->CSSetShader(load_compute_shader, nullptr, 0);
  ID3D11Buffer* load_constants = load_constants_.Get();
  context->CSSetConstantBuffers(0, 1, &load_constants);
  ID3D11UnorderedAccessView* decode_uav = decode_buffer_uav_.Get();
  context->CSSetUnorderedAccessViews(0, 1, &decode_uav, nullptr);
  // The unscaled source; scaled, the base and the mips may be in different
  // scaled resolve buffers.
  if (!texture_resolution_scaled) {
    ID3D11ShaderResourceView* shared_memory_srv = d3d11_shared_memory_.GetRawSRV();
    context->CSSetShaderResources(0, 1, &shared_memory_srv);
  }

  LoadConstants load_constants_data = {};
  // 3 bits for each.
  assert_true(texture_resolution_scale_x <= 7);
  assert_true(texture_resolution_scale_y <= 7);
  load_constants_data.is_tiled_3d_endian_scale =
      uint32_t(texture_key.tiled) | (uint32_t(is_3d_tiling) << 1) |
      (uint32_t(texture_key.endianness) << 2) | (texture_resolution_scale_x << 4) |
      (texture_resolution_scale_y << 7);
  uint32_t guest_x_blocks_per_group_log2 = load_shader_info.GetGuestXBlocksPerGroupLog2();
  bool succeeded = true;
  bool scaled_mips_source_set_up = false;
  uint32_t scaled_mips_source_offset = 0;
  for (uint32_t loop_level = loop_level_first; succeeded && loop_level <= loop_level_last;
       ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    uint32_t guest_address = (is_base ? texture_key.base_page : texture_key.mip_page) << 12;
    if (texture_resolution_scaled) {
      // Bind the buffer holding the base or the mips, at the offset of the
      // range in it.
      if (is_base || !scaled_mips_source_set_up) {
        uint32_t guest_size_unscaled =
            is_base ? d3d11_texture.GetGuestBaseSize() : d3d11_texture.GetGuestMipsSize();
        if (!MakeScaledResolveRangeCurrent(guest_address, guest_size_unscaled, 4)) {
          succeeded = false;
          break;
        }
        ID3D11ShaderResourceView* scaled_resolve_srv = GetCurrentScaledResolveBufferSRV();
        context->CSSetShaderResources(0, 1, &scaled_resolve_srv);
        if (!is_base) {
          scaled_mips_source_set_up = true;
          scaled_mips_source_offset = GetCurrentScaledResolveRangeOffset();
        }
      }
      load_constants_data.guest_offset =
          is_base ? GetCurrentScaledResolveRangeOffset() : scaled_mips_source_offset;
    } else {
      load_constants_data.guest_offset = guest_address;
    }
    if (!is_base) {
      load_constants_data.guest_offset += guest_layout.mip_offsets_bytes[level] *
                                          (texture_resolution_scale_x * texture_resolution_scale_y);
    }
    load_constants_data.guest_pitch_aligned = level_guest_layout.row_pitch_bytes / bytes_per_block;
    load_constants_data.guest_z_stride_block_rows_aligned =
        level_guest_layout.z_slice_stride_block_rows;

    uint32_t level_width, level_height, level_depth;
    if (level == level_packed) {
      level_width = level_guest_layout.x_extent_blocks * block_width;
      level_height = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width = std::max(width >> level, uint32_t(1));
      level_height = std::max(height >> level, uint32_t(1));
      level_depth = std::max(depth >> level, uint32_t(1));
    }
    load_constants_data.size_blocks[0] =
        (level_width + (block_width - 1)) / block_width * texture_resolution_scale_x;
    load_constants_data.size_blocks[1] =
        (level_height + (block_height - 1)) / block_height * texture_resolution_scale_y;
    load_constants_data.size_blocks[2] = level_depth;
    load_constants_data.height_texels = level_height;

    uint32_t group_count_x =
        (load_constants_data.size_blocks[0] + ((uint32_t(1) << guest_x_blocks_per_group_log2) - 1)) >>
        guest_x_blocks_per_group_log2;
    uint32_t group_count_y =
        (load_constants_data.size_blocks[1] + ((uint32_t(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
        kLoadGuestYBlocksPerGroupLog2;

    const HostSliceLayout& level_host_slice_layout =
        is_base ? host_slice_layout_base : host_slice_layouts_mips[level];
    load_constants_data.host_offset = level_host_slice_layout.offset;
    load_constants_data.host_pitch = level_host_slice_layout.row_pitch;
    uint32_t level_array_slice_stride_bytes_scaled =
        level_guest_layout.array_slice_stride_bytes *
        (texture_resolution_scale_x * texture_resolution_scale_y);
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      if (!UpdateConstants(load_constants_.Get(), &load_constants_data,
                           sizeof(load_constants_data))) {
        succeeded = false;
        break;
      }
      context->Dispatch(group_count_x, group_count_y, load_constants_data.size_blocks[2]);
      load_constants_data.guest_offset += level_array_slice_stride_bytes_scaled;
      load_constants_data.host_offset += level_host_slice_layout.size;
    }
  }

  // Store the decoded levels into the texture (or for a block-compressed
  // texture, each layer of each level into the texture of blocks, then copy
  // it). The UAV first, so the decode buffer isn't bound as an output while
  // being bound as the source.
  // The 2D copy of a 3D texture (one slice) is stored as a 2D array.
  bool is_host_3d = d3d11_texture.is_host_3d();
  uint32_t host_level_count = texture_key.mip_max_level + 1;
  context->CSSetShader(is_host_3d ? upload_3d_shader_.Get() : upload_2d_array_shader_.Get(),
                       nullptr, 0);
  ID3D11Buffer* upload_constants = upload_constants_.Get();
  context->CSSetConstantBuffers(0, 1, &upload_constants);
  TextureUploadConstants upload_constants_data = {};
  upload_constants_data.texel_bytes = upload_layout.texel_bytes;
  std::memcpy(upload_constants_data.components, upload_layout.components,
              sizeof(upload_constants_data.components));
  for (uint32_t level = level_first; succeeded && level <= level_last; ++level) {
    uint32_t guest_level = std::min(level, level_packed);
    const HostSliceLayout& source_layout =
        level ? host_slice_layouts_mips[guest_level] : host_slice_layout_base;
    // The host level is the scaled base reduced, the decoded one the reduced
    // guest level scaled, which may be smaller (as in D3D12, the rest of the
    // host level isn't written).
    uint32_t level_host_width = std::max((width * texture_resolution_scale_x) >> level, uint32_t(1));
    uint32_t level_host_height =
        std::max((height * texture_resolution_scale_y) >> level, uint32_t(1));
    uint32_t level_offset_x = 0, level_offset_y = 0, level_offset_z = 0;
    if (level >= level_packed) {
      uint32_t level_offset_blocks_x, level_offset_blocks_y;
      texture_util::GetPackedMipOffset(width, height, depth, guest_format, level,
                                       level_offset_blocks_x, level_offset_blocks_y,
                                       level_offset_z);
      level_offset_x = level_offset_blocks_x * block_width * texture_resolution_scale_x;
      level_offset_y = level_offset_blocks_y * block_height * texture_resolution_scale_y;
    } else {
      level_host_width = std::min(
          level_host_width, std::max(width >> level, uint32_t(1)) * texture_resolution_scale_x);
      level_host_height = std::min(
          level_host_height, std::max(height >> level, uint32_t(1)) * texture_resolution_scale_y);
    }
    uint32_t plane_pitch = source_layout.row_pitch * source_layout.height;
    // Packed mip offsets are in whole guest blocks.
    upload_constants_data.offset = source_layout.offset +
                                   level_offset_x / host_block_width * upload_layout.texel_bytes +
                                   level_offset_y / host_block_height * source_layout.row_pitch +
                                   level_offset_z * plane_pitch;
    upload_constants_data.row_pitch = source_layout.row_pitch;
    upload_constants_data.slice_pitch = is_3d ? plane_pitch : source_layout.size;
    upload_constants_data.size[0] = (level_host_width + (host_block_width - 1)) / host_block_width;
    upload_constants_data.size[1] =
        (level_host_height + (host_block_height - 1)) / host_block_height;
    upload_constants_data.size[2] = is_3d ? std::max(depth >> level, uint32_t(1)) : array_size;

    if (!host_block_compressed) {
      ID3D11UnorderedAccessView* level_uav = d3d11_texture.GetOrCreateUploadUAV(level);
      if (!level_uav ||
          !UpdateConstants(upload_constants_.Get(), &upload_constants_data,
                           sizeof(upload_constants_data))) {
        succeeded = false;
        break;
      }
      context->CSSetUnorderedAccessViews(0, 1, &level_uav, nullptr);
      ID3D11ShaderResourceView* decode_srv = decode_buffer_srv_.Get();
      context->CSSetShaderResources(0, 1, &decode_srv);
      context->Dispatch(
          (upload_constants_data.size[0] + (kTextureUploadGroupSize - 1)) / kTextureUploadGroupSize,
          (upload_constants_data.size[1] + (kTextureUploadGroupSize - 1)) / kTextureUploadGroupSize,
          upload_constants_data.size[2]);
      continue;
    }

    // Block-compressed: a layer at a time through the texture of blocks. A
    // level smaller than a block takes the whole block (the copy covers the
    // level's physical size).
    ID3D11Texture2D* block_texture;
    ID3D11UnorderedAccessView* block_uav =
        GetBlockUploadTexture(upload_layout.texel_bytes, upload_constants_data.size[0],
                              upload_constants_data.size[1], block_texture);
    if (!block_uav) {
      succeeded = false;
      break;
    }
    uint32_t layer_count = upload_constants_data.size[2];
    upload_constants_data.size[2] = 1;
    D3D11_BOX block_box = {0, 0, 0, upload_constants_data.size[0], upload_constants_data.size[1],
                           1};
    for (uint32_t layer = 0; layer < layer_count; ++layer) {
      if (!UpdateConstants(upload_constants_.Get(), &upload_constants_data,
                           sizeof(upload_constants_data))) {
        succeeded = false;
        break;
      }
      context->CSSetUnorderedAccessViews(0, 1, &block_uav, nullptr);
      ID3D11ShaderResourceView* decode_srv = decode_buffer_srv_.Get();
      context->CSSetShaderResources(0, 1, &decode_srv);
      context->Dispatch(
          (upload_constants_data.size[0] + (kTextureUploadGroupSize - 1)) / kTextureUploadGroupSize,
          (upload_constants_data.size[1] + (kTextureUploadGroupSize - 1)) / kTextureUploadGroupSize,
          1);
      context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
      context->CopySubresourceRegion(d3d11_texture.resource(),
                                     D3D11CalcSubresource(level, layer, host_level_count), 0, 0, 0,
                                     block_texture, 0, &block_box);
      upload_constants_data.offset += upload_constants_data.slice_pitch;
    }
  }

  ID3D11ShaderResourceView* null_srv = nullptr;
  context->CSSetShaderResources(0, 1, &null_srv);
  context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
  ID3D11Buffer* null_buffer = nullptr;
  context->CSSetConstantBuffers(0, 1, &null_buffer);
  context->CSSetShader(nullptr, nullptr, 0);

  d3d11_texture.MarkAsUsed();
  if (succeeded) {
    // The guest memory's red and blue order.
    d3d11_texture.set_red_blue_swapped(false);
  }
  ++(succeeded ? stats_.loads : stats_.load_failures);
  return succeeded;
}

}  // namespace rex::graphics::d3d11
