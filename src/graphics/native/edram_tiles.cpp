/**
 * @file        graphics/native/edram_tiles.cpp
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

#include <rex/graphics/native/edram_tiles.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include <rex/math.h>

namespace rex::graphics::native {

uint32_t PitchTiles(const EdramSurface& surface) {
  return xenos::GetSurfacePitchTiles(surface.pitch_pixels, surface.msaa, surface.is_64bpp);
}

uint32_t TileWidthPixels(const EdramSurface& surface) {
  return xenos::kEdramTileWidthSamples >>
         (uint32_t(surface.msaa >= xenos::MsaaSamples::k4X) + uint32_t(surface.is_64bpp));
}

uint32_t TileHeightPixels(const EdramSurface& surface) {
  return xenos::kEdramTileHeightSamples >> uint32_t(surface.msaa >= xenos::MsaaSamples::k2X);
}

uint32_t TileAt(const EdramSurface& surface, uint32_t row, uint32_t column,
                uint32_t tile_count) {
  return (surface.base_tiles + row * PitchTiles(surface) + column) % tile_count;
}

bool GetTileSpan(const EdramSurface& surface, const Rect& rect, TileSpan& span_out) {
  const int32_t left = std::max(rect.left, int32_t(0));
  const int32_t top = std::max(rect.top, int32_t(0));
  const int32_t right = std::min(rect.right, int32_t(surface.width));
  const int32_t bottom = std::min(rect.bottom, int32_t(surface.height));
  if (left >= right || top >= bottom) {
    return false;
  }
  const uint32_t tile_width = TileWidthPixels(surface);
  const uint32_t tile_height = TileHeightPixels(surface);
  span_out.column_first = uint32_t(left) / tile_width;
  span_out.column_last = (uint32_t(right) - 1) / tile_width;
  span_out.row_first = uint32_t(top) / tile_height;
  span_out.row_last = (uint32_t(bottom) - 1) / tile_height;
  return true;
}

Rect TileRectAt(const EdramSurface& surface, uint32_t row, uint32_t column) {
  const int32_t tile_width = int32_t(TileWidthPixels(surface));
  const int32_t tile_height = int32_t(TileHeightPixels(surface));
  return {int32_t(column) * tile_width, int32_t(row) * tile_height,
          int32_t(column + 1) * tile_width, int32_t(row + 1) * tile_height};
}

Rect TileRect(const EdramSurface& surface, uint32_t tile, uint32_t tile_count) {
  const uint32_t pitch_tiles = PitchTiles(surface);
  const uint32_t relative = (tile + tile_count - surface.base_tiles % tile_count) % tile_count;
  return TileRectAt(surface, relative / pitch_tiles, relative % pitch_tiles);
}

void TilesInside(const EdramSurface& surface, const Rect& rect, uint32_t tile_count,
                 std::vector<uint32_t>& tiles_out) {
  tiles_out.clear();
  TileSpan span;
  if (!GetTileSpan(surface, rect, span)) {
    return;
  }
  for (uint32_t row = span.row_first; row <= span.row_last; ++row) {
    for (uint32_t column = span.column_first; column <= span.column_last; ++column) {
      Rect tile_rect = TileRectAt(surface, row, column);
      tile_rect.right = std::min(tile_rect.right, int32_t(surface.width));
      tile_rect.bottom = std::min(tile_rect.bottom, int32_t(surface.height));
      if (tile_rect.left >= rect.left && tile_rect.top >= rect.top &&
          tile_rect.right <= rect.right && tile_rect.bottom <= rect.bottom) {
        tiles_out.push_back(TileAt(surface, row, column, tile_count));
      }
    }
  }
}

void TileRects(const EdramSurface& surface, const std::vector<uint32_t>& tiles, const Rect& clip,
               uint32_t tile_count, std::vector<Rect>& rects_out) {
  rects_out.clear();
  for (uint32_t tile : tiles) {
    Rect t = TileRect(surface, tile, tile_count);
    t = {std::max(t.left, clip.left), std::max(t.top, clip.top), std::min(t.right, clip.right),
         std::min(t.bottom, clip.bottom)};
    if (t.empty()) {
      continue;
    }
    if (!rects_out.empty()) {
      Rect& last = rects_out.back();
      if (last.top == t.top && last.bottom == t.bottom && last.right == t.left) {
        last.right = t.right;
        continue;
      }
    }
    rects_out.push_back(t);
  }
  // Rows with the same extent stack into one rectangle.
  size_t merged = 0;
  for (size_t i = 0; i < rects_out.size(); ++i) {
    if (merged) {
      Rect& last = rects_out[merged - 1];
      const Rect& r = rects_out[i];
      if (last.left == r.left && last.right == r.right && last.bottom == r.top) {
        last.bottom = r.bottom;
        continue;
      }
    }
    rects_out[merged++] = rects_out[i];
  }
  rects_out.resize(merged);
}

void UniformTiles::Reset(uint32_t tile_count) {
  for (uint32_t c = 0; c < 2; ++c) {
    bits_[c].assign((tile_count + 63) / 64, 0);
    value_[c] = 0;
    count_[c] = 0;
  }
}

void UniformTiles::Set(uint32_t component, const std::vector<uint32_t>& tiles, uint32_t value) {
  std::vector<uint64_t>& bits = bits_[component];
  if (value_[component] != value) {
    std::fill(bits.begin(), bits.end(), 0);
    count_[component] = 0;
    value_[component] = value;
  }
  for (uint32_t tile : tiles) {
    if (tile / 64 >= bits.size()) {
      continue;
    }
    uint64_t& word = bits[tile / 64];
    const uint64_t bit = uint64_t(1) << (tile % 64);
    if (!(word & bit)) {
      word |= bit;
      ++count_[component];
    }
  }
}

void UniformTiles::Forget(uint32_t component, uint32_t tile) {
  std::vector<uint64_t>& bits = bits_[component];
  if (tile / 64 >= bits.size()) {
    return;
  }
  uint64_t& word = bits[tile / 64];
  const uint64_t bit = uint64_t(1) << (tile % 64);
  if (word & bit) {
    word &= ~bit;
    --count_[component];
  }
}

void UniformTiles::Forget(uint32_t component, const EdramSurface& surface, const Rect& rect,
                          uint32_t tile_count) {
  if (!count_[component]) {
    return;
  }
  ForEachTile(surface, rect, tile_count, [&](uint32_t tile) { Forget(component, tile); });
}

bool UniformTiles::Has(uint32_t component, uint32_t tile) const {
  const std::vector<uint64_t>& bits = bits_[component];
  return tile / 64 < bits.size() && (bits[tile / 64] >> (tile % 64)) & 1;
}

bool IsUniformColorFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return true;
    default:
      return false;
  }
}

uint32_t Float32To7e3(float f32) {
  // NaN to 0, like the clamp of the shader translators (max then min).
  f32 = std::isnan(f32) ? 0.0f : std::clamp(f32, 0.0f, 31.875f);
  const uint32_t bits = std::bit_cast<uint32_t>(f32);
  // Below 0.25 (2^-2) the result is denormal: the mantissa with its implicit
  // one, shifted right by how far the exponent is under 2^-2. Otherwise the
  // exponent is rebiased from 127 to 3 (adding 0xC2000000 subtracts 124 << 23).
  const uint32_t biased =
      bits < 0x3E800000u
          ? ((bits & 0x7FFFFFu) | 0x800000u) >> std::min(125u - (bits >> 23), 24u)
          : bits + 0xC2000000u;
  // Round to the nearest even, keep 3 exponent and 7 mantissa bits.
  return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
}

namespace {

uint32_t ToUnorm(float v, float max) {
  if (std::isnan(v)) {
    v = 0.0f;
  }
  return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * max));
}

}  // namespace

uint32_t PackColorRaw(const float color[4], xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return ToUnorm(color[0], 1023.0f) | (ToUnorm(color[1], 1023.0f) << 10) |
             (ToUnorm(color[2], 1023.0f) << 20) | (ToUnorm(color[3], 3.0f) << 30);
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return Float32To7e3(color[0]) | (Float32To7e3(color[1]) << 10) |
             (Float32To7e3(color[2]) << 20) | (ToUnorm(color[3], 3.0f) << 30);
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return std::bit_cast<uint32_t>(color[0]);
    default:
      return ToUnorm(color[0], 255.0f) | (ToUnorm(color[1], 255.0f) << 8) |
             (ToUnorm(color[2], 255.0f) << 16) | (ToUnorm(color[3], 255.0f) << 24);
  }
}

void UnpackColorRaw(uint32_t raw, xenos::ColorRenderTargetFormat format, float color_out[4]) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      for (uint32_t i = 0; i < 3; ++i) {
        color_out[i] = float((raw >> (i * 10)) & 0x3FF) / 1023.0f;
      }
      color_out[3] = float(raw >> 30) / 3.0f;
      break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      for (uint32_t i = 0; i < 3; ++i) {
        color_out[i] = xenos::Float7e3To32((raw >> (i * 10)) & 0x3FF);
      }
      color_out[3] = float(raw >> 30) / 3.0f;
      break;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      color_out[0] = std::bit_cast<float>(raw);
      color_out[1] = color_out[2] = color_out[3] = 0.0f;
      break;
    default:
      for (uint32_t i = 0; i < 4; ++i) {
        color_out[i] = float((raw >> (i * 8)) & 0xFF) / 255.0f;
      }
      break;
  }
}

void DecodeColorClear(xenos::ColorRenderTargetFormat format, uint32_t rb_color_clear,
                      uint32_t rb_color_clear_lo, float color_out[4]) {
  // 16-bit fixed: -32...32 in the EDRAM, stored /32 on the host (snorm), so
  // the host value is the raw code over 32767.
  auto fixed16 = [](uint32_t code) {
    return std::max(float(int16_t(uint16_t(code))) / 32767.0f, -1.0f);
  };
  auto half = [](uint32_t code) { return rex::xenos_half_to_float(uint16_t(code)); };
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      color_out[0] = fixed16(rb_color_clear);
      color_out[1] = fixed16(rb_color_clear >> 16);
      color_out[2] = color_out[3] = 0.0f;
      break;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      color_out[0] = half(rb_color_clear);
      color_out[1] = half(rb_color_clear >> 16);
      color_out[2] = color_out[3] = 0.0f;
      break;
    // 64bpp: the low dword (R | G << 16, or R) in RB_COLOR_CLEAR_LO, the high
    // one (B | A << 16, or G) in RB_COLOR_CLEAR.
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      color_out[0] = fixed16(rb_color_clear_lo);
      color_out[1] = fixed16(rb_color_clear_lo >> 16);
      color_out[2] = fixed16(rb_color_clear);
      color_out[3] = fixed16(rb_color_clear >> 16);
      break;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      color_out[0] = half(rb_color_clear_lo);
      color_out[1] = half(rb_color_clear_lo >> 16);
      color_out[2] = half(rb_color_clear);
      color_out[3] = half(rb_color_clear >> 16);
      break;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      color_out[0] = std::bit_cast<float>(rb_color_clear_lo);
      color_out[1] = std::bit_cast<float>(rb_color_clear);
      color_out[2] = color_out[3] = 0.0f;
      break;
    default:
      // 32bpp: the cell in RB_COLOR_CLEAR (8_8_8_8_GAMMA as its raw bytes).
      UnpackColorRaw(rb_color_clear, format, color_out);
      break;
  }
}

uint32_t SurfaceHeightPixels(uint32_t base_tiles, uint32_t pitch_pixels, xenos::MsaaSamples msaa,
                             bool is_64bpp, uint32_t tile_count) {
  EdramSurface surface;
  surface.pitch_pixels = pitch_pixels;
  surface.msaa = msaa;
  surface.is_64bpp = is_64bpp;
  uint32_t pitch_tiles = std::max(PitchTiles(surface), uint32_t(1));
  uint32_t base = base_tiles % tile_count;
  uint32_t rows = (tile_count - base) / pitch_tiles;
  return std::clamp(rows * TileHeightPixels(surface), kMinSurfaceHeight, kMaxSurfaceHeight);
}

uint32_t EncodeDepth24(float depth, xenos::DepthRenderTargetFormat format) {
  depth = std::isnan(depth) ? 0.0f : std::clamp(depth, 0.0f, 1.0f);
  if (format == xenos::DepthRenderTargetFormat::kD24FS8) {
    return xenos::Float32To20e4(depth * 2.0f, true);
  }
  // In double: float has 24 mantissa bits, too few to round depth * (2^24 - 1).
  return uint32_t(std::lround(double(depth) * 16777215.0));
}

float DecodeDepth24(uint32_t depth24, xenos::DepthRenderTargetFormat format) {
  if (format == xenos::DepthRenderTargetFormat::kD24FS8) {
    return xenos::Float20e4To32(depth24) * 0.5f;
  }
  return float(double(depth24 & 0xFFFFFF) / 16777215.0);
}

}  // namespace rex::graphics::native
