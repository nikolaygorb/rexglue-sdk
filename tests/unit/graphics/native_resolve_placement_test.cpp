/**
 * @file        graphics/native_resolve_placement_test.cpp
 * @brief       Tests for where the native renderer finds a resolve within a texture
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/native/resolve_placement.h>
#include <rex/graphics/pipeline/texture/address.h>

using namespace rex::graphics;
using namespace rex::graphics::native;

TEST_CASE("Native resolve destination origin", "[graphics][native][resolve]") {
  // 32 bits per pixel: a macro tile is 4 KB, the base is already aligned.
  ResolveDestOrigin origin = GetResolveDestOrigin(0x1F964000, 2);
  CHECK(origin.texture_base == 0x1F964000);
  CHECK(origin.x_offset == 0);
  // 16 bits: 2 KB macro tiles, the second one's phase moved into x.
  origin = GetResolveDestOrigin(0x00100800, 1);
  CHECK(origin.texture_base == 0x00100000);
  CHECK(origin.x_offset == 32);
  // 8 bits: 1 KB macro tiles.
  origin = GetResolveDestOrigin(0x00100C00, 0);
  CHECK(origin.texture_base == 0x00100000);
  CHECK(origin.x_offset == 96);
}

TEST_CASE("Native resolve within a texture", "[graphics][native][resolve]") {
  const uint32_t texture_base = 0x00200000;
  const uint32_t pitch = 1280;
  const uint32_t height = 736;
  uint32_t x, y;

  // The same base: the origin.
  CHECK(FindMacroTileInTexture(texture_base, texture_base, pitch, height, 2, x, y));
  CHECK(x == 0);
  CHECK(y == 0);

  // Every macro tile back from its own address, at each texel size.
  for (uint32_t bpp_log2 = 0; bpp_log2 <= 3; ++bpp_log2) {
    for (uint32_t tile_y = 0; tile_y < height; tile_y += 32 * 7) {
      for (uint32_t tile_x = 0; tile_x < pitch; tile_x += 32 * 5) {
        uint32_t dest = texture_base + uint32_t(texture_address::Tiled2D(
                                           int32_t(tile_x), int32_t(tile_y), pitch, bpp_log2));
        REQUIRE(FindMacroTileInTexture(texture_base, dest, pitch, height, bpp_log2, x, y));
        CHECK(x == tile_x);
        CHECK(y == tile_y);
      }
    }
  }

  // Before the texture, inside a macro tile, past its end: none.
  CHECK_FALSE(FindMacroTileInTexture(texture_base, texture_base - 0x1000, pitch, height, 2, x, y));
  CHECK_FALSE(FindMacroTileInTexture(texture_base, texture_base + 0x40, pitch, height, 2, x, y));
  CHECK_FALSE(
      FindMacroTileInTexture(texture_base, texture_base + 0x10000000, pitch, height, 2, x, y));
}
