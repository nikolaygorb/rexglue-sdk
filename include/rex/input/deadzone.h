/**
 * @file        rex/input/deadzone.h
 * @brief       Radial thumbstick deadzone (left_stick_deadzone_percentage and the right one)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cmath>
#include <cstdint>

namespace rex::input {

// Zeroes the axes of a stick inside a radial deadzone of `percentage` of the
// thumbstick range: an axis is cut when its part of the deflection is below
// the deadzone's part along the stick's angle, so a diagonal is not held to a
// larger displacement than a cardinal push. 0 or less, or 1 or more, leaves
// the stick as it is.
//
// The range is the XInput one (upstream rexglue ac2a4749): the device
// capabilities it was scaled by are not a range (the SDL driver reports 0xFFFF,
// which made the deadzone twice the size asked for). The cutoffs are
// magnitudes: with a negative cosine or sine (stick left or down) the test was
// empty, so those half planes had no deadzone on that axis.
inline void ApplyRadialDeadzone(double percentage, int16_t& axis_x, int16_t& axis_y) {
  if (percentage <= 0.0 || percentage >= 1.0) {
    return;
  }
  constexpr double kThumbstickMax = 32767.0;
  const double deadzone = kThumbstickMax * percentage;
  const double theta = std::atan2(static_cast<double>(axis_y), static_cast<double>(axis_x));
  const double cutoff_x = std::abs(std::cos(theta)) * deadzone;
  const double cutoff_y = std::abs(std::sin(theta)) * deadzone;
  if (axis_x > -cutoff_x && axis_x < cutoff_x) {
    axis_x = 0;
  }
  if (axis_y > -cutoff_y && axis_y < cutoff_y) {
    axis_y = 0;
  }
}

}  // namespace rex::input
