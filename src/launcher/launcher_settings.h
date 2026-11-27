/**
 * @file        launcher/launcher_settings.h
 * @brief       The title's settings files: read root values, write edits in place
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar_file.h>

#include "launcher_manifest.h"

namespace rex::launcher {

struct SettingsFile {
  std::filesystem::path path;
  std::unordered_map<std::string, ValueText> values;  // root keys, as the loader reads them
  bool exists = false;
  std::string error;  // set when the file can't be read: nothing is written to it
};

class Settings {
 public:
  // Reads settings/<name>.toml for each name.
  void Load(const std::filesystem::path& settings_dir, const std::vector<std::string>& names);

  const SettingsFile* File(const std::string& name) const;
  std::optional<ValueText> Get(const std::string& file, const std::string& key) const;

  // Edits the file in place (rex::cvar::EditConfigText): only these lines
  // change. A file that doesn't exist yet is created.
  bool Write(const std::string& file, const std::vector<rex::cvar::ConfigEdit>& edits,
             std::string& error);

 private:
  std::filesystem::path settings_dir_;
  std::map<std::string, SettingsFile> files_;
};

}  // namespace rex::launcher
