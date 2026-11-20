/**
 * @file        launcher/launcher_manifest.cpp
 * @brief       Reads common.toml and the title's launcher.toml
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "launcher_manifest.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>

#include <fmt/format.h>

#include <rex/filesystem.h>

namespace rex::launcher {

std::optional<ValueText> NodeToValueText(const toml::node& node) {
  if (auto value = node.as_boolean()) {
    return value->get() ? "true" : "false";
  }
  if (auto value = node.as_integer()) {
    return std::to_string(value->get());
  }
  if (auto value = node.as_floating_point()) {
    std::string text = fmt::format("{}", value->get());
    if (text.find_first_of(".eEin") == std::string::npos) {
      text += ".0";
    }
    return text;
  }
  if (auto value = node.as_string()) {
    return QuoteString(value->get());
  }
  return std::nullopt;
}

ValueText QuoteString(std::string_view value) {
  std::string out = "\"";
  for (const char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
          out += fmt::format("\\u{:04X}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

std::optional<double> ValueNumber(const ValueText& value) {
  if (value.empty() || value.front() == '"') {
    return std::nullopt;
  }
  double number = 0;
  const char* begin = value.data();
  const char* end = begin + value.size();
  if (*begin == '+') {
    ++begin;
  }
  auto [ptr, ec] = std::from_chars(begin, end, number);
  if (ec != std::errc() || ptr != end) {
    return std::nullopt;
  }
  return number;
}

bool SameValue(const ValueText& a, const ValueText& b) {
  if (a == b) {
    return true;
  }
  const auto x = ValueNumber(a);
  const auto y = ValueNumber(b);
  return x && y && *x == *y;
}

namespace {

std::optional<ImU32> ParseColor(std::string_view text) {
  if (text.starts_with('#')) {
    text.remove_prefix(1);
  }
  if (text.size() != 6 && text.size() != 8) {
    return std::nullopt;
  }
  uint32_t v = 0;
  for (const char c : text) {
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = uint32_t(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = uint32_t(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = uint32_t(c - 'A' + 10);
    } else {
      return std::nullopt;
    }
    v = v * 16 + digit;
  }
  if (text.size() == 8) {
    return IM_COL32(v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
  }
  return IM_COL32(v >> 16, (v >> 8) & 0xFF, v & 0xFF, 0xFF);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
  const ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                               x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

float Luminance(ImU32 color) {
  const ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
  return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

bool ReadToml(const std::filesystem::path& path, toml::table& out, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = fmt::format("{} is missing", rex::path_to_utf8(path));
    return false;
  }
  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  try {
    out = toml::parse(text, rex::path_to_utf8(path));
  } catch (const toml::parse_error& e) {
    error = fmt::format("{}, line {}: {}", rex::path_to_utf8(path), e.source().begin.line,
                        e.description());
    return false;
  }
  return true;
}

std::string GetString(const toml::table& t, std::string_view key, std::string fallback = {}) {
  if (auto value = t[key].value<std::string>()) {
    return *value;
  }
  return fallback;
}

double GetNumber(const toml::table& t, std::string_view key, double fallback) {
  if (auto value = t[key].value<double>()) {
    return *value;
  }
  return fallback;
}

std::optional<ImU32> GetColor(const toml::table& t, std::string_view key) {
  if (auto value = t[key].value<std::string>()) {
    return ParseColor(*value);
  }
  return std::nullopt;
}

Condition GetCondition(const toml::table& t, std::string_view key) {
  Condition condition;
  if (const toml::table* c = t[key].as_table()) {
    condition.key = GetString(*c, "key");
    if (const toml::node* value = c->get("value")) {
      condition.value = NodeToValueText(*value).value_or("");
    }
  }
  return condition;
}

bool ParseRow(const toml::table& t, Row& row, std::string& error) {
  row.page = GetString(t, "page");
  row.key = GetString(t, "key");
  row.file = GetString(t, "file", "hardware");
  row.label = GetString(t, "label", row.key);
  row.help = GetString(t, "help");
  row.restart = t["restart"].value_or(false);
  row.invert = t["invert"].value_or(false);
  const std::string kind = GetString(t, "kind", "toggle");
  if (kind == "toggle") {
    row.kind = RowKind::kToggle;
  } else if (kind == "choice") {
    row.kind = RowKind::kChoice;
  } else if (kind == "select") {
    row.kind = RowKind::kSelect;
  } else if (kind == "slider") {
    row.kind = RowKind::kSlider;
  } else if (kind == "button") {
    row.kind = RowKind::kButton;
  } else if (kind == "keybind") {
    row.kind = RowKind::kKeybind;
    if (row.page.empty()) {
      row.page = kBindingsPage;
    }
  } else {
    error = fmt::format("row '{}': unknown kind '{}'", row.label, kind);
    return false;
  }
  if (row.key.empty() && row.kind != RowKind::kButton) {
    error = fmt::format("row '{}' has no key", row.label);
    return false;
  }
  if (const toml::node* value = t.get("default")) {
    row.player_default = NodeToValueText(*value);
  }
  if (const toml::array* choices = t["choices"].as_array()) {
    for (const toml::node& node : *choices) {
      const toml::array* entry = node.as_array();
      if (!entry || entry->size() < 2) {
        error = fmt::format("row '{}': a choice is [value, \"label\"]", row.label);
        return false;
      }
      Choice choice;
      choice.value = NodeToValueText(*entry->get(0)).value_or("");
      choice.label = entry->get(1)->value_or(std::string{});
      if (entry->size() > 2) {
        choice.only = entry->get(2)->value_or(std::string{});
      }
      row.choices.push_back(std::move(choice));
    }
  }
  row.source = GetString(t, "source");
  if ((row.kind == RowKind::kChoice || row.kind == RowKind::kSelect) && row.choices.empty() &&
      row.source.empty()) {
    error = fmt::format("row '{}' has no choices", row.label);
    return false;
  }
  row.min = GetNumber(t, "min", 0.0);
  row.max = GetNumber(t, "max", 1.0);
  row.step = GetNumber(t, "step", 1.0);
  row.decimals = int(GetNumber(t, "decimals", 0.0));
  row.suffix = GetString(t, "suffix");
  row.integer = t["integer"].value_or(false);
  row.action = GetString(t, "action");
  row.button_text = GetString(t, "button", row.label);
  row.enable_if = GetCondition(t, "enable_if");
  row.lock_if = GetCondition(t, "lock_if");
  if (const toml::table* lock = t["lock_if"].as_table()) {
    if (const toml::node* shows = lock->get("shows")) {
      row.lock_value = NodeToValueText(*shows).value_or("");
    }
  }
  return true;
}

bool ParseRows(const toml::table& t, std::vector<Row>& rows, std::string& error) {
  if (const toml::array* list = t["row"].as_array()) {
    for (const toml::node& node : *list) {
      const toml::table* row_table = node.as_table();
      if (!row_table) {
        continue;
      }
      Row row;
      if (!ParseRow(*row_table, row, error)) {
        return false;
      }
      rows.push_back(std::move(row));
    }
  }
  return true;
}

void ParseWordmark(const toml::array& lines, const std::filesystem::path& dir,
                   std::vector<WordmarkLine>& out) {
  out.clear();
  for (const toml::node& node : lines) {
    const toml::table* t = node.as_table();
    if (!t) {
      continue;
    }
    WordmarkLine line;
    line.text = GetString(*t, "text");
    line.size = float(GetNumber(*t, "size", 28.0));
    line.tracking = float(GetNumber(*t, "tracking", 0.0));
    line.indent = float(GetNumber(*t, "indent", 0.0));
    line.shear = float(GetNumber(*t, "shear", 0.0));
    line.scale_x = float(GetNumber(*t, "scale_x", 1.0));
    line.rotate = float(GetNumber(*t, "rotate", 0.0));
    const std::string font = GetString(*t, "font", "display");
    line.display_font = font != "body";
    if (font != "body" && font != "display") {
      line.font_file = dir / rex::to_path(font);
    }
    line.same_line = (*t)["same_line"].value_or(false);
    line.color = GetColor(*t, "color").value_or(0);
    if (const toml::array* gradient = (*t)["gradient"].as_array()) {
      for (const toml::node& stop : *gradient) {
        if (auto color = ParseColor(stop.value_or(std::string{}))) {
          line.gradient.push_back(*color);
        }
      }
    }
    if (const toml::table* outline = (*t)["outline"].as_table()) {
      line.outline = GetColor(*outline, "color").value_or(IM_COL32(0, 0, 0, 255));
      line.outline_width = float(GetNumber(*outline, "width", 1.0));
    }
    if (const toml::table* shadow = (*t)["shadow"].as_table()) {
      line.shadow = GetColor(*shadow, "color").value_or(IM_COL32(0, 0, 0, 255));
      line.shadow_offset =
          ImVec2(float(GetNumber(*shadow, "x", 2.0)), float(GetNumber(*shadow, "y", 2.0)));
    }
    if (const toml::table* texture = (*t)["texture"].as_table()) {
      line.texture = GetString(*texture, "kind", "concrete");
      line.texture_color = GetColor(*texture, "color").value_or(IM_COL32(0, 0, 0, 255));
      line.texture_amount = std::clamp(float(GetNumber(*texture, "amount", 1.0)), 0.0f, 1.0f);
      line.texture_scale = std::max(float(GetNumber(*texture, "scale", 8.0)), 1.0f);
    }
    if (const toml::table* plate = (*t)["plate"].as_table()) {
      line.plate = GetColor(*plate, "color").value_or(IM_COL32(0, 0, 0, 255));
      line.plate_padding =
          ImVec2(float(GetNumber(*plate, "pad_x", 12.0)), float(GetNumber(*plate, "pad_y", 6.0)));
    }
    out.push_back(std::move(line));
  }
}

// Colours given in `t` replace the theme's; fonts are relative to `dir`.
void ParseTheme(const toml::table& t, const std::filesystem::path& dir, Theme& theme) {
  struct Slot {
    const char* key;
    ImU32* color;
  };
  const Slot slots[] = {
      {"bg", &theme.bg},           {"bg2", &theme.bg2},
      {"side", &theme.side},       {"panel", &theme.panel},
      {"panel_hot", &theme.panel_hot}, {"border", &theme.border},
      {"text", &theme.text},       {"dim", &theme.dim},
      {"accent", &theme.accent},   {"accent_hi", &theme.accent_hi},
      {"accent_text", &theme.accent_text}, {"accent2", &theme.accent2},
      {"warn", &theme.warn},       {"ok", &theme.ok},
  };
  for (const Slot& slot : slots) {
    if (auto color = GetColor(t, slot.key)) {
      *slot.color = *color;
    }
  }
  if (auto font = t["body_font"].value<std::string>()) {
    theme.body_font = dir / rex::to_path(*font);
  }
  if (auto font = t["display_font"].value<std::string>()) {
    theme.display_font = dir / rex::to_path(*font);
  }
  if (auto tagline = t["tagline"].value<std::string>()) {
    theme.tagline = *tagline;
  }
  if (const toml::array* wordmark = t["wordmark"].as_array()) {
    ParseWordmark(*wordmark, dir, theme.wordmark);
  }
}

// Fills the colours a theme left out from the ones it has.
void DeriveTheme(const toml::table* given, Theme& theme) {
  auto has = [&](const char* key) { return given && given->contains(key); };
  const ImU32 white = IM_COL32(255, 255, 255, 255);
  const ImU32 black = IM_COL32(0, 0, 0, 255);
  if (!has("bg2")) {
    theme.bg2 = Mix(theme.bg, black, 0.45f);
  }
  if (!has("side")) {
    theme.side = Mix(theme.bg, black, 0.25f);
  }
  if (!has("panel")) {
    theme.panel = Mix(theme.bg, theme.text, 0.08f);
  }
  if (!has("panel_hot")) {
    theme.panel_hot = Mix(theme.bg, theme.text, 0.14f);
  }
  if (!has("border")) {
    theme.border = Mix(theme.bg, theme.text, 0.2f);
  }
  if (!has("dim")) {
    theme.dim = Mix(theme.text, theme.bg, 0.38f);
  }
  if (!has("accent_hi")) {
    theme.accent_hi = Mix(theme.accent, white, 0.18f);
  }
  if (!has("accent_text")) {
    theme.accent_text = Luminance(theme.accent) > 0.5f ? Mix(theme.bg, black, 0.3f) : white;
  }
  if (!has("accent2")) {
    theme.accent2 = theme.accent;
  }
}

}  // namespace

Theme DefaultTheme() {
  Theme theme;
  theme.bg = IM_COL32(0x12, 0x14, 0x18, 0xFF);
  theme.text = IM_COL32(0xE6, 0xE8, 0xEC, 0xFF);
  theme.accent = IM_COL32(0x4C, 0x8D, 0xF6, 0xFF);
  theme.warn = IM_COL32(0xFF, 0xB8, 0x6B, 0xFF);
  theme.ok = IM_COL32(0x7F, 0xE0, 0xA0, 0xFF);
  theme.tagline = "Recompiled";
  DeriveTheme(nullptr, theme);
  return theme;
}

bool LoadManifest(const std::filesystem::path& launcher_dir, Manifest& out, std::string& error) {
  toml::table common;
  toml::table game;
  if (!ReadToml(launcher_dir / "common.toml", common, error) ||
      !ReadToml(launcher_dir / "launcher.toml", game, error)) {
    return false;
  }

  Manifest manifest;
  Theme& theme = manifest.theme;
  theme = DefaultTheme();

  if (const toml::array* pages = common["pages"].as_array()) {
    for (const toml::node& page : *pages) {
      manifest.pages.push_back(page.value_or(std::string{}));
    }
  }
  if (const toml::array* args = common["safe_mode"].as_array()) {
    for (const toml::node& arg : *args) {
      manifest.safe_mode_args.push_back(arg.value_or(std::string{}));
    }
  }
  if (!ParseRows(common, manifest.rows, error)) {
    error = "common.toml: " + error;
    return false;
  }

  const toml::table* game_info = game["game"].as_table();
  manifest.name = game_info ? GetString(*game_info, "name", "Game") : "Game";
  if (game_info) {
    if (const toml::array* files = (*game_info)["required_files"].as_array()) {
      for (const toml::node& file : *files) {
        manifest.required_files.push_back(file.value_or(std::string{}));
      }
    }
    if (const toml::array* args = (*game_info)["safe_mode"].as_array()) {
      manifest.safe_mode_args.clear();
      for (const toml::node& arg : *args) {
        manifest.safe_mode_args.push_back(arg.value_or(std::string{}));
      }
    }
  }

  // Common rows a title doesn't have.
  if (const toml::table* common_options = game["common"].as_table()) {
    if (const toml::array* hide = (*common_options)["hide"].as_array()) {
      for (const toml::node& key : *hide) {
        const std::string name = key.value_or(std::string{});
        std::erase_if(manifest.rows, [&](const Row& row) { return row.key == name; });
      }
    }
  }

  // The title's rows: one with a common row's key replaces it in place.
  std::vector<Row> game_rows;
  if (!ParseRows(game, game_rows, error)) {
    error = "launcher.toml: " + error;
    return false;
  }
  for (Row& row : game_rows) {
    auto same = std::find_if(manifest.rows.begin(), manifest.rows.end(), [&](const Row& common_row) {
      return !row.key.empty() && common_row.key == row.key;
    });
    if (same != manifest.rows.end()) {
      *same = std::move(row);
    } else {
      manifest.rows.push_back(std::move(row));
    }
  }

  if (const toml::table* common_theme = common["theme"].as_table()) {
    ParseTheme(*common_theme, launcher_dir, theme);
  }
  const toml::table* game_theme = game["theme"].as_table();
  if (game_theme) {
    ParseTheme(*game_theme, launcher_dir, theme);
  }
  DeriveTheme(game_theme, theme);

  out = std::move(manifest);
  return true;
}

}  // namespace rex::launcher
