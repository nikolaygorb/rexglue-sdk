/**
 * @file        graphics/texture_layout_test.cpp
 * @brief       Tests for Xenos guest texture memory layout
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

using namespace rex::graphics;

// xenia-canary 947075f88: a 1D texture wider than 8192 is stored as 8192-texel rows, and its
// layout keeps the row count instead of forcing a single row.
TEST_CASE("Wide 1D texture layout keeps its rows", "[graphics][texture]") {
  // 20000 texels of k_8_8_8_8 -> 3 rows of 8192, pitch 8192 / 32.
  texture_util::TextureGuestLayout layout =
      texture_util::GetGuestTextureLayout(xenos::DataDimension::k1D, 8192 >> 5, 8192, 3, 1, false,
                                          xenos::TextureFormat::k_8_8_8_8, false, true, 0);
  CHECK(layout.base.row_pitch_bytes == 8192 * 4);
  CHECK(layout.base.y_extent_blocks == 3);
  CHECK(layout.base.level_data_extent_bytes == 2 * 8192 * 4 + 8192 * 4);
}

TEST_CASE("Normal 1D texture layout has one row", "[graphics][texture]") {
  texture_util::TextureGuestLayout layout =
      texture_util::GetGuestTextureLayout(xenos::DataDimension::k1D, 256 >> 5, 256, 1, 1, false,
                                          xenos::TextureFormat::k_8_8_8_8, false, true, 0);
  CHECK(layout.base.y_extent_blocks == 1);
  CHECK(layout.base.level_data_extent_bytes == 256 * 4);
}

namespace {

// A 256x256 2D texture with mips 0...8 and the given addresses (in 4 KB pages).
xenos::xe_gpu_texture_fetch_t Make2DFetch(uint32_t base_page, uint32_t mip_page,
                                          uint32_t mip_min_level) {
  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.size_2d.width = 256 - 1;
  fetch.size_2d.height = 256 - 1;
  fetch.base_address = base_page;
  fetch.mip_address = mip_page;
  fetch.mip_min_level = mip_min_level;
  fetch.mip_max_level = 8;
  return fetch;
}

}  // namespace

// xenia-canary 4a863a0e1: a nonzero minimum mip level is sampler state and doesn't mean the base
// is missing - the base stays when it has its own page.
TEST_CASE("Separate base page is kept with a nonzero minimum mip level", "[graphics][texture]") {
  uint32_t base_page, mip_page, mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(Make2DFetch(0x100, 0x200, 2), nullptr, nullptr,
                                                 nullptr, &base_page, &mip_page, &mip_min_level,
                                                 &mip_max_level);
  CHECK(base_page == 0x100);
  CHECK(mip_page == 0x200);
  CHECK(mip_min_level == 2);
  CHECK(mip_max_level == 8);
}

TEST_CASE("Base sharing the mip page is dropped with a nonzero minimum mip level",
          "[graphics][texture]") {
  uint32_t base_page, mip_page, mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(Make2DFetch(0x100, 0x100, 2), nullptr, nullptr,
                                                 nullptr, &base_page, &mip_page, &mip_min_level,
                                                 &mip_max_level);
  CHECK(base_page == 0);
  CHECK(mip_page == 0x100);
  CHECK(mip_min_level == 2);
}

TEST_CASE("Missing base clamps the minimum mip level to 1", "[graphics][texture]") {
  uint32_t base_page, mip_page, mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(Make2DFetch(0, 0x200, 0), nullptr, nullptr,
                                                 nullptr, &base_page, &mip_page, &mip_min_level,
                                                 &mip_max_level);
  CHECK(base_page == 0);
  CHECK(mip_min_level == 1);
}

// xenia-canary bc6b7a020: non-base 2D array levels are aligned to four slices.
TEST_CASE("2D array mips are spaced by four slices", "[graphics][texture]") {
  // 64x64x3 tiled k_8_8_8_8: mip 1 is 32x32 -> one 4 KB tile per slice.
  texture_util::TextureGuestLayout layout =
      texture_util::GetGuestTextureLayout(xenos::DataDimension::k2DOrStacked, 64 >> 5, 64, 64, 3,
                                          true, xenos::TextureFormat::k_8_8_8_8, false, true, 2);
  REQUIRE(layout.mips[1].array_slice_stride_bytes == 4096);
  CHECK(layout.mip_offsets_bytes[2] == 4 * 4096);
}

// xenia-canary bc6b7a020: volume depth is reduced at each mip level.
TEST_CASE("3D mip slice stride uses the reduced depth", "[graphics][texture]") {
  // 32x32x16 tiled k_8_8_8_8: mip 1 is 16x16x8 -> 32x32 block tile rows, 8 slices.
  texture_util::TextureGuestLayout layout =
      texture_util::GetGuestTextureLayout(xenos::DataDimension::k3D, 32 >> 5, 32, 32, 16, true,
                                          xenos::TextureFormat::k_8_8_8_8, false, true, 2);
  CHECK(layout.mips[1].array_slice_stride_bytes == 4096 * 8);
}

// xenia-canary 19da17403: linear mip row blocks are aligned to max(256 / block size, 32), not
// 256 bytes - for 96bpp, 32 blocks are 384 bytes.
TEST_CASE("Linear 96bpp mip row pitch is 32 blocks", "[graphics][texture]") {
  texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, 64 >> 5, 64, 64, 1, false,
      xenos::TextureFormat::k_32_32_32_FLOAT, false, true, 1);
  CHECK(layout.mips[1].row_pitch_bytes == 32 * 12);
}

TEST_CASE("Linear 8bpp mip row pitch is 256 bytes", "[graphics][texture]") {
  texture_util::TextureGuestLayout layout =
      texture_util::GetGuestTextureLayout(xenos::DataDimension::k2DOrStacked, 256 >> 5, 64, 64, 1,
                                          false, xenos::TextureFormat::k_8, false, true, 1);
  CHECK(layout.mips[1].row_pitch_bytes == 256);
}

// xenia-canary c9c8e483b. Expected values are the exact extents of the boxes, found by walking
// every texel through texture_address::Tiled3D.
TEST_CASE("Tiled 3D lower bound starts at Y=8 in odd Z/4 groups", "[graphics][texture]") {
  // Box (0, 0, 4)-(32, 32, 8), pitch 32, height 32, 32bpp: the first stored byte is at 0x4000.
  CHECK(texture_util::GetTiledAddressLowerBound3D(0, 0, 4, 32, 32, 2) == 0x4000);
  // Box (0, 0, 4)-(32, 32, 8), pitch 64, height 32, 8bpp: the first stored byte is at 0x2000.
  CHECK(texture_util::GetTiledAddressLowerBound3D(0, 0, 4, 64, 32, 0) == 0x2000);
}

TEST_CASE("Tiled 3D 8bpp upper bound covers the last bank in the second X half",
          "[graphics][texture]") {
  // Box (0, 0, 0)-(64, 32, 4), pitch 96, height 32, 8bpp: the last stored byte ends at 0x2C00.
  CHECK(texture_util::GetTiledAddressUpperBound3D(64, 32, 4, 96, 32, 0) == 0x2C00);
}

// xenia-canary ace153cb8: when level 0 is already the packed mip tail, D3D leaves mip_address at
// 0 and the remaining levels are at the base address.
TEST_CASE("Level 0 packed tail uses the base address for mips", "[graphics][texture]") {
  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.size_2d.width = 16 - 1;
  fetch.size_2d.height = 16 - 1;
  fetch.tiled = 1;
  fetch.packed_mips = 1;
  fetch.base_address = 0x100;
  fetch.mip_max_level = 4;
  uint32_t base_page, mip_page, mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &base_page,
                                                 &mip_page, &mip_min_level, &mip_max_level);
  CHECK(base_page == 0x100);
  CHECK(mip_page == 0x100);
  CHECK(mip_min_level == 0);
  CHECK(mip_max_level == 4);
}

// xenia-canary 81e3deaee: a level 0 packed tail uses power-of-two rows for the slice stride.
TEST_CASE("Level 0 packed tail slice stride uses power-of-two rows", "[graphics][texture]") {
  // 16x72x2 tiled k_8_8_8_8 with packed mips: 128 rows (not 96) of 32 blocks * 4 bytes.
  texture_util::TextureGuestLayout layout =
      texture_util::GetGuestTextureLayout(xenos::DataDimension::k2DOrStacked, 32 >> 5, 16, 72, 2,
                                          true, xenos::TextureFormat::k_8_8_8_8, true, true, 0);
  REQUIRE(layout.packed_level == 0);
  CHECK(layout.base.array_slice_stride_bytes == 128 * 128);
}

// xenia-canary c332733af: the upper bound is the exact end of the last stored block, not a
// closed-form estimate that could pull in an extra page.
TEST_CASE("Tiled 3D upper bound is exact for a partial tile", "[graphics][texture]") {
  // Box (0, 0, 0)-(16, 16, 2), pitch 32, height 32, 32bpp: the last stored byte ends at 0x1000.
  CHECK(texture_util::GetTiledAddressUpperBound3D(16, 16, 2, 32, 32, 2) == 0x1000);
}

// xenia-canary d11950528: FormatInfo carries the stored component widths of fixed-point formats
// for integer num_format scaling.
TEST_CASE("Format info has fixed-point component widths", "[graphics][texture]") {
  const FormatInfo& f565 = *FormatInfo::Get(xenos::TextureFormat::k_5_6_5);
  CHECK(f565.fixed);
  CHECK(f565.component_bits[0] == 5);
  CHECK(f565.component_bits[1] == 6);
  CHECK(f565.component_bits[2] == 5);
  CHECK(f565.component_bits[3] == 0);
  const FormatInfo& f2101010 = *FormatInfo::Get(xenos::TextureFormat::k_2_10_10_10);
  CHECK(f2101010.fixed);
  CHECK(f2101010.component_bits[3] == 2);
  const FormatInfo& f111110 = *FormatInfo::Get(xenos::TextureFormat::k_11_11_10);
  CHECK(f111110.component_bits[0] == 10);
  CHECK(f111110.component_bits[2] == 11);
  CHECK_FALSE(FormatInfo::Get(xenos::TextureFormat::k_16_FLOAT)->fixed);
  CHECK_FALSE(FormatInfo::Get(xenos::TextureFormat::k_DXT1)->fixed);
  CHECK(FormatInfo::Get(xenos::TextureFormat::k_DXN)->fixed);
}
