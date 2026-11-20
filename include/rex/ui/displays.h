/**
 * @file        rex/ui/displays.h
 * @brief       The monitors and their full-screen modes, as the window code counts them
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <vector>

namespace rex::ui {

struct DisplayMode {
  int width = 0;   // pixels
  int height = 0;  // pixels
  float refresh_rate = 0.0f;  // the highest one of this size; 0 if unknown
};

struct DisplayInfo {
  std::string name;
  bool primary = false;
  DisplayMode desktop;
  std::vector<DisplayMode> modes;  // one per size, largest first
};

/// The displays in the order of the `monitor` setting (the first one is
/// monitor = 1), each with its full-screen sizes. Needs the windowing
/// (SDL video) set up, as in any windowed app; empty otherwise.
std::vector<DisplayInfo> EnumerateDisplays();

}  // namespace rex::ui
