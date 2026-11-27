/**
 * @file        ui/displays.cpp
 * @brief       The monitors and their full-screen modes, as the window code counts them
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/displays.h>

#include <algorithm>

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

namespace rex::ui {

std::vector<DisplayInfo> EnumerateDisplays() {
  std::vector<DisplayInfo> out;
  int count = 0;
  // The order WindowSDL uses for the monitor setting (window_sdl.cpp).
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  if (!displays) {
    return out;
  }
  const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
  for (int i = 0; i < count; ++i) {
    const SDL_DisplayID id = displays[i];
    DisplayInfo info;
    const char* name = SDL_GetDisplayName(id);
    info.name = name ? name : "";
    info.primary = id == primary;
    auto to_mode = [](const SDL_DisplayMode& mode) {
      const float density = mode.pixel_density > 0.0f ? mode.pixel_density : 1.0f;
      return DisplayMode{int(float(mode.w) * density + 0.5f), int(float(mode.h) * density + 0.5f),
                         mode.refresh_rate};
    };
    if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(id)) {
      info.desktop = to_mode(*desktop);
    }
    int mode_count = 0;
    if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &mode_count)) {
      for (int m = 0; m < mode_count; ++m) {
        if (!modes[m]) {
          continue;
        }
        const DisplayMode mode = to_mode(*modes[m]);
        auto same = std::find_if(info.modes.begin(), info.modes.end(), [&](const DisplayMode& o) {
          return o.width == mode.width && o.height == mode.height;
        });
        if (same == info.modes.end()) {
          info.modes.push_back(mode);
        } else {
          same->refresh_rate = std::max(same->refresh_rate, mode.refresh_rate);
        }
      }
      SDL_free(modes);
    }
    std::sort(info.modes.begin(), info.modes.end(), [](const DisplayMode& a, const DisplayMode& b) {
      return a.width != b.width ? a.width > b.width : a.height > b.height;
    });
    out.push_back(std::move(info));
  }
  SDL_free(displays);
  return out;
}

}  // namespace rex::ui
