/**
 * @file        graphics/native/edram_tiles.h
 * @brief       Native renderer: EDRAM tiles of a surface, uniform tiles, raw cell values
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              EdramLayout .. UnpackColorRaw), Copyright (c) 2026 OpenPortroid, MIT License
 *              (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/native/scale.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::native {

// The native renderer has no EDRAM memory. Each surface (EDRAM base, pitch,
// format, MSAA) is a host render target, and the EDRAM is bookkept in tiles of
// 80x16 32-bit samples (40x16 for 64bpp) so that a surface aliasing another one
// (D3D clearing colour through a depth surface, passes reusing memory with
// another format or pitch) gets the other's contents where it overlaps. All
// coordinates here are guest pixels; ScaleRect turns them into host pixels.
struct EdramSurface {
  // RB_COLOR_INFO::color_base / RB_DEPTH_INFO::depth_base.
  uint32_t base_tiles = 0;
  // RB_SURFACE_INFO::surface_pitch.
  uint32_t pitch_pixels = 0;
  xenos::MsaaSamples msaa = xenos::MsaaSamples::k1X;
  bool is_64bpp = false;
  // The pixel extent of the host render target (tiles past it are not part of
  // the surface).
  uint32_t width = 0;
  uint32_t height = 0;
};

// Width of a surface row in 32bpp tiles (xenos::GetSurfacePitchTiles).
uint32_t PitchTiles(const EdramSurface& surface);
// Pixels of one tile along X (80 samples, halved for 4x MSAA and for 64bpp)
// and along Y (16 samples, halved for 2x and 4x MSAA).
uint32_t TileWidthPixels(const EdramSurface& surface);
uint32_t TileHeightPixels(const EdramSurface& surface);

// Tile index (0 .. tile_count - 1, the EDRAM wraps) of row / column of the
// surface.
uint32_t TileAt(const EdramSurface& surface, uint32_t row, uint32_t column,
                uint32_t tile_count);

// Rows and columns of the tiles a pixel rectangle touches, clipped to the
// surface; false when nothing is left.
struct TileSpan {
  uint32_t row_first = 0;
  uint32_t row_last = 0;  // inclusive
  uint32_t column_first = 0;
  uint32_t column_last = 0;  // inclusive
};
bool GetTileSpan(const EdramSurface& surface, const Rect& rect, TileSpan& span_out);

// Calls fn(tile) for each tile the rectangle touches.
template <typename F>
void ForEachTile(const EdramSurface& surface, const Rect& rect, uint32_t tile_count, F&& fn) {
  TileSpan span;
  if (!GetTileSpan(surface, rect, span)) {
    return;
  }
  // TileAt for each, without its pitch and modulo for every tile (the claims
  // of each draw walk whole surfaces).
  const uint32_t pitch_tiles = PitchTiles(surface);
  for (uint32_t row = span.row_first; row <= span.row_last; ++row) {
    uint32_t tile = (surface.base_tiles + row * pitch_tiles + span.column_first) % tile_count;
    for (uint32_t column = span.column_first; column <= span.column_last; ++column) {
      fn(tile);
      if (++tile == tile_count) {
        tile = 0;
      }
    }
  }
}

// Pixel rectangle of row / column (not clipped to the surface). Not an overload
// of TileRect: both would take (surface, uint32_t, uint32_t).
Rect TileRectAt(const EdramSurface& surface, uint32_t row, uint32_t column);
// Pixel rectangle of a tile index of the surface: the position of its first
// occurrence from the base (a surface is never larger than the EDRAM).
Rect TileRect(const EdramSurface& surface, uint32_t tile, uint32_t tile_count);

// Tiles of the rectangle that it covers completely, with each tile clipped to
// the surface first (a tile sticking out of the surface counts as covered when
// its part inside is). Row-major order.
void TilesInside(const EdramSurface& surface, const Rect& rect, uint32_t tile_count,
                 std::vector<uint32_t>& tiles_out);

// Pixel rectangles covering `tiles` (row-major, as TilesInside gives them)
// within `clip`: neighbours in a row are merged, then rows of the same extent.
// Guest pixels: scale them for a host render target.
void TileRects(const EdramSurface& surface, const std::vector<uint32_t>& tiles, const Rect& clip,
               uint32_t tile_count, std::vector<Rect>& rects_out);

// Tiles known to hold one raw value after a clear, per component (0: the colour
// cell, or the depth bits of a depth cell with the stencil byte zero; 1: the
// stencil byte), so that a transfer from them can be a clear. One value per
// component at a time: setting another value forgets the old tiles.
class UniformTiles {
 public:
  void Reset(uint32_t tile_count);

  void Set(uint32_t component, const std::vector<uint32_t>& tiles, uint32_t value);
  void Forget(uint32_t component, uint32_t tile);
  void Forget(uint32_t component, const EdramSurface& surface, const Rect& rect,
              uint32_t tile_count);

  bool Has(uint32_t component, uint32_t tile) const;
  bool Any(uint32_t component) const { return count_[component] != 0; }
  uint32_t value(uint32_t component) const { return value_[component]; }

 private:
  std::vector<uint64_t> bits_[2];
  uint32_t value_[2] = {};
  uint32_t count_[2] = {};
};

// Colour formats whose clears are carried through transfers as raw values:
// 8_8_8_8, 2_10_10_10 (+ AS_10_10_10_10), 2_10_10_10_FLOAT (+ AS_16_16_16_16),
// 32_FLOAT. Not the gamma format (its host values are not the EDRAM's).
bool IsUniformColorFormat(xenos::ColorRenderTargetFormat format);

// Float -> Xenos 7e3 (bits 0:9), clamped to [0, 31.875], rounded to the nearest
// even, as the shader translators' UnclampedFloat32To7e3.
uint32_t Float32To7e3(float f32);

// Host clear colour <-> the 32-bit EDRAM cell of a uniform colour format
// (unorm channels rounded to the nearest). For 32_FLOAT only [0] is used.
uint32_t PackColorRaw(const float color[4], xenos::ColorRenderTargetFormat format);
void UnpackColorRaw(uint32_t raw, xenos::ColorRenderTargetFormat format, float color_out[4]);

// The host clear colour of a resolve's colour clear (RB_COLOR_CLEAR, and
// RB_COLOR_CLEAR_LO for 64bpp formats) in the host render target's terms:
// 16-bit fixed formats stored /32 (snorm), float formats as floats, the gamma
// format as its raw bytes, the rest as UnpackColorRaw.
void DecodeColorClear(xenos::ColorRenderTargetFormat format, uint32_t rb_color_clear,
                      uint32_t rb_color_clear_lo, float color_out[4]);

// Height of a host render target standing for a surface: the rows from its
// base to the end of the EDRAM (no wrap), whole tiles, within these bounds.
constexpr uint32_t kMinSurfaceHeight = 8;
constexpr uint32_t kMaxSurfaceHeight = 2048;
uint32_t SurfaceHeightPixels(uint32_t base_tiles, uint32_t pitch_pixels, xenos::MsaaSamples msaa,
                             bool is_64bpp, uint32_t tile_count);

// Host depth 0..1 <-> the 24 depth bits of a depth cell (D24FS8: 20e4 rounded
// to the nearest even, of twice the host value: the host holds float24 depth
// at half its value, GetHostViewportInfo full_float24_in_0_to_1; D24S8:
// unorm24).
uint32_t EncodeDepth24(float depth, xenos::DepthRenderTargetFormat format);
float DecodeDepth24(uint32_t depth24, xenos::DepthRenderTargetFormat format);

}  // namespace rex::graphics::native
