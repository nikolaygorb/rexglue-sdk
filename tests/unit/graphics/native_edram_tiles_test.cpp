/**
 * @file        graphics/native_edram_tiles_test.cpp
 * @brief       Tests for the native renderer's EDRAM tile bookkeeping and raw cell values
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <bit>
#include <cmath>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/native/edram_tiles.h>
#include <rex/graphics/xenos.h>

using namespace rex::graphics;
using namespace rex::graphics::native;

namespace {

constexpr uint32_t kTiles = xenos::kEdramTileCount;

EdramSurface Surface(uint32_t base, uint32_t pitch, uint32_t width, uint32_t height,
                     xenos::MsaaSamples msaa = xenos::MsaaSamples::k1X, bool is_64bpp = false) {
  EdramSurface s;
  s.base_tiles = base;
  s.pitch_pixels = pitch;
  s.width = width;
  s.height = height;
  s.msaa = msaa;
  s.is_64bpp = is_64bpp;
  return s;
}

std::vector<uint32_t> Touched(const EdramSurface& s, const Rect& r, uint32_t tile_count = kTiles) {
  std::vector<uint32_t> tiles;
  ForEachTile(s, r, tile_count, [&](uint32_t tile) { tiles.push_back(tile); });
  return tiles;
}

}  // namespace

TEST_CASE("Native EDRAM tile sizes follow MSAA and 64bpp", "[graphics][native][edram]") {
  CHECK(PitchTiles(Surface(0, 1280, 1280, 720)) == 16);
  CHECK(PitchTiles(Surface(0, 1281, 1281, 720)) == 17);
  CHECK(PitchTiles(Surface(0, 1280, 1280, 720, xenos::MsaaSamples::k4X)) == 32);
  CHECK(PitchTiles(Surface(0, 1280, 1280, 720, xenos::MsaaSamples::k1X, true)) == 32);
  CHECK(TileWidthPixels(Surface(0, 1280, 1280, 720)) == 80);
  CHECK(TileWidthPixels(Surface(0, 1280, 1280, 720, xenos::MsaaSamples::k4X)) == 40);
  CHECK(TileWidthPixels(Surface(0, 1280, 1280, 720, xenos::MsaaSamples::k1X, true)) == 40);
  CHECK(TileWidthPixels(Surface(0, 1280, 1280, 720, xenos::MsaaSamples::k4X, true)) == 20);
  CHECK(TileHeightPixels(Surface(0, 1280, 1280, 720)) == 16);
  CHECK(TileHeightPixels(Surface(0, 1280, 1280, 720, xenos::MsaaSamples::k2X)) == 8);
  CHECK(TileHeightPixels(Surface(0, 1280, 1280, 720, xenos::MsaaSamples::k4X)) == 8);
}

TEST_CASE("Native ForEachTile covers the touched tiles and wraps", "[graphics][native][edram]") {
  const EdramSurface s = Surface(10, 1280, 1280, 720);
  CHECK(Touched(s, {0, 0, 80, 16}) == std::vector<uint32_t>{10});
  CHECK(Touched(s, {79, 15, 81, 17}) == std::vector<uint32_t>{10, 11, 26, 27});
  // Clipped to the surface, empty rectangles touch nothing.
  CHECK(Touched(s, {1270, 710, 2000, 2000}) == std::vector<uint32_t>{10 + 44 * 16 + 15});
  CHECK(Touched(s, {5, 5, 5, 9}).empty());
  CHECK(Touched(s, {1280, 0, 1300, 16}).empty());
  // The EDRAM wraps at the tile count.
  const EdramSurface wrapping = Surface(2040, 1280, 1280, 720);
  std::vector<uint32_t> expected;
  for (uint32_t i = 0; i < 16; ++i) {
    expected.push_back((2040 + i) % kTiles);
  }
  CHECK(Touched(wrapping, {0, 0, 1280, 16}) == expected);
  // The extended tile count does not wrap there.
  CHECK(Touched(wrapping, {0, 0, 160, 16}, xenos::kEdramTileCountMax) ==
        std::vector<uint32_t>{2040, 2041});
}

TEST_CASE("Native TileRect inverts TileAt", "[graphics][native][edram]") {
  for (uint32_t base : {0u, 100u, 2000u}) {
    const EdramSurface s = Surface(base, 1280, 1280, 720);
    for (uint32_t row = 0; row < 45; ++row) {
      for (uint32_t column = 0; column < 16; ++column) {
        REQUIRE(TileRect(s, TileAt(s, row, column, kTiles), kTiles) == TileRectAt(s, row, column));
      }
    }
  }
  CHECK(TileRectAt(Surface(0, 1280, 1280, 720), 1, 2) == Rect{160, 16, 240, 32});
  CHECK(TileRectAt(Surface(0, 640, 640, 360, xenos::MsaaSamples::k4X), 1, 2) ==
        Rect{80, 8, 120, 16});
}

TEST_CASE("Native TilesInside takes only fully covered tiles", "[graphics][native][edram]") {
  const EdramSurface s = Surface(0, 1280, 1280, 720);
  std::vector<uint32_t> tiles;
  TilesInside(s, {0, 0, 160, 32}, kTiles, tiles);
  CHECK(tiles == std::vector<uint32_t>{0, 1, 16, 17});
  TilesInside(s, {1, 0, 160, 32}, kTiles, tiles);
  CHECK(tiles == std::vector<uint32_t>{1, 17});
  TilesInside(s, {0, 0, 79, 16}, kTiles, tiles);
  CHECK(tiles.empty());
  // A tile sticking out of the surface counts when its part inside is covered.
  const EdramSurface narrow = Surface(0, 100, 100, 20);
  TilesInside(narrow, {0, 0, 100, 20}, kTiles, tiles);
  CHECK(tiles == std::vector<uint32_t>{0, 1, 2, 3});
}

TEST_CASE("Native TileRects merges rows and columns", "[graphics][native][edram]") {
  const EdramSurface s = Surface(0, 1280, 1280, 720);
  std::vector<Rect> rects;
  TileRects(s, {0, 1, 16, 17}, {0, 0, 1280, 720}, kTiles, rects);
  CHECK(rects == std::vector<Rect>{{0, 0, 160, 32}});
  TileRects(s, {0, 1, 16, 17}, {0, 0, 150, 30}, kTiles, rects);
  CHECK(rects == std::vector<Rect>{{0, 0, 150, 30}});
  // An L shape: two rectangles.
  TileRects(s, {0, 1, 16}, {0, 0, 1280, 720}, kTiles, rects);
  CHECK(rects == std::vector<Rect>{{0, 0, 160, 16}, {0, 16, 80, 32}});
  // Tiles outside the clip are dropped.
  TileRects(s, {0, 5}, {0, 0, 160, 16}, kTiles, rects);
  CHECK(rects == std::vector<Rect>{{0, 0, 80, 16}});
}

TEST_CASE("Native uniform tiles keep one value per component", "[graphics][native][edram]") {
  UniformTiles uniform;
  uniform.Reset(kTiles);
  CHECK_FALSE(uniform.Any(0));
  uniform.Set(0, {1, 2}, 5);
  CHECK(uniform.Has(0, 1));
  CHECK(uniform.Has(0, 2));
  CHECK_FALSE(uniform.Has(1, 1));
  CHECK(uniform.value(0) == 5);
  // Another value forgets the tiles of the old one.
  uniform.Set(0, {3}, 6);
  CHECK_FALSE(uniform.Has(0, 1));
  CHECK(uniform.Has(0, 3));
  uniform.Forget(0, 3);
  CHECK_FALSE(uniform.Any(0));
  // Out of range tiles are ignored.
  uniform.Set(1, {kTiles + 5}, 1);
  CHECK_FALSE(uniform.Any(1));
  // Forgetting by rectangle.
  const EdramSurface s = Surface(0, 1280, 1280, 720);
  uniform.Set(0, {0, 1, 16, 17}, 7);
  uniform.Forget(0, s, {0, 0, 80, 16}, kTiles);
  CHECK_FALSE(uniform.Has(0, 0));
  CHECK(uniform.Has(0, 1));
  CHECK(uniform.Has(0, 16));
  CHECK(uniform.Any(0));
}

TEST_CASE("Native Float32To7e3 matches Float7e3To32", "[graphics][native][edram]") {
  for (uint32_t code = 0; code < 1024; ++code) {
    REQUIRE(Float32To7e3(xenos::Float7e3To32(code)) == code);
  }
  CHECK(Float32To7e3(0.0f) == 0);
  CHECK(Float32To7e3(1.0f) == 0x180);
  CHECK(Float32To7e3(31.875f) == 0x3FF);
  CHECK(Float32To7e3(100.0f) == 0x3FF);
  CHECK(Float32To7e3(-1.0f) == 0);
  CHECK(Float32To7e3(std::numeric_limits<float>::quiet_NaN()) == 0);
}

TEST_CASE("Native raw colour cells round-trip", "[graphics][native][edram]") {
  const float c[4] = {1.0f, 0.0f, 0.5f, 1.0f};
  CHECK(PackColorRaw(c, xenos::ColorRenderTargetFormat::k_8_8_8_8) == 0xFF8000FFu);
  CHECK(PackColorRaw(c, xenos::ColorRenderTargetFormat::k_2_10_10_10) ==
        (1023u | (512u << 20) | (3u << 30)));
  CHECK(PackColorRaw(c, xenos::ColorRenderTargetFormat::k_32_FLOAT) == std::bit_cast<uint32_t>(1.0f));
  const xenos::ColorRenderTargetFormat formats[] = {
      xenos::ColorRenderTargetFormat::k_8_8_8_8,
      xenos::ColorRenderTargetFormat::k_2_10_10_10,
      xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10,
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT,
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16,
  };
  // A spread of cells, every channel value at least once in the low channel.
  for (xenos::ColorRenderTargetFormat format : formats) {
    REQUIRE(IsUniformColorFormat(format));
    for (uint32_t i = 0; i < 4096; ++i) {
      const uint32_t raw = (i * 0x9E3779B1u) ^ (i & 0x3FF);
      float unpacked[4];
      UnpackColorRaw(raw, format, unpacked);
      REQUIRE(PackColorRaw(unpacked, format) == raw);
    }
  }
  CHECK(IsUniformColorFormat(xenos::ColorRenderTargetFormat::k_32_FLOAT));
  CHECK_FALSE(IsUniformColorFormat(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA));
  CHECK_FALSE(IsUniformColorFormat(xenos::ColorRenderTargetFormat::k_16_16_16_16));
}

TEST_CASE("Native depth cells round-trip", "[graphics][native][edram]") {
  CHECK(EncodeDepth24(0.0f, xenos::DepthRenderTargetFormat::kD24S8) == 0);
  CHECK(EncodeDepth24(1.0f, xenos::DepthRenderTargetFormat::kD24S8) == 0xFFFFFF);
  CHECK(EncodeDepth24(2.0f, xenos::DepthRenderTargetFormat::kD24S8) == 0xFFFFFF);
  // Float24: the host value is half the guest's.
  const uint32_t one_20e4 = EncodeDepth24(0.5f, xenos::DepthRenderTargetFormat::kD24FS8);
  CHECK(one_20e4 == xenos::Float32To20e4(1.0f, true));
  CHECK(DecodeDepth24(one_20e4, xenos::DepthRenderTargetFormat::kD24FS8) == 0.5f);
  for (uint32_t code = 0; code <= one_20e4; code += 997) {
    REQUIRE(EncodeDepth24(DecodeDepth24(code, xenos::DepthRenderTargetFormat::kD24FS8),
                          xenos::DepthRenderTargetFormat::kD24FS8) == code);
  }
  // Unorm24 through float32 is not exact near 1 (two codes can share a float):
  // within one code.
  for (uint32_t code = 0; code <= 0xFFFFFF; code += 4099) {
    const uint32_t back = EncodeDepth24(DecodeDepth24(code, xenos::DepthRenderTargetFormat::kD24S8),
                                        xenos::DepthRenderTargetFormat::kD24S8);
    REQUIRE((back >= code ? back - code : code - back) <= 1);
  }
  CHECK(EncodeDepth24(DecodeDepth24(0xFFFFFF, xenos::DepthRenderTargetFormat::kD24S8),
                      xenos::DepthRenderTargetFormat::kD24S8) == 0xFFFFFF);
}

TEST_CASE("Native surface height runs to the end of the EDRAM", "[graphics][native][edram]") {
  using xenos::MsaaSamples;
  // 1280 pixels: 16 tiles a row, 128 rows of 16 pixels, capped at 2048.
  CHECK(SurfaceHeightPixels(0, 1280, MsaaSamples::k1X, false, kTiles) == 2048);
  CHECK(SurfaceHeightPixels(1024, 1280, MsaaSamples::k1X, false, kTiles) == 1024);
  // 4x MSAA: 32 tiles a row of 8 pixels; 64bpp: 32 tiles a row of 16.
  CHECK(SurfaceHeightPixels(0, 1280, MsaaSamples::k4X, false, kTiles) == 512);
  CHECK(SurfaceHeightPixels(0, 1280, MsaaSamples::k1X, true, kTiles) == 1024);
  // No whole row left: the minimum. The base wraps at the tile count.
  CHECK(SurfaceHeightPixels(2047, 1280, MsaaSamples::k1X, false, kTiles) == kMinSurfaceHeight);
  CHECK(SurfaceHeightPixels(kTiles + 1024, 1280, MsaaSamples::k1X, false, kTiles) == 1024);
  // 8192 tiles: taller surfaces, still capped.
  CHECK(SurfaceHeightPixels(0, 2560, MsaaSamples::k1X, false, 8192) == kMaxSurfaceHeight);
}

TEST_CASE("Native resolve colour clears per format", "[graphics][native][edram]") {
  using F = xenos::ColorRenderTargetFormat;
  float c[4];
  DecodeColorClear(F::k_8_8_8_8, 0xFF000080u, 0, c);
  CHECK(c[0] == 128.0f / 255.0f);
  CHECK(c[1] == 0.0f);
  CHECK(c[3] == 1.0f);
  // The gamma format: its raw bytes, as 8_8_8_8.
  DecodeColorClear(F::k_8_8_8_8_GAMMA, 0x00FF0000u, 0, c);
  CHECK(c[2] == 1.0f);
  // 16_16: codes over 32767 (the host stores the -32...32 range /32).
  DecodeColorClear(F::k_16_16, 0x80017FFFu, 0, c);
  CHECK(c[0] == 1.0f);
  CHECK(c[1] == -1.0f);
  DecodeColorClear(F::k_16_16, 0x80000000u, 0, c);
  CHECK(c[1] == -1.0f);
  // 64bpp: R | G << 16 in the low dword, B | A << 16 in the high one.
  DecodeColorClear(F::k_16_16_16_16_FLOAT, 0x00003800u, 0x40003C00u, c);
  CHECK(c[0] == 1.0f);
  CHECK(c[1] == 2.0f);
  CHECK(c[2] == 0.5f);
  CHECK(c[3] == 0.0f);
  DecodeColorClear(F::k_32_32_FLOAT, std::bit_cast<uint32_t>(-2.0f), std::bit_cast<uint32_t>(1.5f),
                   c);
  CHECK(c[0] == 1.5f);
  CHECK(c[1] == -2.0f);
  // 32bpp formats take only RB_COLOR_CLEAR.
  DecodeColorClear(F::k_32_FLOAT, std::bit_cast<uint32_t>(0.25f), 0xFFFFFFFFu, c);
  CHECK(c[0] == 0.25f);
}
