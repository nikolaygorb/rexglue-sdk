/**
 * @file        graphics/native_resolve_rect_test.cpp
 * @brief       Tests for the native renderer's resolve rectangle
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/native/resolve_rect.h>
#include <rex/graphics/xenos.h>

using namespace rex::graphics;
using namespace rex::graphics::native;

namespace {

// D3D's three resolve vertices for [x0, x1) x [y0, y1), with its half-pixel
// offset (pix_center kD3DZero) unless `half_pixel` is false.
ResolveRectInput Input(float x0, float y0, float x1, float y1, uint32_t pitch = 1280,
                       bool half_pixel = true) {
  ResolveRectInput input;
  float offset = half_pixel ? -0.5f : 0.0f;
  const float vertices[6] = {x0 + offset, y0 + offset, x1 + offset,
                             y0 + offset, x1 + offset, y1 + offset};
  for (int i = 0; i < 6; ++i) {
    input.vertices[i] = vertices[i];
  }
  input.half_pixel_offset = half_pixel;
  input.scissor = {0, 0, 8192, 8192};
  input.surface_pitch = pitch;
  return input;
}

}  // namespace

TEST_CASE("Native resolve rectangle of a full screen", "[graphics][native][resolve]") {
  CHECK(ResolveRect(Input(0, 0, 1280, 720)) == Rect{0, 0, 1280, 720});
  CHECK(ResolveRect(Input(0, 0, 1280, 720, 1280, false)) == Rect{0, 0, 1280, 720});
  // The vertex order does not matter.
  ResolveRectInput input = Input(0, 0, 640, 360);
  std::swap(input.vertices[0], input.vertices[4]);
  std::swap(input.vertices[1], input.vertices[5]);
  CHECK(ResolveRect(input) == Rect{0, 0, 640, 360});
}

TEST_CASE("Native resolve rectangle is expanded to 8 pixels", "[graphics][native][resolve]") {
  CHECK(ResolveRect(Input(3, 9, 13, 17)) == Rect{0, 8, 16, 24});
}

TEST_CASE("Native resolve rectangle follows the top-left rule", "[graphics][native][resolve]") {
  // A right edge exactly on a pixel centre (8.5) leaves pixel 8 out; a hair
  // past it takes it in (and the alignment then takes the next 8).
  ResolveRectInput on_center = Input(0, 0, 8, 8, 1280, false);
  on_center.vertices[2] = on_center.vertices[4] = 8.5f;
  CHECK(ResolveRect(on_center) == Rect{0, 0, 8, 8});
  ResolveRectInput past_center = on_center;
  past_center.vertices[2] = past_center.vertices[4] = 8.5f + 1.0f / 256.0f;
  CHECK(ResolveRect(past_center) == Rect{0, 0, 16, 8});
}

TEST_CASE("Native resolve rectangle with the window offset and scissor",
          "[graphics][native][resolve]") {
  ResolveRectInput input = Input(0, 0, 64, 64);
  input.window_x_offset = 16;
  input.window_y_offset = 32;
  CHECK(ResolveRect(input) == Rect{0, 0, 64, 64});  // not enabled
  input.vtx_window_offset_enable = true;
  CHECK(ResolveRect(input) == Rect{16, 32, 80, 96});

  ResolveRectInput clipped = Input(0, 0, 1280, 720);
  clipped.scissor = {100, 50, 600, 300};
  // Clipped, then expanded to 8.
  CHECK(ResolveRect(clipped) == Rect{96, 48, 600, 304});

  ResolveRectInput outside = Input(0, 0, 64, 64);
  outside.scissor = {128, 128, 256, 256};
  CHECK(ResolveRect(outside).empty());
}

TEST_CASE("Native resolve rectangle is clamped to the pitch and the size limit",
          "[graphics][native][resolve]") {
  // Pitch 1284: 1280 aligned.
  CHECK(ResolveRect(Input(0, 0, 1300, 16, 1284)) == Rect{0, 0, 1280, 16});
  CHECK(ResolveRect(Input(1288, 0, 1300, 16, 1284)).empty());
  ResolveRectInput tall = Input(0, 0, 16, 17000);
  tall.scissor = {0, 0, 8192, 20000};
  CHECK(ResolveRect(tall) == Rect{0, 0, 16, int32_t(xenos::kMaxResolveSize)});
}

TEST_CASE("Native resolve pieces cover a texture together",
          "[graphics][native][resolve]") {
  // Predicated tiling, as RDR: 1280x512 at the base, 1280x208 at row 512.
  CHECK(RectsCoverTexture({{0, 0, 1280, 512}, {0, 512, 1280, 720}}, 1280, 720));
  // Order and overlap don't matter.
  CHECK(RectsCoverTexture({{0, 496, 1280, 720}, {0, 0, 1280, 512}}, 1280, 720));
  // One row missing between the tiles, or the last row.
  CHECK_FALSE(RectsCoverTexture({{0, 0, 1280, 512}, {0, 513, 1280, 720}}, 1280, 720));
  CHECK_FALSE(RectsCoverTexture({{0, 0, 1280, 512}, {0, 512, 1280, 719}}, 1280, 720));
  // Only the first tile.
  CHECK_FALSE(RectsCoverTexture({{0, 0, 1280, 512}}, 1280, 720));
  // Vertical split, a column missing at the right edge.
  CHECK(RectsCoverTexture({{0, 0, 640, 720}, {640, 0, 1280, 720}}, 1280, 720));
  CHECK_FALSE(RectsCoverTexture({{0, 0, 640, 720}, {640, 0, 1279, 720}}, 1280, 720));
  // A hole in the middle of a band covered on both sides.
  CHECK_FALSE(RectsCoverTexture(
      {{0, 0, 1280, 300}, {0, 300, 600, 400}, {700, 300, 1280, 400}, {0, 400, 1280, 720}},
      1280, 720));
  // Four quadrants; pieces reaching outside the texture are clipped to it.
  CHECK(RectsCoverTexture(
      {{0, 0, 640, 360}, {640, 0, 1280, 360}, {0, 360, 640, 720}, {640, 360, 1300, 800}},
      1280, 720));
  CHECK(RectsCoverTexture({{-32, -32, 1312, 752}}, 1280, 720));
  // Empty inputs.
  CHECK_FALSE(RectsCoverTexture({}, 1280, 720));
  CHECK_FALSE(RectsCoverTexture({{0, 0, 1280, 720}}, 0, 720));
  CHECK_FALSE(RectsCoverTexture({{0, 0, 0, 0}}, 16, 16));
}
