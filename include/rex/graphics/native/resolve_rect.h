/**
 * @file        graphics/native/resolve_rect.h
 * @brief       Native renderer: the guest rectangle of a resolve
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              RendererResolve), Copyright (c) 2026 OpenPortroid, MIT License
 *              (src/graphics/native/LICENSE.ArmyOfTwoRecompilation), itself a port of
 *              draw_util::GetResolveInfo.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <rex/graphics/native/scale.h>

namespace rex::graphics::native {

// What the rectangle of a resolve is computed from: the three vertices D3D
// puts in vertex fetch constant 0 (x, y pairs, endian-swapped), and registers.
struct ResolveRectInput {
  float vertices[6] = {};
  // PA_SU_VTX_CNTL::pix_center is kD3DZero (D3D9's half-pixel offset, undone).
  bool half_pixel_offset = false;
  // PA_SU_SC_MODE_CNTL::vtx_window_offset_enable with PA_SC_WINDOW_OFFSET.
  bool vtx_window_offset_enable = false;
  int32_t window_x_offset = 0;
  int32_t window_y_offset = 0;
  // draw_util::GetScissor(regs, scissor, false): window scissor, not clamped
  // to the surface pitch.
  Rect scissor;
  // RB_SURFACE_INFO::surface_pitch.
  uint32_t surface_pitch = 0;
};

// The covered pixels in surface coordinates, as draw_util::GetResolveInfo
// derives them: 16.8 fixed point vertices with the top-left rule, the window
// offset, the scissor, alignment to 8 (kResolveAlignmentPixels) by expanding,
// clamped to the 8-aligned pitch and to kMaxResolveSize rows. Empty when
// nothing is left.
Rect ResolveRect(const ResolveRectInput& input);

// Whether the rectangles together (clipped to the texture) cover all of a
// width x height texture, holes and gaps of any size refused. Predicated
// tiling resolves a 1280x720 surface as 1280x512 and 1280x208 pieces, none of
// which covers it alone. Bands between the rectangles' top / bottom edges,
// each checked for an unbroken run of spans from 0 to width; at most a few
// dozen rectangles (kMaxWrittenRects per resolve image).
inline bool RectsCoverTexture(const std::vector<Rect>& rects, uint32_t width, uint32_t height) {
  if (!width || !height || rects.empty()) return false;
  const int32_t w = int32_t(width), h = int32_t(height);
  std::vector<int32_t> edges = {0, h};
  for (const Rect& r : rects) {
    if (r.empty()) continue;
    if (r.top > 0 && r.top < h) edges.push_back(r.top);
    if (r.bottom > 0 && r.bottom < h) edges.push_back(r.bottom);
  }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  std::vector<std::pair<int32_t, int32_t>> spans;
  for (size_t i = 0; i + 1 < edges.size(); ++i) {
    const int32_t band_top = edges[i], band_bottom = edges[i + 1];
    spans.clear();
    for (const Rect& r : rects) {
      if (!r.empty() && r.top <= band_top && r.bottom >= band_bottom && r.left < w && r.right > 0) {
        spans.emplace_back(std::max(r.left, 0), std::min(r.right, w));
      }
    }
    std::sort(spans.begin(), spans.end());
    int32_t reach = 0;
    for (const auto& [left, right] : spans) {
      if (left > reach) break;
      reach = std::max(reach, right);
    }
    if (reach < w) return false;
  }
  return true;
}

}  // namespace rex::graphics::native
