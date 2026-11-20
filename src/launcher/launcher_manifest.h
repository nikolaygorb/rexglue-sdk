/**
 * @file        launcher/launcher_manifest.h
 * @brief       What the launcher shows: common.toml (SDK) + the title's launcher.toml
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

namespace rex::launcher {

// A setting value as the settings file writes it after `=`: `true`, `2`,
// `0.5`, `"vulkan"`.
using ValueText = std::string;

// The value text of a TOML boolean, integer, float or string; nullopt for
// anything else (arrays, tables, dates).
std::optional<ValueText> NodeToValueText(const toml::node& node);
// A string as a TOML basic string, quoted and escaped.
ValueText QuoteString(std::string_view value);
// Whether two value texts mean the same value (`2` and `2.0` do).
bool SameValue(const ValueText& a, const ValueText& b);
// The number in a value text, if it is one.
std::optional<double> ValueNumber(const ValueText& value);

// kKeybind: a keyboard / mouse binding (a comma-separated key list, as the
// mouse and keyboard driver reads it), edited on the key bindings screen.
enum class RowKind { kToggle, kChoice, kSelect, kSlider, kButton, kKeybind };

// The page of the key bindings screen; such rows are not on a page of their own.
inline constexpr const char* kBindingsPage = "#bindings";

struct Choice {
  ValueText value;
  std::string label;
  // "", "windows", "linux", or "file:<plugin>" (rexgpu-native: shown only
  // when rexgpu-native.dll / librexgpu-native.so is next to the game).
  std::string only;
};

// `key` has `value` (empty key: always true).
struct Condition {
  std::string key;
  ValueText value;
};

struct Row {
  std::string page;
  std::string key;               // the setting; empty for a button
  std::string file = "hardware";  // settings/<file>.toml
  RowKind kind = RowKind::kToggle;
  std::string label;
  std::string help;
  std::optional<ValueText> player_default;  // what "Reset page" puts back
  bool restart = false;
  bool invert = false;  // toggle: on while the setting is false (wet_disable_*)
  std::vector<Choice> choices;
  double min = 0;
  double max = 1;
  double step = 1;
  int decimals = 0;
  std::string suffix;   // slider: after the number ("×", "°")
  bool integer = false;  // slider: written as an integer
  std::string action;   // button: bindings, open_settings, open_logs, reset_page, reset_all
  std::string button_text;
  // Choices filled in from the system: "displays" (the monitor setting),
  // "display_modes" (the chosen monitor's full-screen sizes).
  std::string source;
  Condition enable_if;  // greyed out unless it holds
  Condition lock_if;    // while it holds: shows lock_value, can't be changed
  ValueText lock_value;
};

struct WordmarkLine {
  std::string text;
  float size = 28.0f;
  float tracking = 0.0f;  // letter spacing, in em
  float indent = 0.0f;    // px from the left edge
  float shear = 0.0f;     // degrees, leaning right (italic look)
  float scale_x = 1.0f;   // horizontal stretch
  bool display_font = true;           // font = "display" (default) or "body"
  std::filesystem::path font_file;    // font = "fonts/<file>.ttf": its own font
  bool same_line = false;             // continues the previous line, bottoms aligned
  std::vector<ImU32> gradient;  // top to bottom; empty: color
  ImU32 color = 0;              // 0: the theme's text colour
  ImU32 outline = 0;
  float outline_width = 0.0f;
  ImU32 shadow = 0;
  ImVec2 shadow_offset{0.0f, 0.0f};
  ImU32 plate = 0;  // a filled band behind the text
  ImVec2 plate_padding{0.0f, 0.0f};
  float rotate = 0.0f;  // degrees, the line with its plate, counter-clockwise
  // A pattern laid over the letters only, in `texture_color`: "concrete"
  // (mottled stone) or "distress" (worn paint: speckles in the colour behind).
  std::string texture;
  ImU32 texture_color = 0;
  float texture_amount = 1.0f;  // 0..1
  float texture_scale = 8.0f;   // px, the size of the pattern's patches
};

struct Theme {
  ImU32 bg = 0;
  ImU32 bg2 = 0;
  ImU32 side = 0;
  ImU32 panel = 0;
  ImU32 panel_hot = 0;
  ImU32 border = 0;
  ImU32 text = 0;
  ImU32 dim = 0;
  ImU32 accent = 0;
  ImU32 accent_hi = 0;
  ImU32 accent_text = 0;
  ImU32 accent2 = 0;
  ImU32 warn = 0;
  ImU32 ok = 0;
  std::filesystem::path body_font;     // empty: the SDK's default
  std::filesystem::path display_font;  // empty: the body font
  std::vector<WordmarkLine> wordmark;  // empty: the title's name
  std::string tagline;                 // small line under the wordmark
};

struct Manifest {
  std::string name;  // the title's name, for the window and the wordmark
  std::vector<std::string> pages;
  std::vector<Row> rows;
  Theme theme;
  std::vector<std::string> required_files;  // relative to the game root; Play needs them
  std::vector<std::string> safe_mode_args;
};

// A neutral dark theme: under every title's, and alone when the manifest
// can't be read.
Theme DefaultTheme();

// Reads `<launcher_dir>/common.toml`, then `<launcher_dir>/launcher.toml`.
// false with `error` set when either can't be read.
bool LoadManifest(const std::filesystem::path& launcher_dir, Manifest& out, std::string& error);

}  // namespace rex::launcher
