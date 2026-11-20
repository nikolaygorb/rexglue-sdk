/**
 * @file        input/deadzone_test.cpp
 * @brief       Tests the radial thumbstick deadzone against the XInput range, in every direction
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <rex/input/deadzone.h>

using rex::input::ApplyRadialDeadzone;

namespace {

bool Cut(double percentage, int16_t x, int16_t y) {
  ApplyRadialDeadzone(percentage, x, y);
  return x == 0 && y == 0;
}

}  // namespace

TEST_CASE("The deadzone is a share of the thumbstick range", "[input][deadzone]") {
  // 25 %: 8191.75 of 32767. The SDL caps (0xFFFF) made it 16383.75.
  CHECK(Cut(0.25, 8000, 0));
  CHECK_FALSE(Cut(0.25, 8300, 0));
  CHECK_FALSE(Cut(0.25, 12000, 0));
}

TEST_CASE("The deadzone holds left and down too", "[input][deadzone]") {
  // A negative cosine or sine made the cutoff negative and the axis was kept.
  CHECK(Cut(0.25, -8000, 0));
  CHECK(Cut(0.25, 0, -8000));
  CHECK(Cut(0.25, -5000, -5000));
  CHECK(Cut(0.25, -5000, 5000));
  CHECK_FALSE(Cut(0.25, -8300, 0));
  CHECK_FALSE(Cut(0.25, -7000, -7000));
}

TEST_CASE("The deadzone is radial", "[input][deadzone]") {
  // 5000 on both axes is 7071 from the center: inside 8191.75.
  CHECK(Cut(0.25, 5000, 5000));
  // 6000 on both is 8485: outside, both axes kept.
  int16_t x = 6000, y = 6000;
  ApplyRadialDeadzone(0.25, x, y);
  CHECK(x == 6000);
  CHECK(y == 6000);
}

TEST_CASE("A deadzone of 0 or of 1 and more leaves the stick", "[input][deadzone]") {
  CHECK_FALSE(Cut(0.0, 100, 100));
  CHECK_FALSE(Cut(1.0, 100, 100));
}
