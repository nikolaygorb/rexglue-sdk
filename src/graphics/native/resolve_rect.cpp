/**
 * @file        graphics/native/resolve_rect.cpp
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

#include <rex/graphics/native/resolve_rect.h>

#include <algorithm>

#include <rex/graphics/xenos.h>
#include <rex/ui/graphics_util.h>

namespace rex::graphics::native {

Rect ResolveRect(const ResolveRectInput& input) {
  const float half_pixel = input.half_pixel_offset ? 0.5f : 0.0f;
  int32_t fixed[6];
  for (uint32_t i = 0; i < 6; ++i) {
    fixed[i] = ui::FloatToD3D11Fixed16p8(input.vertices[i] + half_pixel);
  }
  int32_t x0 = std::min({fixed[0], fixed[2], fixed[4]});
  int32_t y0 = std::min({fixed[1], fixed[3], fixed[5]});
  int32_t x1 = std::max({fixed[0], fixed[2], fixed[4]});
  int32_t y1 = std::max({fixed[1], fixed[3], fixed[5]});
  // Top-left rule: .5 is covered on the left / top, not on the right / bottom.
  x0 = (x0 + 127) >> 8;
  y0 = (y0 + 127) >> 8;
  x1 = (x1 + 127) >> 8;
  y1 = (y1 + 127) >> 8;

  if (input.vtx_window_offset_enable) {
    x0 += input.window_x_offset;
    y0 += input.window_y_offset;
    x1 += input.window_x_offset;
    y1 += input.window_y_offset;
  }

  const Rect& scissor = input.scissor;
  x0 = std::clamp(x0, scissor.left, std::max(scissor.left, scissor.right));
  y0 = std::clamp(y0, scissor.top, std::max(scissor.top, scissor.bottom));
  x1 = std::clamp(x1, scissor.left, std::max(scissor.left, scissor.right));
  y1 = std::clamp(y1, scissor.top, std::max(scissor.top, scissor.bottom));

  // D3D aligns to 8 itself; expand to be safe, as the SDK does.
  constexpr int32_t kAlignment = int32_t(xenos::kResolveAlignmentPixels);
  x0 &= ~(kAlignment - 1);
  y0 &= ~(kAlignment - 1);
  x1 = (x1 + kAlignment - 1) & ~(kAlignment - 1);
  y1 = (y1 + kAlignment - 1) & ~(kAlignment - 1);

  const int32_t pitch_aligned = int32_t(input.surface_pitch & ~uint32_t(kAlignment - 1));
  x0 = std::min(x0, pitch_aligned);
  x1 = std::min(x1, pitch_aligned);
  y1 = std::min(y1, y0 + int32_t(xenos::kMaxResolveSize));

  if (x0 >= x1 || y0 >= y1) {
    return Rect();
  }
  return {x0, y0, x1, y1};
}

}  // namespace rex::graphics::native
