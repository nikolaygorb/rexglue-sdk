/**
 * @file        launcher/launcher_bindings.h
 * @brief       The key bindings screen's gamepad: its buttons, their keybind_*
 *              settings, and its drawing (own shapes, in the title's colours)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <vector>

#include <imgui.h>

#include "launcher_manifest.h"

namespace rex::launcher::bindings {

struct PadKey {
  const char* suffix;  // the setting is keybind_<suffix>
  const char* label;   // "Up", or the button's name for a one-key button
};

// One button (or stick, or D-pad) of the pad with its callout box.
struct PadElement {
  const char* id;
  const char* label;
  bool right;     // callout on the right side
  float y;        // callout top, canvas units
  ImVec2 anchor;  // where the callout's line ends, pad units
  std::vector<PadKey> keys;
};

// The canvas the pad and callouts are laid out on, in units scaled to the screen.
inline constexpr float kCanvasWidth = 760.0f;
inline constexpr float kCanvasHeight = 352.0f;
inline constexpr float kCalloutWidth = 190.0f;
inline constexpr float kCalloutHeight = 36.0f;

const std::vector<PadElement>& PadElements();

// A point of the pad drawing (pad units) on the screen.
ImVec2 PadPoint(ImVec2 canvas, float scale, ImVec2 pad);
// The top left of an element's callout on the screen.
ImVec2 CalloutPosition(ImVec2 canvas, float scale, const PadElement& element);

// Draws the pad; the element `selected` gets the accent outline.
void DrawPad(ImDrawList* draw_list, ImVec2 canvas, float scale, const std::string& selected,
             const Theme& theme, ImFont* letter_font);

// A binding's keys from its value text ("\"Semicolon,Space\"") and back.
std::vector<std::string> KeyList(const ValueText& value);
ValueText KeyListValue(const std::vector<std::string>& keys);
// "Semicolon" -> ";", "Control" -> "Ctrl": for showing.
std::string PrettyKey(const std::string& key);

}  // namespace rex::launcher::bindings
