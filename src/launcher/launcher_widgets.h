/**
 * @file        launcher/launcher_widgets.h
 * @brief       The launcher's own widgets: real Dear ImGui items (keyboard
 *              focus, IDs, hover and press come from ImGui), drawn in the
 *              title's theme
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <imgui.h>

#include "launcher_manifest.h"

namespace rex::launcher::widgets {

struct Fonts {
  ImFont* body = nullptr;     // rows, buttons
  ImFont* note_font = nullptr;    // help text, status line
  ImFont* title = nullptr;    // page title (display font)
  ImFont* heading = nullptr;  // help heading (display font)
  ImFont* play = nullptr;     // the Play button (display font)
};

// Every widget draws with these until the next call.
void SetStyle(const Theme* theme, const Fonts* fonts);

ImU32 WithAlpha(ImU32 color, float alpha);

bool Tab(const char* label, bool selected, float width);
// A row of segments; `enabled[i]` false greys one out. true when `selected` changed.
bool Segmented(const char* id, const std::vector<std::string>& labels,
               const std::vector<bool>& enabled, int& selected, bool disabled);
bool Toggle(const char* id, bool& on, bool disabled);
// Drag (or click) along the track; the value snaps to `step`.
bool Slider(const char* id, double& value, double min, double max, double step,
            const std::string& text, bool disabled);
// A button showing the current choice, opening a list. `selected` -1: none.
bool Select(const char* id, const std::vector<std::string>& labels, int& selected, bool disabled);

enum class ButtonStyle { kGhost, kPrimary, kPlay };
bool Button(const char* label, ButtonStyle style, bool disabled = false);

// Key bindings screen: a clickable box with a small label over a line of
// text, at `pos` (screen) of `size`.
bool Callout(const char* id, ImVec2 pos, ImVec2 size, const char* label, const char* text,
             bool selected);
// A bound key; true when its × was pressed. `warn`: the key is on another
// button too.
bool KeyChip(const char* id, const char* text, bool warn);
// "+ Add", or the waiting state while a key is captured.
bool CaptureButton(const char* id, bool capturing);

// A wordmark line's texture over the letters: an RGBA image (white, alpha =
// the pattern inside the letters' shape) for an ImmediateDrawer texture, laid
// over the text from (`margin`, `margin`) up and left of the line's top left.
// Empty when the line has no texture. Needs the built font atlas; `density` is
// the fonts' RasterizerDensity.
std::vector<uint8_t> BuildWordmarkTexture(ImFont* font, const WordmarkLine& line, float density,
                                          int& width, int& height, float& margin);

struct WordmarkOverlay {
  ImTextureID texture{};
  ImVec2 size{0.0f, 0.0f};  // logical px
  float margin = 0.0f;
};

// The title's wordmark from `origin` (top left); `fonts[i]` draws `lines[i]`,
// `overlays[i]` (if any) its texture. Returns the height it took.
float Wordmark(ImDrawList* draw_list, ImVec2 origin, const std::vector<WordmarkLine>& lines,
               const std::vector<ImFont*>& fonts, const std::vector<WordmarkOverlay>& overlays,
               ImU32 text_color);

}  // namespace rex::launcher::widgets
