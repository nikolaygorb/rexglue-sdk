/**
 * @file        launcher/launcher_widgets.cpp
 * @brief       The launcher's own widgets
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Before any include of imgui.h (imgui_internal.h needs the operators).
#define IMGUI_DEFINE_MATH_OPERATORS

#include "launcher_widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <imgui_internal.h>

// Note: ImGui lives in rexruntime. The context is reached through
// ImGui::GetCurrentContext() only: imgui_internal.h's inline helpers read the
// GImGui global, a data symbol a Windows DLL doesn't hand to the executable.

namespace rex::launcher::widgets {

namespace {

const Theme* g_theme = nullptr;
const Fonts* g_fonts = nullptr;

constexpr float kPi = 3.14159265358979f;

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
  const ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                               x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

struct Item {
  ImGuiWindow* window = nullptr;
  ImGuiID id = 0;
  ImRect bb;
  bool hovered = false;
  bool held = false;
  bool pressed = false;
};

// Lays out an item of `size` at the cursor and runs ImGui's button logic on
// it (mouse, keyboard / gamepad navigation, BeginDisabled).
bool AddItem(const char* str_id, const ImVec2& size, Item& item) {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  ImGuiWindow* window = g.CurrentWindow;
  if (window->SkipItems) {
    return false;
  }
  item.window = window;
  item.id = window->GetID(str_id);
  const ImVec2 pos = window->DC.CursorPos;
  item.bb = ImRect(pos, pos + size);
  ImGui::ItemSize(size);
  if (!ImGui::ItemAdd(item.bb, item.id)) {
    return false;
  }
  item.pressed = ImGui::ButtonBehavior(item.bb, item.id, &item.hovered, &item.held);
  return true;
}

ImFont* BodyFont() {
  return g_fonts && g_fonts->body ? g_fonts->body : ImGui::GetFont();
}

ImVec2 TextSize(ImFont* font, const char* text, const char* text_end = nullptr) {
  return font->CalcTextSizeA(font->LegacySize, FLT_MAX, 0.0f, text, text_end);
}

void TextAt(ImDrawList* draw_list, ImFont* font, ImVec2 pos, ImU32 color, const char* text) {
  draw_list->AddText(font, font->LegacySize, ImVec2(std::floor(pos.x), std::floor(pos.y)), color,
                     text);
}

void TextCentered(ImDrawList* draw_list, ImFont* font, const ImRect& rect, ImU32 color,
                  const char* text) {
  const ImVec2 size = TextSize(font, text);
  TextAt(draw_list, font,
         ImVec2(rect.Min.x + (rect.GetWidth() - size.x) * 0.5f,
                rect.Min.y + (rect.GetHeight() - size.y) * 0.5f),
         color, text);
}

// Recolours vertices [begin, end) from top to bottom through `stops`, keeping
// each vertex's alpha (the glyph's coverage is in the texture).
void ShadeStops(ImDrawList* draw_list, int begin, int end, float top, float bottom,
                const std::vector<ImU32>& stops) {
  if (stops.empty() || end <= begin) {
    return;
  }
  for (int i = begin; i < end; ++i) {
    ImDrawVert& vertex = draw_list->VtxBuffer[i];
    ImU32 color = stops.front();
    if (stops.size() > 1 && bottom > top) {
      const float t = std::clamp((vertex.pos.y - top) / (bottom - top), 0.0f, 1.0f);
      const float f = t * float(stops.size() - 1);
      const size_t k = std::min(size_t(f), stops.size() - 2);
      color = Mix(stops[k], stops[k + 1], f - float(k));
    }
    const ImU32 alpha = std::min((vertex.col >> IM_COL32_A_SHIFT) & 0xFF,
                                 (color >> IM_COL32_A_SHIFT) & 0xFF);
    vertex.col = (color & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
  }
}

// Text with letter spacing, drawn glyph by glyph, each glyph at a whole pixel
// from `pos` (BuildWordmarkTexture places the letters the same way).
float DrawTracked(ImDrawList* draw_list, ImFont* font, ImVec2 pos, ImU32 color,
                  const std::string& text, float tracking, bool draw) {
  const float size = font->LegacySize;
  const char* s = text.c_str();
  const char* end = s + text.size();
  float x = 0.0f;
  bool first = true;
  while (s < end) {
    unsigned int c = 0;
    const int length = ImTextCharFromUtf8(&c, s, end);
    if (length <= 0) {
      break;
    }
    if (!first) {
      x += tracking;
    }
    first = false;
    if (draw) {
      draw_list->AddText(font, size, ImVec2(pos.x + std::floor(x), pos.y), color, s, s + length);
    }
    x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, s, s + length).x;
    s += length;
  }
  return x;
}

// Value noise for the wordmark textures: a hash per lattice point, smoothly
// interpolated, a few octaves summed.
float Hash01(int x, int y, uint32_t seed) {
  uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return float(h & 0xFFFFFF) / 16777216.0f;
}

float ValueNoise(float x, float y, uint32_t seed) {
  const float fx = std::floor(x);
  const float fy = std::floor(y);
  const int ix = int(fx);
  const int iy = int(fy);
  float tx = x - fx;
  float ty = y - fy;
  tx = tx * tx * (3.0f - 2.0f * tx);
  ty = ty * ty * (3.0f - 2.0f * ty);
  const float a = Hash01(ix, iy, seed);
  const float b = Hash01(ix + 1, iy, seed);
  const float c = Hash01(ix, iy + 1, seed);
  const float d = Hash01(ix + 1, iy + 1, seed);
  return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

float Fbm(float x, float y, int octaves, uint32_t seed) {
  float sum = 0.0f;
  float amplitude = 0.5f;
  float total = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    sum += ValueNoise(x, y, seed + uint32_t(i) * 101u) * amplitude;
    total += amplitude;
    x *= 2.0f;
    y *= 2.0f;
    amplitude *= 0.5f;
  }
  return sum / total;
}

// The pattern's strength at a point: `x`, `y` in logical px, `px`, `py` the
// texture's own pixel (for the fine grain).
float Pattern(const std::string& kind, float x, float y, int px, int py, float scale) {
  if (kind == "concrete") {
    // Darker patches and pits in light stone.
    const float n = Fbm(x / scale, y / scale, 4, 7u);
    float a = std::clamp((n - 0.46f) * 2.6f, 0.0f, 1.0f) * 0.8f;
    const float grain = Hash01(px, py, 3u);
    if (grain > 0.92f) {
      a = std::max(a, (grain - 0.92f) * 6.0f);
    }
    return std::clamp(a, 0.0f, 1.0f);
  }
  if (kind == "distress") {
    // Worn paint: blotches and specks where the colour behind shows through.
    const float n = Fbm(x / scale, y / scale, 3, 11u);
    float a = std::clamp((n - 0.6f) / 0.05f, 0.0f, 1.0f);
    if (Hash01(px, py, 5u) > 0.985f) {
      a = 1.0f;
    }
    return a;
  }
  return 0.0f;
}

}  // namespace

void SetStyle(const Theme* theme, const Fonts* fonts) {
  g_theme = theme;
  g_fonts = fonts;
}

ImU32 WithAlpha(ImU32 color, float alpha) {
  const ImU32 a = ImU32(float((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

bool Tab(const char* label, bool selected, float width) {
  Item item;
  if (!AddItem(label, ImVec2(width, 38.0f), item)) {
    return false;
  }
  const Theme& t = *g_theme;
  ImDrawList* draw_list = item.window->DrawList;
  if (selected || item.hovered) {
    draw_list->AddRectFilled(item.bb.Min, item.bb.Max, selected ? t.panel_hot : t.panel, 6.0f);
  }
  if (selected) {
    draw_list->AddRectFilled(ImVec2(item.bb.Min.x, item.bb.Min.y + 9.0f),
                             ImVec2(item.bb.Min.x + 3.0f, item.bb.Max.y - 9.0f), t.accent, 2.0f);
  }
  ImFont* font = BodyFont();
  const ImVec2 size = TextSize(font, label);
  TextAt(draw_list, font,
         ImVec2(item.bb.Min.x + 16.0f, item.bb.Min.y + (item.bb.GetHeight() - size.y) * 0.5f),
         (selected || item.hovered) ? t.text : t.dim, label);
  ImGui::RenderNavCursor(item.bb, item.id);
  return item.pressed;
}

bool Segmented(const char* id, const std::vector<std::string>& labels,
               const std::vector<bool>& enabled, int& selected, bool disabled) {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  ImGuiWindow* window = g.CurrentWindow;
  if (window->SkipItems) {
    return false;
  }
  const Theme& t = *g_theme;
  ImFont* font = BodyFont();
  constexpr float kHeight = 32.0f;
  constexpr float kPad = 3.0f;
  constexpr float kGap = 2.0f;
  std::vector<float> widths;
  float total = kPad * 2.0f;
  for (const std::string& label : labels) {
    widths.push_back(TextSize(font, label.c_str()).x + 26.0f);
    total += widths.back();
  }
  total += kGap * float(labels.empty() ? 0 : labels.size() - 1);

  const ImVec2 origin = window->DC.CursorPos;
  ImDrawList* draw_list = window->DrawList;
  draw_list->AddRectFilled(origin, origin + ImVec2(total, kHeight), t.panel, 7.0f);
  draw_list->AddRect(origin, origin + ImVec2(total, kHeight), t.border, 7.0f);

  bool changed = false;
  ImGui::PushID(id);
  if (disabled) {
    ImGui::BeginDisabled();
  }
  float x = origin.x + kPad;
  for (size_t i = 0; i < labels.size(); ++i) {
    const bool usable = i >= enabled.size() || enabled[i];
    ImGui::SetCursorScreenPos(ImVec2(x, origin.y + kPad));
    if (!usable) {
      ImGui::BeginDisabled();
    }
    ImGui::PushID(int(i));
    Item item;
    if (AddItem("##segment", ImVec2(widths[i], kHeight - kPad * 2.0f), item)) {
      if (item.pressed && usable && selected != int(i)) {
        selected = int(i);
        changed = true;
      }
      const bool on = selected == int(i);
      if (on) {
        draw_list->AddRectFilled(item.bb.Min, item.bb.Max,
                                 disabled ? Mix(t.panel_hot, t.accent, 0.35f) : t.accent, 5.0f);
      } else if (item.hovered) {
        draw_list->AddRectFilled(item.bb.Min, item.bb.Max, t.panel_hot, 5.0f);
      }
      ImU32 color = on ? t.accent_text : (item.hovered ? t.text : t.dim);
      if (!usable || (disabled && !on)) {
        color = WithAlpha(t.dim, 0.45f);
      }
      TextCentered(draw_list, font, item.bb, color, labels[i].c_str());
      ImGui::RenderNavCursor(item.bb, item.id);
    }
    ImGui::PopID();
    if (!usable) {
      ImGui::EndDisabled();
    }
    x += widths[i] + kGap;
  }
  if (disabled) {
    ImGui::EndDisabled();
  }
  ImGui::PopID();
  // One item for the whole control, for the layout.
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(total, kHeight));
  return changed;
}

bool Toggle(const char* id, bool& on, bool disabled) {
  if (disabled) {
    ImGui::BeginDisabled();
  }
  Item item;
  bool changed = false;
  if (AddItem(id, ImVec2(42.0f, 24.0f), item)) {
    if (item.pressed) {
      on = !on;
      changed = true;
    }
    const Theme& t = *g_theme;
    ImDrawList* draw_list = item.window->DrawList;
    const float radius = item.bb.GetHeight() * 0.5f;
    ImU32 track = on ? t.accent : t.panel;
    ImU32 knob = on ? t.accent_text : t.dim;
    if (disabled) {
      track = on ? Mix(t.panel_hot, t.accent, 0.35f) : t.panel;
      knob = WithAlpha(knob, 0.6f);
    } else if (item.hovered && !on) {
      track = t.panel_hot;
    }
    draw_list->AddRectFilled(item.bb.Min, item.bb.Max, track, radius);
    if (!on) {
      draw_list->AddRect(item.bb.Min, item.bb.Max, t.border, radius);
    }
    const float knob_x = on ? item.bb.Max.x - radius : item.bb.Min.x + radius;
    draw_list->AddCircleFilled(ImVec2(knob_x, item.bb.Min.y + radius), radius - 4.0f, knob);
    ImGui::RenderNavCursor(item.bb, item.id);
  }
  if (disabled) {
    ImGui::EndDisabled();
  }
  return changed;
}

bool Slider(const char* id, double& value, double min, double max, double step,
            const std::string& text, bool disabled) {
  if (disabled) {
    ImGui::BeginDisabled();
  }
  const float width = std::clamp(ImGui::GetContentRegionAvail().x - 12.0f, 120.0f, 310.0f);
  const float track_width = width - 80.0f;
  ImFont* font = BodyFont();
  Item item;
  bool changed = false;
  if (AddItem(id, ImVec2(width, 26.0f), item)) {
    const Theme& t = *g_theme;
    ImDrawList* draw_list = item.window->DrawList;
    const ImRect track(ImVec2(item.bb.Min.x + 8.0f, item.bb.GetCenter().y - 2.0f),
                       ImVec2(item.bb.Min.x + track_width - 8.0f, item.bb.GetCenter().y + 2.0f));
    if (item.held && max > min) {
      const float mouse_x = ImGui::GetIO().MousePos.x;
      const double f = std::clamp(double((mouse_x - track.Min.x) / track.GetWidth()), 0.0, 1.0);
      double v = min + f * (max - min);
      if (step > 0.0) {
        v = min + std::round((v - min) / step) * step;
      }
      v = std::clamp(v, min, max);
      if (v != value) {
        value = v;
        changed = true;
      }
    }
    // Arrow keys step the value while the slider has the keyboard focus.
    if (!disabled && ImGui::IsItemFocused() && step > 0.0) {
      double v = value;
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        v += step;
      } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        v -= step;
      }
      v = std::clamp(v, min, max);
      if (v != value) {
        value = v;
        changed = true;
      }
    }
    const float f = max > min ? float(std::clamp((value - min) / (max - min), 0.0, 1.0)) : 0.0f;
    const float knob_x = track.Min.x + track.GetWidth() * f;
    const ImU32 fill = disabled ? Mix(t.panel_hot, t.accent, 0.35f) : t.accent;
    draw_list->AddRectFilled(track.Min, track.Max, t.panel, 2.0f);
    draw_list->AddRectFilled(track.Min, ImVec2(knob_x, track.Max.y), fill, 2.0f);
    const float knob_radius = (item.hovered || item.held) ? 8.5f : 7.5f;
    draw_list->AddCircleFilled(ImVec2(knob_x, track.GetCenter().y), knob_radius,
                               disabled ? t.dim : t.text);
    draw_list->AddCircle(ImVec2(knob_x, track.GetCenter().y), knob_radius, fill, 0, 3.0f);
    const ImVec2 text_size = TextSize(font, text.c_str());
    TextAt(draw_list, font,
           ImVec2(item.bb.Min.x + track_width + 8.0f, item.bb.GetCenter().y - text_size.y * 0.5f),
           disabled ? t.dim : t.text, text.c_str());
    ImGui::RenderNavCursor(item.bb, item.id);
  }
  if (disabled) {
    ImGui::EndDisabled();
  }
  return changed;
}

bool Select(const char* id, const std::vector<std::string>& labels, int& selected, bool disabled) {
  const Theme& t = *g_theme;
  ImFont* font = BodyFont();
  float width = 220.0f;
  for (const std::string& label : labels) {
    width = std::max(width, TextSize(font, label.c_str()).x + 52.0f);
  }
  const float popup_width = std::min(width, ImGui::GetIO().DisplaySize.x - 24.0f);
  width = std::max(80.0f, std::min(width, ImGui::GetContentRegionAvail().x - 12.0f));
  bool changed = false;
  ImGui::PushID(id);
  if (disabled) {
    ImGui::BeginDisabled();
  }
  Item item;
  if (AddItem("##select", ImVec2(width, 32.0f), item)) {
    ImDrawList* draw_list = item.window->DrawList;
    draw_list->AddRectFilled(item.bb.Min, item.bb.Max, item.hovered ? t.panel_hot : t.panel, 6.0f);
    draw_list->AddRect(item.bb.Min, item.bb.Max, t.border, 6.0f);
    const char* text = (selected >= 0 && selected < int(labels.size())) ? labels[selected].c_str()
                                                                          : "Custom";
    const ImVec2 text_size = TextSize(font, text);
    draw_list->PushClipRect(item.bb.Min, ImVec2(item.bb.Max.x - 32.0f, item.bb.Max.y), true);
    TextAt(draw_list, font,
           ImVec2(item.bb.Min.x + 12.0f, item.bb.GetCenter().y - text_size.y * 0.5f),
           disabled ? t.dim : t.text, text);
    draw_list->PopClipRect();
    if (item.hovered && text_size.x > width - 44.0f) {
      ImGui::SetTooltip("%s", text);
    }
    const ImVec2 c(item.bb.Max.x - 16.0f, item.bb.GetCenter().y);
    draw_list->AddTriangleFilled(ImVec2(c.x - 5.0f, c.y - 2.5f), ImVec2(c.x + 5.0f, c.y - 2.5f),
                                 ImVec2(c.x, c.y + 3.5f), t.dim);
    ImGui::RenderNavCursor(item.bb, item.id);
    if (item.pressed) {
      ImGui::OpenPopup("##list");
    }
    ImGui::SetNextWindowPos(ImVec2(item.bb.Min.x, item.bb.Max.y + 4.0f));
    ImGui::SetNextWindowSize(ImVec2(popup_width, 0.0f));
    if (ImGui::BeginPopup("##list")) {
      for (size_t i = 0; i < labels.size(); ++i) {
        ImGui::PushID(int(i));
        Item option;
        if (AddItem("##option", ImVec2(popup_width - 12.0f, 30.0f), option)) {
          ImDrawList* popup_list = option.window->DrawList;
          const bool on = selected == int(i);
          if (on || option.hovered) {
            popup_list->AddRectFilled(option.bb.Min, option.bb.Max,
                                      on ? t.accent : t.panel_hot, 5.0f);
          }
          const ImVec2 option_size = TextSize(font, labels[i].c_str());
          TextAt(popup_list, font,
                 ImVec2(option.bb.Min.x + 10.0f, option.bb.GetCenter().y - option_size.y * 0.5f),
                 on ? t.accent_text : t.text, labels[i].c_str());
          ImGui::RenderNavCursor(option.bb, option.id);
          if (option.pressed) {
            if (selected != int(i)) {
              selected = int(i);
              changed = true;
            }
            ImGui::CloseCurrentPopup();
          }
        }
        ImGui::PopID();
      }
      ImGui::EndPopup();
    }
  }
  if (disabled) {
    ImGui::EndDisabled();
  }
  ImGui::PopID();
  return changed;
}

bool Button(const char* label, ButtonStyle style, bool disabled) {
  const Theme& t = *g_theme;
  ImFont* font = style == ButtonStyle::kPlay && g_fonts && g_fonts->play ? g_fonts->play
                                                                          : BodyFont();
  const ImVec2 text_size = TextSize(font, label);
  const ImVec2 size = style == ButtonStyle::kPlay
                          ? ImVec2(text_size.x + 76.0f, 46.0f)
                          : ImVec2(text_size.x + 34.0f, 38.0f);
  if (disabled) {
    ImGui::BeginDisabled();
  }
  Item item;
  bool pressed = false;
  if (AddItem(label, size, item)) {
    pressed = item.pressed;
    ImDrawList* draw_list = item.window->DrawList;
    ImU32 text_color = t.text;
    switch (style) {
      case ButtonStyle::kGhost:
        if (item.hovered) {
          draw_list->AddRectFilled(item.bb.Min, item.bb.Max, t.panel, 7.0f);
        }
        draw_list->AddRect(item.bb.Min, item.bb.Max, t.border, 7.0f);
        break;
      case ButtonStyle::kPrimary:
        draw_list->AddRectFilled(item.bb.Min, item.bb.Max, item.hovered ? t.accent_hi : t.accent,
                                 7.0f);
        text_color = t.accent_text;
        break;
      case ButtonStyle::kPlay: {
        const int begin = draw_list->VtxBuffer.Size;
        draw_list->AddRectFilled(item.bb.Min, item.bb.Max, t.accent, 8.0f);
        const ImU32 top = item.hovered ? Mix(t.accent_hi, IM_COL32(255, 255, 255, 255), 0.1f)
                                       : t.accent_hi;
        ShadeStops(draw_list, begin, draw_list->VtxBuffer.Size, item.bb.Min.y, item.bb.Max.y,
                   {top, t.accent});
        text_color = t.accent_text;
        break;
      }
    }
    if (disabled) {
      text_color = WithAlpha(text_color, 0.45f);
    }
    TextCentered(draw_list, font, item.bb, text_color, label);
    ImGui::RenderNavCursor(item.bb, item.id);
  }
  if (disabled) {
    ImGui::EndDisabled();
  }
  return pressed;
}

bool Callout(const char* id, ImVec2 pos, ImVec2 size, const char* label, const char* text,
             bool selected) {
  ImGui::SetCursorScreenPos(pos);
  Item item;
  if (!AddItem(id, size, item)) {
    return false;
  }
  const Theme& t = *g_theme;
  ImDrawList* draw_list = item.window->DrawList;
  draw_list->AddRectFilled(item.bb.Min, item.bb.Max, item.hovered ? t.panel_hot : t.panel, 6.0f);
  draw_list->AddRect(item.bb.Min, item.bb.Max, selected ? t.accent : t.border, 6.0f, 0,
                     selected ? 1.5f : 1.0f);
  ImFont* note_font = g_fonts && g_fonts->note_font ? g_fonts->note_font : BodyFont();
  ImFont* body = BodyFont();
  const float top = item.bb.Min.y + (item.bb.GetHeight() - note_font->LegacySize - body->LegacySize) * 0.5f;
  TextAt(draw_list, note_font, ImVec2(item.bb.Min.x + 10.0f, top), t.dim, label);
  TextAt(draw_list, body, ImVec2(item.bb.Min.x + 10.0f, top + note_font->LegacySize), t.text, text);
  ImGui::RenderNavCursor(item.bb, item.id);
  return item.pressed;
}

bool KeyChip(const char* id, const char* text, bool warn) {
  const Theme& t = *g_theme;
  ImFont* font = BodyFont();
  const ImVec2 text_size = TextSize(font, text);
  const ImVec2 size(text_size.x + 36.0f, 28.0f);
  ImGuiContext& g = *ImGui::GetCurrentContext();
  ImGuiWindow* window = g.CurrentWindow;
  if (window->SkipItems) {
    return false;
  }
  const ImVec2 pos = window->DC.CursorPos;
  ImDrawList* draw_list = window->DrawList;
  const ImU32 edge = warn ? t.warn : t.border;
  draw_list->AddRectFilled(pos, pos + size, t.panel_hot, 5.0f);
  draw_list->AddRect(pos, pos + size, edge, 5.0f);
  TextAt(draw_list, font, ImVec2(pos.x + 9.0f, pos.y + (size.y - text_size.y) * 0.5f),
         warn ? t.warn : t.text, text);
  bool pressed = false;
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(ImVec2(pos.x + text_size.x + 14.0f, pos.y + 4.0f));
  Item remove;
  if (AddItem("##remove", ImVec2(18.0f, 20.0f), remove)) {
    if (remove.hovered) {
      draw_list->AddRectFilled(remove.bb.Min, remove.bb.Max, t.panel, 3.0f);
    }
    TextCentered(draw_list, font, remove.bb, remove.hovered ? t.text : t.dim, "\xC3\x97");
    ImGui::RenderNavCursor(remove.bb, remove.id);
    pressed = remove.pressed;
  }
  ImGui::PopID();
  ImGui::SetCursorScreenPos(pos);
  ImGui::Dummy(size);
  return pressed;
}

bool CaptureButton(const char* id, bool capturing) {
  const Theme& t = *g_theme;
  ImFont* font = BodyFont();
  const char* text = capturing ? "Press a key or mouse button\xE2\x80\xA6" : "+ Add";
  const ImVec2 text_size = TextSize(font, text);
  Item item;
  if (!AddItem(id, ImVec2(text_size.x + 22.0f, 28.0f), item)) {
    return false;
  }
  ImDrawList* draw_list = item.window->DrawList;
  if (item.hovered && !capturing) {
    draw_list->AddRectFilled(item.bb.Min, item.bb.Max, t.panel, 5.0f);
  }
  draw_list->AddRect(item.bb.Min, item.bb.Max, capturing ? t.accent : t.border, 5.0f, 0,
                     capturing ? 1.5f : 1.0f);
  TextCentered(draw_list, font, item.bb, capturing ? t.accent : (item.hovered ? t.text : t.dim),
               text);
  ImGui::RenderNavCursor(item.bb, item.id);
  return item.pressed;
}

std::vector<uint8_t> BuildWordmarkTexture(ImFont* font, const WordmarkLine& line, float density,
                                          int& width, int& height, float& margin) {
  std::vector<uint8_t> out;
  if (!font || line.texture.empty() || line.text.empty()) {
    return out;
  }
  unsigned char* atlas = nullptr;
  int atlas_width = 0;
  int atlas_height = 0;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas, &atlas_width, &atlas_height);
  ImFontBaked* baked = font->GetFontBaked(font->LegacySize, density);
  if (!atlas || !baked) {
    return out;
  }
  const float size = font->LegacySize;
  const float tracking = line.tracking * size;
  margin = 2.0f;
  const float text_width = DrawTracked(nullptr, font, ImVec2(0, 0), 0, line.text, tracking, false);
  width = int(std::ceil((text_width + margin * 2.0f) * density));
  height = int(std::ceil((size * 1.3f + margin * 2.0f) * density));
  if (width <= 0 || height <= 0) {
    return out;
  }

  // The letters' coverage, from their pixels in the font atlas, placed as
  // DrawTracked places them.
  std::vector<float> coverage(size_t(width) * size_t(height), 0.0f);
  const char* s = line.text.c_str();
  const char* end = s + line.text.size();
  float x = 0.0f;
  bool first = true;
  while (s < end) {
    unsigned int c = 0;
    const int length = ImTextCharFromUtf8(&c, s, end);
    if (length <= 0) {
      break;
    }
    if (!first) {
      x += tracking;
    }
    first = false;
    const ImFontGlyph* glyph = baked->FindGlyph(ImWchar(c));
    if (glyph && glyph->Visible) {
      const int u0 = int(glyph->U0 * float(atlas_width) + 0.5f);
      const int v0 = int(glyph->V0 * float(atlas_height) + 0.5f);
      const int u1 = int(glyph->U1 * float(atlas_width) + 0.5f);
      const int v1 = int(glyph->V1 * float(atlas_height) + 0.5f);
      const int ox = int(std::lround((std::floor(x) + glyph->X0 + margin) * density));
      const int oy = int(std::lround((glyph->Y0 + margin) * density));
      for (int sy = v0; sy < v1; ++sy) {
        for (int sx = u0; sx < u1; ++sx) {
          const int px = ox + (sx - u0);
          const int py = oy + (sy - v0);
          if (px < 0 || py < 0 || px >= width || py >= height) {
            continue;
          }
          float& cell = coverage[size_t(py) * size_t(width) + size_t(px)];
          cell = std::max(cell, float(atlas[(size_t(sy) * size_t(atlas_width) + size_t(sx)) * 4 + 3]) /
                                    255.0f);
        }
      }
    }
    x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, s, s + length).x;
    s += length;
  }

  out.resize(size_t(width) * size_t(height) * 4);
  for (int py = 0; py < height; ++py) {
    for (int px = 0; px < width; ++px) {
      const size_t i = size_t(py) * size_t(width) + size_t(px);
      const float pattern =
          Pattern(line.texture, float(px) / density, float(py) / density, px, py, line.texture_scale);
      const float alpha = coverage[i] * pattern * line.texture_amount;
      out[i * 4 + 0] = 255;
      out[i * 4 + 1] = 255;
      out[i * 4 + 2] = 255;
      out[i * 4 + 3] = uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  }
  return out;
}

float Wordmark(ImDrawList* draw_list, ImVec2 origin, const std::vector<WordmarkLine>& lines,
               const std::vector<ImFont*>& fonts, const std::vector<WordmarkOverlay>& overlays,
               ImU32 text_color) {
  float y = origin.y;
  bool has_previous = false;
  float previous_right = 0.0f;
  float previous_bottom = 0.0f;
  for (size_t i = 0; i < lines.size() && i < fonts.size(); ++i) {
    const WordmarkLine& line = lines[i];
    ImFont* font = fonts[i];
    if (!font || line.text.empty()) {
      continue;
    }
    const float size = font->LegacySize;
    const float tracking = line.tracking * size;
    const float width = DrawTracked(draw_list, font, ImVec2(0, 0), 0, line.text, tracking, false);
    const float shear = std::tan(line.shear * kPi / 180.0f);
    const float height = size * 1.04f + line.plate_padding.y * 2.0f;
    // A line on its own row, or after the previous one with the bottoms lined up.
    float line_x = origin.x + line.indent;
    float line_y = y;
    if (line.same_line && has_previous) {
      line_x = previous_right + 8.0f + line.indent;
      line_y = previous_bottom - height;
    }
    // Whole pixels, so the texture overlay lines up with the letters.
    const ImVec2 pos(std::floor(line_x + line.plate_padding.x),
                     std::floor(line_y + line.plate_padding.y));
    const float baseline = pos.y + size * 0.78f;
    const float right = pos.x + width * line.scale_x + std::abs(shear) * size;

    const int plate_begin = draw_list->VtxBuffer.Size;
    if (line.plate) {
      draw_list->AddRectFilled(ImVec2(line_x, line_y),
                               ImVec2(right + line.plate_padding.x, line_y + height), line.plate);
    }
    const int text_begin = draw_list->VtxBuffer.Size;
    if (line.shadow) {
      DrawTracked(draw_list, font, pos + line.shadow_offset, line.shadow, line.text, tracking, true);
    }
    if (line.outline && line.outline_width > 0.0f) {
      const float w = line.outline_width;
      const ImVec2 offsets[] = {{-w, 0},  {w, 0},  {0, -w}, {0, w},
                                {-w, -w}, {w, -w}, {-w, w}, {w, w}};
      for (const ImVec2& offset : offsets) {
        DrawTracked(draw_list, font, pos + offset, line.outline, line.text, tracking, true);
      }
    }
    const int main_begin = draw_list->VtxBuffer.Size;
    DrawTracked(draw_list, font, pos, line.color ? line.color : text_color, line.text, tracking,
                true);
    if (!line.gradient.empty()) {
      ShadeStops(draw_list, main_begin, draw_list->VtxBuffer.Size, pos.y + size * 0.12f,
                 pos.y + size * 0.9f, line.gradient);
    }
    // The texture over the letters, in its colour.
    if (i < overlays.size() && overlays[i].texture) {
      const ImVec2 overlay_min(pos.x - overlays[i].margin, pos.y - overlays[i].margin);
      draw_list->AddImage(overlays[i].texture, overlay_min, overlay_min + overlays[i].size,
                          ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), line.texture_color);
    }
    const int end = draw_list->VtxBuffer.Size;
    // Stretch and lean the text (its shadow and outline too) about its left
    // edge and baseline.
    if (line.scale_x != 1.0f || shear != 0.0f) {
      for (int v = text_begin; v < end; ++v) {
        ImVec2& p = draw_list->VtxBuffer[v].pos;
        p.x = pos.x + (p.x - pos.x) * line.scale_x + (baseline - p.y) * shear;
      }
    }
    // Turn the line with its plate about the plate's left middle.
    float rotated_extra = 0.0f;
    if (line.rotate != 0.0f) {
      const float a = -line.rotate * kPi / 180.0f;
      const float c = std::cos(a);
      const float s = std::sin(a);
      const ImVec2 pivot(line_x, line_y + height * 0.5f);
      for (int v = plate_begin; v < end; ++v) {
        ImVec2& p = draw_list->VtxBuffer[v].pos;
        const ImVec2 d = p - pivot;
        p = ImVec2(pivot.x + d.x * c - d.y * s, pivot.y + d.x * s + d.y * c);
      }
      rotated_extra = std::abs(std::sin(a)) * (right - line_x) * 0.5f;
    }
    has_previous = true;
    previous_right = right + line.plate_padding.x;
    previous_bottom = line_y + height;
    y = std::max(y, line_y + height + rotated_extra);
  }
  return y - origin.y;
}

}  // namespace rex::launcher::widgets
