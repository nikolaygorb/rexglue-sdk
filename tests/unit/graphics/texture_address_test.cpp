/**
 * @file        graphics/texture_address_test.cpp
 * @brief       Tests for Xenos tiled texture addressing
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdint>
#include <initializer_list>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/texture/address.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

using namespace rex::graphics;

namespace {

// The tiling functions replaced by texture_address in xenia-canary 28b69c21b, kept as the
// reference: the new ones must produce the same addresses.
int32_t OldTiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

int32_t OldTiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                         uint32_t bytes_per_block_log2) {
  int32_t macro_outer = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  int32_t macro = ((((x >> 5) + macro_outer) << (bytes_per_block_log2 + 6)) & 0xFFFFFFF) << 1;
  int32_t micro = (((x & 7) + ((y & 6) << 2)) << (bytes_per_block_log2 + 6)) >> 6;
  int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  int32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                    ((z & 3) << (bytes_per_block_log2 + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

}  // namespace

// xenia-canary 28b69c21b: the documented bit layout gives the same 2D addresses.
TEST_CASE("Tiled 2D address matches the previous tiling", "[graphics][texture]") {
  for (uint32_t bpb_log2 = 0; bpb_log2 <= 4; ++bpb_log2) {
    for (uint32_t pitch : {32u, 64u, 96u, 160u}) {
      for (int32_t y = 0; y < 72; ++y) {
        for (int32_t x = 0; x < int32_t(pitch); ++x) {
          REQUIRE(texture_address::Tiled2D(x, y, pitch, bpb_log2) ==
                  OldTiledOffset2D(x, y, pitch, bpb_log2));
        }
      }
    }
  }
}

// xenia-canary 28b69c21b: the documented bit layout gives the same 3D addresses.
TEST_CASE("Tiled 3D address matches the previous tiling", "[graphics][texture]") {
  for (uint32_t bpb_log2 = 0; bpb_log2 <= 4; ++bpb_log2) {
    for (uint32_t pitch : {32u, 96u}) {
      for (uint32_t height : {32u, 64u, 96u}) {
        for (int32_t z = 0; z < 12; ++z) {
          for (int32_t y = 0; y < int32_t(height); ++y) {
            for (int32_t x = 0; x < int32_t(pitch); ++x) {
              REQUIRE(texture_address::Tiled3D(x, y, z, pitch, height, bpb_log2) ==
                      int64_t(OldTiledOffset3D(x, y, z, pitch, height, bpb_log2)));
            }
          }
        }
      }
    }
  }
}

// xenia-canary 28b69c21b: the extent of a tiled 3D mip uses the mip's slice stride in block rows
// (rounded from a power of two) as the height, not its rows rounded to 32.
TEST_CASE("Tiled 3D mip extent uses the slice stride", "[graphics][texture]") {
  // 130x130x16 tiled k_8_8_8_8, mip 1 is 65x65x8: 128 rows per slice, not 96.
  texture_util::TextureGuestLayout layout =
      texture_util::GetGuestTextureLayout(xenos::DataDimension::k3D, 160 >> 5, 130, 130, 16, true,
                                          xenos::TextureFormat::k_8_8_8_8, false, true, 1);
  const texture_util::TextureGuestLayout::Level& mip = layout.mips[1];
  REQUIRE(mip.z_slice_stride_block_rows == 128);
  REQUIRE(mip.row_pitch_bytes == 128 * 4);
  CHECK(mip.array_slice_data_extent_bytes ==
        uint32_t(texture_util::GetTiledAddressUpperBound3D(65, 65, 8, 128, 128, 2)));
  CHECK(mip.array_slice_data_extent_bytes >
        uint32_t(texture_util::GetTiledAddressUpperBound3D(65, 65, 8, 128, 96, 2)));
}

// Found by texture_layout_fuzzer: with a pitch below 32 blocks (0 in a garbage fetch constant),
// Z / 4 no longer moves the address forward, and the last, partial 4-slice portion didn't hold
// the highest address, so the extent the texture cache watches ended before the data.
TEST_CASE("Tiled 3D upper bound covers all slices with a zero pitch", "[graphics][texture]") {
  // 1x1x6 with 4-byte blocks: slices 4 and 5 are the last portion.
  int64_t end = 0;
  for (int32_t z = 0; z < 6; ++z) {
    end = std::max(end, texture_address::Tiled3D(0, 0, z, 0, 32, 2) + 4);
  }
  REQUIRE(end == 5124);
  CHECK(texture_util::GetTiledAddressUpperBound3D(1, 1, 6, 0, 32, 2) >= uint64_t(end));
  // A normal pitch keeps the last portion only.
  CHECK(texture_util::GetTiledAddressUpperBound3D(1, 1, 6, 32, 32, 2) ==
        uint64_t(texture_address::Tiled3D(0, 0, 5, 32, 32, 2) + 4));
}

// Found by texture_layout_fuzzer: the level sizes were 32-bit, so a garbage fetch constant for a
// huge texture wrapped around to a small range of guest memory to load and watch.
TEST_CASE("A texture larger than the guest memory has an empty layout", "[graphics][texture]") {
  // 8192x8192x64 stacked with 16-byte texels: 64 GB.
  texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, 8192 >> 5, 8192, 8192, 64, true,
      xenos::TextureFormat::k_32_32_32_32_FLOAT, false, true, 0);
  CHECK(layout.base.level_data_extent_bytes == 0);
  CHECK(layout.mips_total_extent_bytes == 0);
  // 2048x2048x1024 volume with 4-byte texels: 16 GB.
  layout = texture_util::GetGuestTextureLayout(xenos::DataDimension::k3D, 2048 >> 5, 2048, 2048,
                                               1024, false, xenos::TextureFormat::k_8_8_8_8,
                                               false, true, 0);
  CHECK(layout.base.level_data_extent_bytes == 0);
  // 2048x2048x16 is 256 MB and still loads.
  layout = texture_util::GetGuestTextureLayout(xenos::DataDimension::k2DOrStacked, 2048 >> 5,
                                               2048, 2048, 16, false,
                                               xenos::TextureFormat::k_8_8_8_8, false, true, 0);
  CHECK(layout.base.level_data_extent_bytes == 2048u * 2048u * 4u * 16u);
}
