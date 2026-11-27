/**
 * @file        graphics/native_scale_test.cpp
 * @brief       Tests for the native renderer's resolution scale in quarters
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/native/scale.h>

using namespace rex::graphics;
using namespace rex::graphics::native;

namespace {

// The definition: host pixel h belongs to the guest pixel its centre is in. The
// first host pixel at or after guest edge v is the smallest h with
// (h + 0.5) * 4 / scale4 >= v, in integers (2h + 1) * 4 >= 2 v scale4.
int32_t FirstHostPixelAtOrAfter(int32_t v, uint32_t scale4) {
  int32_t h = 0;
  while ((2 * h + 1) * 4 < 2 * v * int32_t(scale4)) {
    ++h;
  }
  return h;
}

}  // namespace

TEST_CASE("Native scale factors round to quarters", "[graphics][native]") {
  CHECK(Scale4FromFactor(1.0) == 4);
  CHECK(Scale4FromFactor(1.3) == 5);
  CHECK(Scale4FromFactor(1.5) == 6);
  CHECK(Scale4FromFactor(2.0) == 8);
  CHECK(Scale4FromFactor(4.0) == 16);
  CHECK(Scale4FromFactor(5.0) == 16);
  CHECK(Scale4FromFactor(0.5) == 4);
  CHECK(Scale4FromFactor(std::numeric_limits<double>::quiet_NaN()) == 4);
}

TEST_CASE("Native ScaleEdge examples", "[graphics][native]") {
  for (int32_t v = 0; v < 16; ++v) {
    CHECK(ScaleEdge(v, 4) == v);
    CHECK(ScaleEdge(v, 8) == 2 * v);
  }
  // 1.5x: one, two, one, two host pixels per guest pixel.
  CHECK(ScaleEdge(0, 6) == 0);
  CHECK(ScaleEdge(1, 6) == 1);
  CHECK(ScaleEdge(2, 6) == 3);
  CHECK(ScaleEdge(3, 6) == 4);
  CHECK(ScaleEdge(4, 6) == 6);
  // 1.25x.
  CHECK(ScaleEdge(1, 5) == 1);
  CHECK(ScaleEdge(2, 5) == 2);
  CHECK(ScaleEdge(3, 5) == 4);
  CHECK(ScaleEdge(4, 5) == 5);
  CHECK(ScaleRect({0, 0, 1280, 720}, 6) == Rect{0, 0, 1920, 1080});
}

TEST_CASE("Native ScaleEdge follows pixel centres at every scale", "[graphics][native]") {
  for (uint32_t scale4 = kScale4Min; scale4 <= kScale4Max; ++scale4) {
    for (int32_t v = 0; v <= 400; ++v) {
      REQUIRE(ScaleEdge(v, scale4) == FirstHostPixelAtOrAfter(v, scale4));
    }
    // EDRAM tile edges (80x16 samples) land on whole host pixels.
    for (int32_t tiles = 0; tiles <= 32; ++tiles) {
      CHECK(ScaleEdge(80 * tiles, scale4) * 4 == 80 * tiles * int32_t(scale4));
      CHECK(ScaleEdge(16 * tiles, scale4) * 4 == 16 * tiles * int32_t(scale4));
    }
  }
}

TEST_CASE("Native CenterPixel is a host pixel of its guest pixel", "[graphics][native]") {
  CHECK(CenterPixel(0, 4) == 0);
  CHECK(CenterPixel(5, 4) == 5);
  // 2x: the top-left of the 2x2 block.
  CHECK(CenterPixel(0, 8) == 0);
  CHECK(CenterPixel(3, 8) == 6);
  CHECK(CenterPixel(2, 5) == 3);
  for (uint32_t scale4 = kScale4Min; scale4 <= kScale4Max; ++scale4) {
    for (uint32_t v = 0; v <= 400; ++v) {
      const uint32_t h = CenterPixel(v, scale4);
      // Inside the guest pixel's host span.
      REQUIRE(int32_t(h) >= ScaleEdge(int32_t(v), scale4));
      REQUIRE(int32_t(h) < ScaleEdge(int32_t(v) + 1, scale4));
      // Holds the guest centre: h < (2v + 1) scale4 / 8 <= h + 1.
      REQUIRE(8 * h < (2 * v + 1) * scale4);
      REQUIRE((2 * v + 1) * scale4 <= 8 * (h + 1));
    }
  }
}

TEST_CASE("Native ScaleSize holds every scaled pixel", "[graphics][native]") {
  CHECK(ScaleSize(1280, 6) == 1920);
  CHECK(ScaleSize(1, 6) == 2);
  CHECK(ScaleSize(1, 5) == 2);
  CHECK(ScaleSize(720, 8) == 1440);
  for (uint32_t scale4 = kScale4Min; scale4 <= kScale4Max; ++scale4) {
    for (uint32_t size = 0; size <= 400; ++size) {
      const int32_t edge = ScaleEdge(int32_t(size), scale4);
      REQUIRE(int32_t(ScaleSize(size, scale4)) >= edge);
      REQUIRE(int32_t(ScaleSize(size, scale4)) - edge <= 1);
    }
  }
}

TEST_CASE("Native small render targets take a whole scale", "[graphics][native]") {
  CHECK(SmallTargetScale4(4) == 4);
  CHECK(SmallTargetScale4(5) == 8);
  CHECK(SmallTargetScale4(6) == 8);
  CHECK(SmallTargetScale4(8) == 8);
  CHECK(SmallTargetScale4(9) == 12);
  // Never at 1x.
  CHECK_FALSE(IsSmallTarget(320, xenos::MsaaSamples::k1X, 4, 480));
  CHECK(IsSmallTarget(320, xenos::MsaaSamples::k1X, 6, 480));
  CHECK(IsSmallTarget(480, xenos::MsaaSamples::k2X, 6, 480));
  // 4x MSAA counts at twice its pitch.
  CHECK_FALSE(IsSmallTarget(320, xenos::MsaaSamples::k4X, 6, 480));
  CHECK(IsSmallTarget(240, xenos::MsaaSamples::k4X, 6, 480));
  CHECK_FALSE(IsSmallTarget(320, xenos::MsaaSamples::k1X, 6, 0));
  CHECK(SurfaceScale4(320, xenos::MsaaSamples::k1X, 6, 480) == 8);
  CHECK(SurfaceScale4(1280, xenos::MsaaSamples::k1X, 6, 480) == 6);
}
