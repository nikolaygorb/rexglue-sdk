/**
 * @file        graphics/native/scale.h
 * @brief       Native renderer: draw resolution scale in quarters
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              "Resolution scale"), Copyright (c) 2026 OpenPortroid, MIT License
 *              (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics::native {

// Rectangle in pixels, right / bottom exclusive (guest or host pixels, as the
// caller says).
struct Rect {
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;

  constexpr bool empty() const { return left >= right || top >= bottom; }
  friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

// The scale is kept in quarters (scale4: 4 = 1x, 5 = 1.25x, 6 = 1.5x, 8 = 2x,
// up to 16 = 4x). At any of them an EDRAM tile (80x16 samples, 20 * scale4 by
// 4 * scale4 host pixels) is a whole number of host pixels, so per-tile
// bookkeeping stays exact. At a fractional scale guest pixels are not all as
// many host pixels wide (at 1.5x: one, two, one, two...): a host pixel belongs
// to the guest pixel its centre is in, which is what the rasterizer covers of a
// guest rectangle stretched over the host surface.
constexpr uint32_t kScale4Min = 4;
constexpr uint32_t kScale4Max = 16;

// A scale factor (1.0 to 4.0) to the nearest quarter.
inline uint32_t Scale4FromFactor(double scale) {
  if (!(scale >= 1.0)) {  // also NaN
    scale = 1.0;
  }
  return uint32_t(std::lround(std::min(scale, 4.0) * 4.0));
}

// Guest pixel edge -> host pixel edge: the first host pixel whose centre is at
// or after the guest edge, floor((v * scale4 + 1) / 4). Consistent with the
// rasterizer's top-left rule for a pixel centre exactly on the edge.
constexpr int32_t ScaleEdge(int32_t v, uint32_t scale4) {
  // Arithmetic shift: floor also for negative edges.
  return (v * int32_t(scale4) + 1) >> 2;
}

constexpr Rect ScaleRect(const Rect& r, uint32_t scale4) {
  return {ScaleEdge(r.left, scale4), ScaleEdge(r.top, scale4), ScaleEdge(r.right, scale4),
          ScaleEdge(r.bottom, scale4)};
}

// Host size of a surface `size` guest pixels long (rounded up, so it holds
// ScaleEdge(size) host pixels and, at a fractional scale, at most one more).
constexpr uint32_t ScaleSize(uint32_t size, uint32_t scale4) {
  return (size * scale4 + 3) / 4;
}

// The host pixel holding the centre of guest pixel `v`; on a tie (the centre on
// a host pixel edge, at even scales) the top-left one, like the SDK's readback
// downscale.
constexpr uint32_t CenterPixel(uint32_t v, uint32_t scale4) {
  return ((2 * v + 1) * scale4 - 1) >> 3;
}

// The whole scale small render targets use (the next one up, 2x at 1.5x).
constexpr uint32_t SmallTargetScale4(uint32_t scale4) {
  return (scale4 + 3) & ~uint32_t(3);
}

// Small render targets (downsampled copies, blur, bloom, depth of field, the
// luminance chain down to a 1x1 the CPU reads) are drawn at a whole scale:
// their passes step through their sources in guest texels, and at a fractional
// scale neighbouring host pixels would take different texels (striped blur, an
// unevenly weighted average). `small_width` is the widest such surface in guest
// pixels (0 = none). A 4x MSAA surface is half as wide as the 1x view of the
// same EDRAM, so it counts at twice its pitch.
constexpr bool IsSmallTarget(uint32_t pitch_pixels, xenos::MsaaSamples msaa, uint32_t scale4,
                             uint32_t small_width) {
  return scale4 > kScale4Min &&
         pitch_pixels * (msaa >= xenos::MsaaSamples::k4X ? 2u : 1u) <= small_width;
}

// Scale of the render targets of a surface.
constexpr uint32_t SurfaceScale4(uint32_t pitch_pixels, xenos::MsaaSamples msaa, uint32_t scale4,
                                 uint32_t small_width) {
  return IsSmallTarget(pitch_pixels, msaa, scale4, small_width) ? SmallTargetScale4(scale4)
                                                                : scale4;
}

}  // namespace rex::graphics::native
