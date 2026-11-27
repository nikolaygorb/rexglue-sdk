/**
 * @file        launcher/launcher_bindings.cpp
 * @brief       The key bindings screen's gamepad
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Before any include of imgui.h.
#define IMGUI_DEFINE_MATH_OPERATORS

#include "launcher_bindings.h"

#include <algorithm>
#include <cfloat>
#include <map>

#include <imgui_internal.h>

namespace rex::launcher::bindings {

namespace {

// The pad drawing's own units: x 134..626, y 68..360 (a generic Xbox 360 style
// pad, drawn here, no artwork). It sits in the canvas centred, scaled by 0.66.
constexpr float kPadScale = 0.66f;

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
  const ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                               x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

}  // namespace

const std::vector<PadElement>& PadElements() {
  static const std::vector<PadElement> elements = {
      {"lt", "Left trigger", false, 14.0f, {258.0f, 80.0f}, {{"left_trigger", "Left trigger"}}},
      {"lb", "Left bumper", false, 60.0f, {257.0f, 101.0f}, {{"left_shoulder", "Left bumper"}}},
      {"ls",
       "Left stick",
       false,
       120.0f,
       {290.0f, 168.0f},
       {{"lstick_up", "Up"},
        {"lstick_down", "Down"},
        {"lstick_left", "Left"},
        {"lstick_right", "Right"},
        {"lstick_press", "Press"}}},
      {"dp",
       "D-pad",
       false,
       190.0f,
       {328.0f, 228.0f},
       {{"dpad_up", "Up"}, {"dpad_down", "Down"}, {"dpad_left", "Left"}, {"dpad_right", "Right"}}},
      {"back", "Back", false, 252.0f, {352.0f, 168.0f}, {{"back", "Back"}}},
      {"guide", "Guide", false, 300.0f, {380.0f, 138.0f}, {{"guide", "Guide"}}},
      {"rt", "Right trigger", true, 14.0f, {502.0f, 80.0f}, {{"right_trigger", "Right trigger"}}},
      {"rb", "Right bumper", true, 56.0f, {503.0f, 101.0f}, {{"right_shoulder", "Right bumper"}}},
      {"y", "Y", true, 98.0f, {470.0f, 144.0f}, {{"y", "Y"}}},
      {"b", "B", true, 140.0f, {494.0f, 168.0f}, {{"b", "B"}}},
      {"a", "A", true, 182.0f, {470.0f, 192.0f}, {{"a", "A"}}},
      {"x", "X", true, 224.0f, {446.0f, 168.0f}, {{"x", "X"}}},
      {"rs",
       "Right stick",
       true,
       266.0f,
       {432.0f, 228.0f},
       {{"rstick_up", "Up"},
        {"rstick_down", "Down"},
        {"rstick_left", "Left"},
        {"rstick_right", "Right"},
        {"rstick_press", "Press"}}},
      {"start", "Start", true, 308.0f, {408.0f, 168.0f}, {{"start", "Start"}}},
  };
  return elements;
}

ImVec2 PadPoint(ImVec2 canvas, float scale, ImVec2 pad) {
  return ImVec2(canvas.x + (380.0f + (pad.x - 380.0f) * kPadScale) * scale,
                canvas.y + (60.0f + (pad.y - 70.0f) * kPadScale) * scale);
}

ImVec2 CalloutPosition(ImVec2 canvas, float scale, const PadElement& element) {
  const float x = element.right ? kCanvasWidth - 12.0f - kCalloutWidth : 12.0f;
  return ImVec2(canvas.x + x * scale, canvas.y + element.y * scale);
}

void DrawPad(ImDrawList* dl, ImVec2 canvas, float scale, const std::string& selected,
             const Theme& t, ImFont* letter_font) {
  auto P = [&](float x, float y) { return PadPoint(canvas, scale, ImVec2(x, y)); };
  const float k = kPadScale * scale;  // pad units to screen pixels
  const ImU32 body = t.panel;
  const ImU32 well = t.bg2;
  const ImU32 part = t.panel_hot;
  const ImU32 edge = t.border;
  const ImU32 accent = t.accent;
  const float edge_width = std::max(1.0f, 2.0f * k);
  const float accent_width = std::max(2.0f, 4.0f * k);
  auto on = [&](const char* id) { return selected == id; };

  // Triggers and bumpers, behind the body.
  auto trigger = [&](float x0, const char* id) {
    dl->PathLineTo(P(x0, 92));
    dl->PathLineTo(P(x0, 82));
    dl->PathBezierQuadraticCurveTo(P(x0, 68), P(x0 + 16, 68));
    dl->PathLineTo(P(x0 + 44, 68));
    dl->PathBezierQuadraticCurveTo(P(x0 + 60, 68), P(x0 + 60, 82));
    dl->PathLineTo(P(x0 + 60, 92));
    dl->PathFillConvex(part);
    dl->PathLineTo(P(x0, 92));
    dl->PathLineTo(P(x0, 82));
    dl->PathBezierQuadraticCurveTo(P(x0, 68), P(x0 + 16, 68));
    dl->PathLineTo(P(x0 + 44, 68));
    dl->PathBezierQuadraticCurveTo(P(x0 + 60, 68), P(x0 + 60, 82));
    dl->PathLineTo(P(x0 + 60, 92));
    dl->PathStroke(on(id) ? accent : edge, ImDrawFlags_Closed, on(id) ? accent_width : edge_width);
  };
  trigger(228, "lt");
  trigger(472, "rt");
  auto bumper = [&](float x0, const char* id) {
    dl->AddRectFilled(P(x0, 93), P(x0 + 86, 109), part, 8 * k);
    dl->AddRect(P(x0, 93), P(x0 + 86, 109), on(id) ? accent : edge, 8 * k, 0,
                on(id) ? accent_width : edge_width);
  };
  bumper(214, "lb");
  bumper(460, "rb");

  // The body.
  auto body_path = [&] {
    dl->PathLineTo(P(250, 108));
    dl->PathBezierCubicCurveTo(P(300, 96), P(460, 96), P(510, 108));
    dl->PathBezierCubicCurveTo(P(560, 120), P(592, 150), P(602, 200));
    dl->PathBezierCubicCurveTo(P(616, 268), P(626, 330), P(592, 346));
    dl->PathBezierCubicCurveTo(P(560, 360), P(534, 322), P(510, 292));
    dl->PathBezierCubicCurveTo(P(492, 270), P(472, 262), P(452, 262));
    dl->PathLineTo(P(308, 262));
    dl->PathBezierCubicCurveTo(P(288, 262), P(268, 270), P(250, 292));
    dl->PathBezierCubicCurveTo(P(226, 322), P(200, 360), P(168, 346));
    dl->PathBezierCubicCurveTo(P(134, 330), P(144, 268), P(158, 200));
    dl->PathBezierCubicCurveTo(P(168, 150), P(200, 120), P(250, 108));
  };
  body_path();
  dl->PathFillConcave(body);
  body_path();
  dl->PathStroke(edge, ImDrawFlags_Closed, std::max(1.5f, 3.0f * k));

  // Sticks and the D-pad in their wells.
  auto stick = [&](float x, float y, float r_well, float r_cap, const char* id) {
    dl->AddCircleFilled(P(x, y), r_well * k, well);
    dl->AddCircle(P(x, y), r_well * k, edge, 0, edge_width);
    dl->AddCircleFilled(P(x, y), r_cap * k, part);
    dl->AddCircle(P(x, y), r_cap * k, on(id) ? accent : edge, 0,
                  on(id) ? accent_width : edge_width);
  };
  stick(290, 168, 27, 18, "ls");
  stick(432, 228, 26, 17, "rs");
  dl->AddCircleFilled(P(328, 228), 27 * k, well);
  dl->AddCircle(P(328, 228), 27 * k, edge, 0, edge_width);
  const ImU32 cross_edge = on("dp") ? accent : edge;
  const float cross_width = on("dp") ? accent_width : edge_width;
  dl->AddRectFilled(P(321, 207), P(335, 249), part, 2 * k);
  dl->AddRectFilled(P(307, 221), P(349, 235), part, 2 * k);
  dl->AddRect(P(321, 207), P(335, 249), cross_edge, 2 * k, 0, cross_width);
  dl->AddRect(P(307, 221), P(349, 235), cross_edge, 2 * k, 0, cross_width);
  dl->AddRectFilled(P(322, 222), P(334, 234), part);

  // Back, Start, Guide.
  auto small_button = [&](float x0, const char* id) {
    dl->AddRectFilled(P(x0, 162), P(x0 + 20, 174), part, 6 * k);
    dl->AddRect(P(x0, 162), P(x0 + 20, 174), on(id) ? accent : edge, 6 * k, 0,
                on(id) ? accent_width : edge_width);
  };
  small_button(342, "back");
  small_button(398, "start");
  dl->AddCircleFilled(P(380, 138), 15 * k, part);
  dl->AddCircle(P(380, 138), 15 * k, on("guide") ? accent : edge, 0,
                on("guide") ? accent_width : edge_width);
  dl->AddCircle(P(380, 138), 8 * k, t.dim, 0, edge_width);

  // Face buttons in their usual colours, softened towards the theme.
  struct Face {
    const char* id;
    const char* letter;
    float x, y;
    ImU32 color;
  };
  const Face faces[] = {
      {"y", "Y", 470, 144, IM_COL32(0xE3, 0xBE, 0x3C, 0xFF)},
      {"x", "X", 446, 168, IM_COL32(0x3E, 0x7D, 0xD6, 0xFF)},
      {"b", "B", 494, 168, IM_COL32(0xD9, 0x48, 0x3C, 0xFF)},
      {"a", "A", 470, 192, IM_COL32(0x62, 0xB0, 0x4A, 0xFF)},
  };
  for (const Face& face : faces) {
    const ImVec2 c = P(face.x, face.y);
    dl->AddCircleFilled(c, 13 * k, Mix(face.color, body, 0.15f));
    if (on(face.id)) {
      dl->AddCircle(c, 13 * k + accent_width * 0.5f, accent, 0, accent_width);
    }
    if (letter_font) {
      const float size = letter_font->LegacySize;
      const ImVec2 text = letter_font->CalcTextSizeA(size, FLT_MAX, 0.0f, face.letter);
      dl->AddText(letter_font, size, ImVec2(c.x - text.x * 0.5f, c.y - text.y * 0.5f),
                  IM_COL32(0x10, 0x10, 0x10, 0xFF), face.letter);
    }
  }
}

namespace {

std::string Unquote(const ValueText& value) {
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return value;
  }
  std::string out;
  for (size_t i = 1; i + 1 < value.size(); ++i) {
    char c = value[i];
    if (c == '\\' && i + 2 < value.size()) {
      const char next = value[++i];
      switch (next) {
        case 'n':
          c = '\n';
          break;
        case 't':
          c = '\t';
          break;
        case 'r':
          c = '\r';
          break;
        default:
          c = next;  // \" and \\ (key names have nothing else)
          break;
      }
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::vector<std::string> KeyList(const ValueText& value) {
  std::vector<std::string> keys;
  const std::string text = Unquote(value);
  size_t start = 0;
  while (start <= text.size()) {
    size_t comma = text.find(',', start);
    if (comma == std::string::npos) {
      comma = text.size();
    }
    std::string key = text.substr(start, comma - start);
    const size_t first = key.find_first_not_of(" \t");
    const size_t last = key.find_last_not_of(" \t");
    if (first != std::string::npos) {
      keys.push_back(key.substr(first, last - first + 1));
    }
    start = comma + 1;
  }
  return keys;
}

ValueText KeyListValue(const std::vector<std::string>& keys) {
  std::string text;
  for (const std::string& key : keys) {
    if (!text.empty()) {
      text.push_back(',');
    }
    text += key;
  }
  return QuoteString(text);
}

std::string PrettyKey(const std::string& key) {
  // Arrow keys keep their names: the default font has no arrow glyphs.
  static const std::map<std::string, std::string> names = {
      {"Semicolon", ";"}, {"Quote", "'"},    {"Return", "Enter"}, {"Backtick", "`"},
      {"LBracket", "["},  {"RBracket", "]"}, {"Comma", ","},      {"Period", "."},
      {"Slash", "/"},     {"Backslash", "\\"}, {"Minus", "-"},    {"Plus", "="},
      {"Control", "Ctrl"},
  };
  std::string out;
  size_t start = 0;
  while (start <= key.size()) {
    size_t plus = key.find('+', start);
    // A trailing '+' is part of the last name, not a separator.
    if (plus == key.size() - 1) {
      plus = std::string::npos;
    }
    const std::string part =
        key.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
    auto it = names.find(part);
    out += it != names.end() ? it->second : part;
    if (plus == std::string::npos) {
      break;
    }
    out.push_back('+');
    start = plus + 1;
  }
  return out;
}

}  // namespace rex::launcher::bindings
