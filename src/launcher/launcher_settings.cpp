/**
 * @file        launcher/launcher_settings.cpp
 * @brief       The title's settings files: read root values, write edits in place
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "launcher_settings.h"

#include <fstream>
#include <iterator>
#include <system_error>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>

namespace rex::launcher {

namespace {

std::optional<std::string> ReadText(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Temporary file and rename, so an interrupted write can't leave half a file.
bool WriteText(const std::filesystem::path& path, const std::string& text, std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) {
      error = fmt::format("Can't write {}", rex::path_to_utf8(temp));
      return false;
    }
    file << text;
    file.flush();
    if (!file) {
      error = fmt::format("Can't write {}", rex::path_to_utf8(temp));
      std::filesystem::remove(temp, ec);
      return false;
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    error = fmt::format("Can't replace {}: {}", rex::path_to_utf8(path), ec.message());
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
}

}  // namespace

void Settings::Load(const std::filesystem::path& settings_dir,
                    const std::vector<std::string>& names) {
  settings_dir_ = settings_dir;
  files_.clear();
  for (const std::string& name : names) {
    SettingsFile file;
    file.path = settings_dir / rex::to_path(name + ".toml");
    const auto text = ReadText(file.path);
    if (text) {
      file.exists = true;
      // As the game reads it: the last line of a repeated key wins.
      std::vector<std::string> dropped;
      const std::string deduplicated = rex::cvar::DropEarlierConfigDuplicates(*text, dropped);
      try {
        const toml::table root = toml::parse(deduplicated, rex::path_to_utf8(file.path));
        for (const auto& [key, node] : root) {
          if (auto value = NodeToValueText(node)) {
            file.values.emplace(std::string(key.str()), std::move(*value));
          }
        }
      } catch (const toml::parse_error& e) {
        file.error = fmt::format("settings/{}.toml can't be read (line {}: {})", name,
                                 e.source().begin.line, e.description());
      }
    }
    files_.emplace(name, std::move(file));
  }
}

const SettingsFile* Settings::File(const std::string& name) const {
  auto it = files_.find(name);
  return it == files_.end() ? nullptr : &it->second;
}

std::optional<ValueText> Settings::Get(const std::string& file, const std::string& key) const {
  const SettingsFile* settings_file = File(file);
  if (!settings_file) {
    return std::nullopt;
  }
  auto it = settings_file->values.find(key);
  if (it == settings_file->values.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool Settings::Write(const std::string& file, const std::vector<rex::cvar::ConfigEdit>& edits,
                     std::string& error) {
  auto it = files_.find(file);
  if (it == files_.end()) {
    SettingsFile settings_file;
    settings_file.path = settings_dir_ / rex::to_path(file + ".toml");
    it = files_.emplace(file, std::move(settings_file)).first;
  }
  SettingsFile& settings_file = it->second;
  if (!settings_file.error.empty()) {
    error = settings_file.error;
    return false;
  }
  // From the disk again: an edit made by hand while the launcher was open stays.
  const std::string text = ReadText(settings_file.path).value_or(std::string{});
  if (!WriteText(settings_file.path, rex::cvar::EditConfigText(text, edits), error)) {
    return false;
  }
  settings_file.exists = true;
  for (const auto& edit : edits) {
    settings_file.values[edit.name] = edit.value;
  }
  return true;
}

}  // namespace rex::launcher
