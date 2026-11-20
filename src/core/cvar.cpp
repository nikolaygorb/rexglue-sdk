/**
 * @file        cvar.cpp
 * @brief       Configuration variable system implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/cvar_file.h>
#include <rex/logging.h>
#include <rex/platform/env.h>

#include <toml++/toml.hpp>

namespace rex::cvar {

namespace {

bool g_finalized = false;
bool g_lifecycle_override = false;
std::mutex g_mutex;

// Set once cvar::Init has parsed the command line; later registrations are
// from runtime-loaded modules and drain pending values.
std::atomic<bool> g_init_done = false;

// Recursive: FlagRegistrar chain methods re-enter; change callbacks invoked
// from SetFlagByName must not mutate the registry.
std::recursive_mutex& GetRegistryMutex() {
  static std::recursive_mutex m;
  return m;
}

// Flag registry - use functions to avoid static init order issues
std::vector<FlagEntry>& GetRegistryStorage() {
  static std::vector<FlagEntry> registry;
  return registry;
}

std::unordered_map<std::string, size_t>& GetRegistryIndex() {
  static std::unordered_map<std::string, size_t> index;
  return index;
}

// Values that arrived before their cvar was registered; runtime-loaded
// modules register cvars long after Init/LoadConfig. A runtime value set
// before registration (an app choosing a default for a GPU plugin's cvar
// before the plugin loads) becomes the default: the config, environment and
// command line still override it.
struct PendingValues {
  std::optional<std::string> cmdline;
  std::optional<std::string> config;
  std::optional<std::string> runtime;
};

std::unordered_map<std::string, PendingValues>& GetPendingValuesStorage() {
  static std::unordered_map<std::string, PendingValues> pending;
  return pending;
}

// A settings file's own lines (name -> value as the file has it). A value from
// the command line or the environment is a one-off for that run, so a save
// leaves the file's line for that flag as it is; lines for names nobody
// registered in this run (a plugin not loaded, another program's settings)
// stay in the file.
struct ConfigLine {
  std::string value;
  bool is_string = false;  // a TOML string (quoted) in the file
};

// One settings file read by LoadConfig (or written whole by SaveConfig).
struct ConfigFile {
  std::filesystem::path path;
  std::string key;  // ConfigFileKey(path)
  std::unordered_map<std::string, ConfigLine> lines;
  bool unreadable = false;  // LoadConfig could not parse it
};

// The settings files in the order their values were applied: a file loaded
// again moves to the end. A title loads several (hardware.toml, then
// mapping.toml); a name's line in a later file won over an earlier one.
std::vector<ConfigFile>& GetConfigFilesStorage() {
  static std::vector<ConfigFile> files;
  return files;
}

// The same file reached through different spellings of its path gets one key.
std::string ConfigFileKey(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::path full = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    full = std::filesystem::absolute(path, ec);
    if (ec) {
      full = path;
    }
  }
  std::string key = rex::path_to_utf8(full.lexically_normal());
#ifdef _WIN32
  for (char& c : key) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
#endif
  return key;
}

// The line of the file whose value for `name` was applied last. Called with
// the registry lock held.
const ConfigLine* FindConfigLine(const std::string& name) {
  const auto& files = GetConfigFilesStorage();
  for (auto it = files.rbegin(); it != files.rend(); ++it) {
    auto line = it->lines.find(name);
    if (line != it->lines.end()) {
      return &line->second;
    }
  }
  return nullptr;
}

// Makes `path` the most recently applied settings file with these lines.
void RecordConfigFile(const std::filesystem::path& path,
                      std::unordered_map<std::string, ConfigLine> lines, bool unreadable) {
  std::string key = ConfigFileKey(path);
  std::lock_guard lock(GetRegistryMutex());
  auto& files = GetConfigFilesStorage();
  std::erase_if(files, [&](const ConfigFile& file) { return file.key == key; });
  files.push_back(ConfigFile{path, std::move(key), std::move(lines), unreadable});
}

// The value each flag got from the command line or the environment (in the
// flag's own form). While the flag still has it, the save writes the file's
// line instead; a different value means the app changed it since (also
// through REXCVAR_SET, which leaves the source as it was), and that is saved.
std::unordered_map<std::string, std::string>& GetOneOffValuesStorage() {
  static std::unordered_map<std::string, std::string> values;
  return values;
}

bool IsOneOffSource(Source source) {
  return source == Source::kCommandLine || source == Source::kEnvironment;
}

// Convert flag name to environment variable: gpu_vsync -> REX_GPU_VSYNC
std::string FlagNameToEnvVar(std::string_view name) {
  std::string result = "REX_";
  for (char c : name) {
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

enum class ApplyResult { kApplied, kSkipped, kRejected };

ApplyResult SetFlagFromSource(std::string_view name, std::string_view value, Source source);

bool Outranks(Source source, const FlagEntry& entry) {
  return source >= entry.source;
}

// Unvalidated apply, for the command line and environment paths.
bool ApplyFromSource(FlagEntry& entry, std::string_view value, Source source) {
  if (!Outranks(source, entry) || !entry.setter(value)) {
    return false;
  }
  entry.source = source;
  if (IsOneOffSource(source)) {
    GetOneOffValuesStorage()[entry.name] = entry.getter();
  }
  return true;
}

// Recursively apply TOML values; `lines` receives the file's values by name.
void ApplyTomlTable(const toml::table& table, const std::string& prefix,
                    std::unordered_map<std::string, ConfigLine>& lines) {
  for (const auto& [key, value] : table) {
    std::string full_key = prefix.empty() ? std::string(key) : prefix + "_" + std::string(key);

    if (value.is_table()) {
      ApplyTomlTable(*value.as_table(), full_key, lines);
    } else {
      std::string value_str;
      if (value.is_boolean()) {
        value_str = value.as_boolean()->get() ? "true" : "false";
      } else if (value.is_integer()) {
        value_str = std::to_string(value.as_integer()->get());
      } else if (value.is_floating_point()) {
        value_str = std::to_string(value.as_floating_point()->get());
      } else if (value.is_string()) {
        value_str = value.as_string()->get();
      } else {
        REXLOG_WARN("Config: unsupported type for key '{}'", full_key);
        continue;
      }

      lines[full_key] = ConfigLine{value_str, value.is_string()};

      if (GetFlagInfo(full_key) == nullptr) {
        std::lock_guard lock(GetRegistryMutex());
        GetPendingValuesStorage()[full_key].config = value_str;
        REXLOG_DEBUG("Config: '{}' deferred (cvar not yet registered)", full_key);
        continue;
      }

      switch (SetFlagFromSource(full_key, value_str, Source::kConfig)) {
        case ApplyResult::kApplied:
          REXLOG_DEBUG("Config: {} = {}", full_key, value_str);
          break;
        case ApplyResult::kSkipped:
          REXLOG_DEBUG("Config: {} ignored, already set by a higher-priority source", full_key);
          break;
        case ApplyResult::kRejected:
          REXLOG_WARN("Config: invalid value for cvar '{}'", full_key);
          break;
      }
    }
  }
}

// todo(tomc): move restart manager to Runtime
std::vector<std::string>& GetPendingRestartStorage() {
  static std::vector<std::string> pending;
  return pending;
}

// Callback storage for change notifications
std::unordered_map<std::string, std::vector<ChangeCallback>>& GetCallbackStorage() {
  static std::unordered_map<std::string, std::vector<ChangeCallback>> callbacks;
  return callbacks;
}

void MarkPendingRestart(std::string_view name) {
  auto& pending = GetPendingRestartStorage();
  std::string name_str(name);
  if (std::find(pending.begin(), pending.end(), name_str) == pending.end()) {
    pending.push_back(name_str);
  }
}

bool ValidateConstraints(const FlagEntry& entry, std::string_view value) {
  const auto& c = entry.constraints;

  // Range validation for numeric types
  if (c.HasRangeConstraint()) {
    double numeric_val = 0;
    if (entry.type == FlagType::String || entry.type == FlagType::Boolean) {
      // These types don't have numeric range constraints
    } else if (entry.type == FlagType::Double) {
      if (!ParseDouble(value, numeric_val))
        return false;
    } else {
      // Integer types
      int64_t int_val = 0;
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), int_val);
      if (ec != std::errc())
        return false;
      numeric_val = static_cast<double>(int_val);
    }

    if (c.min.has_value() && numeric_val < *c.min) {
      REXLOG_WARN("Flag '{}': value {} below min ({})", entry.name, value, *c.min);
      return false;
    }
    if (c.max.has_value() && numeric_val > *c.max) {
      REXLOG_WARN("Flag '{}': value {} exceeds max ({})", entry.name, value, *c.max);
      return false;
    }
  }

  // Allowed values validation
  if (c.HasAllowedValues()) {
    bool found = false;
    for (const auto& allowed : c.allowed_values) {
      if (allowed == value) {
        found = true;
        break;
      }
    }
    if (!found) {
      REXLOG_WARN("Flag '{}': '{}' not in allowed values", entry.name, value);
      return false;
    }
  }

  // Custom validator
  if (c.custom_validator && !c.custom_validator(value)) {
    REXLOG_WARN("Flag '{}': custom validation failed for '{}'", entry.name, value);
    return false;
  }

  return true;
}

}  // namespace

//=============================================================================
// Registry
//=============================================================================

std::vector<FlagEntry>& GetRegistry() {
  return GetRegistryStorage();
}

std::optional<size_t> RegisterFlag(FlagEntry entry) {
  std::lock_guard lock(GetRegistryMutex());
  (void)GetCallbackStorage();
  (void)GetPendingRestartStorage();
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  auto it = index.find(entry.name);
  if (it != index.end()) {
    REXLOG_ERROR("cvar: duplicate registration of '{}'; second registration ignored", entry.name);
    return std::nullopt;
  }
  size_t pos = storage.size();
  index[entry.name] = pos;
  storage.push_back(std::move(entry));

  // Late registration: replay pending values in ascending priority.
  if (g_init_done) {
    FlagEntry& stored = storage[pos];
    auto& pending = GetPendingValuesStorage();
    auto pending_it = pending.find(stored.name);
    if (pending_it != pending.end() && pending_it->second.runtime) {
      ApplyFromSource(stored, *pending_it->second.runtime, Source::kDefault);
    }
    if (pending_it != pending.end() && pending_it->second.config) {
      ApplyFromSource(stored, *pending_it->second.config, Source::kConfig);
    }
    auto env_value = rex::platform::env::get(FlagNameToEnvVar(stored.name));
    if (env_value.has_value()) {
      ApplyFromSource(stored, *env_value, Source::kEnvironment);
    }
    if (pending_it != pending.end()) {
      if (pending_it->second.cmdline) {
        ApplyFromSource(stored, *pending_it->second.cmdline, Source::kCommandLine);
      }
      pending.erase(pending_it);
    }
  }
  return pos;
}

void UnregisterFlag(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  std::string key(name);
  auto idx_it = index.find(key);
  if (idx_it == index.end()) {
    return;
  }
  size_t pos = idx_it->second;
  index.erase(idx_it);
  storage.erase(storage.begin() + pos);
  for (auto& [n, i] : index) {
    if (i > pos) {
      --i;
    }
  }
  GetCallbackStorage().erase(key);
  auto& pending = GetPendingRestartStorage();
  pending.erase(std::remove(pending.begin(), pending.end(), key), pending.end());
}

void FlagRegistrar::apply_(std::function<void(FlagEntry&)> fn) {
  if (owned_name_.empty()) {
    return;
  }
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto it = index.find(owned_name_);
  if (it == index.end()) {
    return;
  }
  fn(GetRegistryStorage()[it->second]);
}

namespace {

ApplyResult SetFlagFromSource(std::string_view name, std::string_view value, Source source) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    if (source == Source::kRuntime && g_init_done) {
      GetPendingValuesStorage()[std::string(name)].runtime = std::string(value);
      REXLOG_DEBUG("cvar: '{}' deferred until its module registers it", name);
      return ApplyResult::kApplied;
    }
    return ApplyResult::kRejected;
  }

  auto& entry = GetRegistryStorage()[it->second];

  if (!Outranks(source, entry)) {
    return ApplyResult::kSkipped;
  }

  if (!g_lifecycle_override && entry.lifecycle == Lifecycle::kInitOnly && IsFinalized()) {
    REXLOG_WARN("Cannot modify init-only flag '{}' after initialization", name);
    return ApplyResult::kRejected;
  }

  if (!ValidateConstraints(entry, value)) {
    return ApplyResult::kRejected;
  }

  if (!entry.setter(value)) {
    return ApplyResult::kRejected;
  }
  entry.source = source;
  if (IsOneOffSource(source)) {
    GetOneOffValuesStorage()[entry.name] = entry.getter();
  }

  if (entry.lifecycle == Lifecycle::kRequiresRestart) {
    MarkPendingRestart(name);
  }

  auto& callbacks = GetCallbackStorage();
  auto callback_it = callbacks.find(std::string(name));
  if (callback_it != callbacks.end()) {
    for (const auto& callback : callback_it->second) {
      callback(name, value);
    }
  }

  return ApplyResult::kApplied;
}

}  // namespace

bool SetFlagByName(std::string_view name, std::string_view value) {
  return SetFlagFromSource(name, value, Source::kRuntime) == ApplyResult::kApplied;
}

bool SetFlagFromCommandLine(std::string_view name, std::string_view value) {
  return SetFlagFromSource(name, value, Source::kCommandLine) != ApplyResult::kRejected;
}

bool InvokeCommand(std::string_view name, std::string_view args) {
  std::function<void(std::string_view)> cb;
  {
    std::lock_guard lock(GetRegistryMutex());
    auto it = GetRegistryIndex().find(std::string(name));
    if (it == GetRegistryIndex().end()) {
      return false;
    }
    const auto& entry = GetRegistryStorage()[it->second];
    if (entry.type != FlagType::Command) {
      return false;
    }
    // Copy the callback out from under the lock; GetFlagInfo pointers are
    // invalidated by registry mutation and a command may touch the registry.
    cb = entry.command_callback;
  }
  if (!cb) {
    return false;
  }
  cb(args);
  return true;
}

std::string GetFlagByName(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return "";
  }

  return GetRegistryStorage()[it->second].getter();
}

Source GetFlagSource(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    // Not registered yet: report where the value it will get at registration comes from.
    auto& pending = GetPendingValuesStorage();
    auto pending_it = pending.find(std::string(name));
    if (pending_it != pending.end() && pending_it->second.cmdline) {
      return Source::kCommandLine;
    }
    if (rex::platform::env::get(FlagNameToEnvVar(name)).has_value()) {
      return Source::kEnvironment;
    }
    if (pending_it != pending.end() && pending_it->second.config) {
      return Source::kConfig;
    }
    return Source::kDefault;
  }
  return GetRegistryStorage()[it->second].source;
}

std::vector<std::string> ListFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  result.reserve(GetRegistryStorage().size());
  for (const auto& entry : GetRegistryStorage()) {
    result.push_back(entry.name);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByCategory(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.category == category) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByLifecycle(Lifecycle lc) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.lifecycle == lc) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

const FlagEntry* GetFlagInfo(std::string_view name) {
  // Pointer is invalidated by any subsequent registry call.
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return nullptr;
  }
  return &GetRegistryStorage()[it->second];
}

template <>
bool Query<bool>(std::string_view name) {
  std::string v = GetFlagByName(name);
  return rex::string::from_string<bool>(v, false);
}

template <>
int32_t Query<int32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
int64_t Query<int64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint32_t Query<uint32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint64_t Query<uint64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
double Query<double>(std::string_view name) {
  std::string v = GetFlagByName(name);
  double out = 0.0;
  ParseDouble(v, out);
  return out;
}

template <>
std::string Query<std::string>(std::string_view name) {
  return GetFlagByName(name);
}

std::vector<std::string> GetPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  return GetPendingRestartStorage();
}

void ClearPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  GetPendingRestartStorage().clear();
}

void ResetToDefault(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return;
  }
  auto& entry = GetRegistryStorage()[it->second];
  entry.setter(entry.default_value);
  entry.source = Source::kDefault;
}

void ResetAllToDefaults() {
  std::lock_guard lock(GetRegistryMutex());
  for (auto& entry : GetRegistryStorage()) {
    entry.setter(entry.default_value);
    entry.source = Source::kDefault;
  }
}

bool HasNonDefaultValue(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }
  const auto& entry = GetRegistryStorage()[it->second];
  return entry.getter() != entry.default_value;
}

std::vector<std::string> ListModifiedFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.getter() != entry.default_value) {
      result.push_back(entry.name);
    }
  }
  return result;
}

std::string RedactHomePath(std::string_view value, std::string_view home) {
  const auto is_slash = [](char c) { return c == '/' || c == '\\'; };
  while (!home.empty() && is_slash(home.back())) {
    home.remove_suffix(1);
  }
  if (home.empty()) {
    return std::string(value);
  }
  const auto fold = [](char c) {
    return c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  };
  std::string result;
  result.reserve(value.size());
  size_t pos = 0;
  while (pos < value.size()) {
    const size_t end = pos + home.size();
    if (end <= value.size() && (end == value.size() || is_slash(value[end])) &&
        std::equal(home.begin(), home.end(), value.begin() + pos,
                   [&](char a, char b) { return fold(a) == fold(b); })) {
      result += '~';
      pos = end;
    } else {
      result += value[pos++];
    }
  }
  return result;
}

std::vector<std::string> DescribeFlags(bool modified) {
  auto home = rex::platform::env::get("USERPROFILE");
  if (!home) {
    home = rex::platform::env::get("HOME");
  }
  const auto source_name = [](const FlagEntry& entry, bool entry_modified) -> std::string_view {
    switch (entry.source) {
      case Source::kConfig:
        return "config";
      case Source::kEnvironment:
        return "environment";
      case Source::kCommandLine:
        return "command line";
      case Source::kRuntime:
        return "runtime";
      default:
        // REXCVAR_SET writes the value without going through a source.
        return entry_modified ? "code" : "default";
    }
  };

  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.type == FlagType::Command) {
      continue;
    }
    std::string value = entry.getter();
    const bool entry_modified = value != entry.default_value;
    if (entry_modified != modified) {
      continue;
    }
    if (entry.name.find("gamertag") != std::string::npos) {
      value = "<hidden>";
    } else if (home) {
      value = RedactHomePath(value, *home);
    }
    result.push_back(
        fmt::format("{} = {} ({})", entry.name, value, source_name(entry, entry_modified)));
  }
  std::sort(result.begin(), result.end());
  return result;
}

void LogFlags() {
  const auto modified = DescribeFlags(true);
  REXLOG_INFO("Config: {} setting(s) changed from their defaults", modified.size());
  for (const auto& line : modified) {
    REXLOG_INFO("  {}", line);
  }
  for (const auto& line : DescribeFlags(false)) {
    REXLOG_DEBUG("  {}", line);
  }
}

namespace {

// Quote a string value as a single-line TOML basic string, escaping backslashes,
// quotes and control characters. The default formatter flags would also allow
// literal and multi-line strings, and toml++ 3.4 does not write every multi-line
// string back correctly.
std::string TomlString(const std::string& value) {
  const toml::value<std::string> node{value};
  std::ostringstream out;
  out << toml::toml_formatter{node, toml::format_flags::allow_unicode_strings};
  return out.str();
}

// A whole settings file's lines: each registered flag that differs from its
// default (a command-line or environment value replaced by the settings
// files' own, see ConfigLine), then, for the whole file only, the settings
// files' lines for names nobody registered. `written` receives what was
// serialized.
// Called with the registry lock held.
std::string SerializeLines(const std::string_view* category,
                           std::unordered_map<std::string, ConfigLine>* written) {
  std::string result;
  auto add = [&](const std::string& name, const std::string& value, bool is_string) {
    result += name + " = " + (is_string ? TomlString(value) : value) + "\n";
    if (written) {
      (*written)[name] = ConfigLine{value, is_string};
    }
  };
  for (const auto& entry : GetRegistryStorage()) {
    if (category && entry.category != *category) {
      continue;
    }
    const bool is_string = entry.type == FlagType::String;
    std::string value = entry.getter();
    if (IsOneOffSource(entry.source)) {
      auto one_off = GetOneOffValuesStorage().find(entry.name);
      if (one_off == GetOneOffValuesStorage().end() || one_off->second == value) {
        // Still the run's one-off value (a launcher's flag, a test switch):
        // the file keeps what it had.
        const ConfigLine* line = FindConfigLine(entry.name);
        if (line && line->value != entry.default_value) {
          add(entry.name, line->value, is_string);
        }
        continue;
      }
      // Changed in the app since: saved like any change.
    }
    if (value != entry.default_value) {
      add(entry.name, value, is_string);
    }
  }
  if (!category) {
    // Every settings file's lines for names nobody registered, a later file's
    // line over an earlier one's.
    std::unordered_map<std::string, ConfigLine> unknown_by_name;
    for (const auto& file : GetConfigFilesStorage()) {
      for (const auto& [name, line] : file.lines) {
        if (GetRegistryIndex().find(name) == GetRegistryIndex().end()) {
          unknown_by_name[name] = line;
        }
      }
    }
    std::vector<std::pair<std::string, ConfigLine>> unknown(unknown_by_name.begin(),
                                                            unknown_by_name.end());
    std::sort(unknown.begin(), unknown.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    for (const auto& [name, line] : unknown) {
      add(name, line.value, line.is_string);
    }
  }
  return result;
}

}  // namespace

std::string SerializeToTOML() {
  std::lock_guard lock(GetRegistryMutex());
  return SerializeLines(nullptr, nullptr);
}

std::string SerializeToTOML(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  return SerializeLines(&category, nullptr);
}

bool SetDefaultValue(std::string_view name, std::string_view value) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }
  auto& entry = GetRegistryStorage()[it->second];
  if (entry.type == FlagType::Command || !ValidateConstraints(entry, value)) {
    return false;
  }
  // The setter turns the text into the flag's own form ("1000" -> "1000.000000"
  // for a double), which is what the default is compared with.
  const std::string current = entry.getter();
  if (!entry.setter(value)) {
    return false;
  }
  std::string normalized = entry.getter();
  if (entry.source != Source::kDefault) {
    entry.setter(current);  // something else set it: that value stays
  }
  entry.default_value = std::move(normalized);
  if (entry.source == Source::kDefault && entry.default_value != current) {
    auto& callbacks = GetCallbackStorage();
    if (auto callback_it = callbacks.find(entry.name); callback_it != callbacks.end()) {
      for (const auto& callback : callback_it->second) {
        callback(entry.name, entry.default_value);
      }
    }
  }
  return true;
}

std::optional<std::string> GetConfigFileValue(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  const ConfigLine* line = FindConfigLine(std::string(name));
  if (!line) {
    return std::nullopt;
  }
  return line->value;
}

void RegisterChangeCallback(std::string_view name, ChangeCallback callback) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage()[std::string(name)].push_back(std::move(callback));
}

void UnregisterChangeCallbacks(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage().erase(std::string(name));
}

//=============================================================================
// Initialization
//=============================================================================

std::vector<std::string> Init(int argc, char** argv) {
  // CLI11 rejects an empty "--name=" value with a parse error that drops
  // every other argument, yet an empty string is a meaningful value (an
  // empty android_gpu_driver selects the system driver). Apply those here
  // and hand CLI11 the rest.
  std::vector<char*> args;
  for (int i = 0; i < argc; i++) {
    std::string_view view(argv[i]);
    if (i > 0 && view.starts_with("--") && view.size() > 3 && view.back() == '=' &&
        view.find('=') == view.size() - 1) {
      const std::string name(view.substr(2, view.size() - 3));
      auto& entries = GetRegistryStorage();
      auto entry = std::find_if(entries.begin(), entries.end(),
                                [&](const auto& e) { return e.name == name; });
      if (entry == entries.end()) {
        std::lock_guard lock(GetRegistryMutex());
        GetPendingValuesStorage()[name].cmdline = std::string();
      } else if (entry->type != FlagType::Boolean) {
        ApplyFromSource(*entry, "", Source::kCommandLine);
      }
      continue;
    }
    args.push_back(argv[i]);
  }
  argc = int(args.size());
  argv = args.data();

  CLI::App app{"", ""};
  app.allow_extras();

  for (auto& entry : GetRegistryStorage()) {
    if (entry.type == FlagType::Boolean) {
      app.add_flag_function(
          "--" + entry.name + ",!--no-" + entry.name,
          [&entry](int64_t count) {
            ApplyFromSource(entry, count > 0 ? "true" : "false", Source::kCommandLine);
          },
          entry.description);
    } else {
      // A switch given twice is legal and the last one wins. CLI11's default
      // policy (Throw) made it throw in the callback stage, which skipped every
      // flag registered after it (wwe2k14-recomp full/117).
      app.add_option_function<std::string>(
             "--" + entry.name,
             [&entry](const std::string& val) {
               ApplyFromSource(entry, val, Source::kCommandLine);
             },
             entry.description)
          ->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);
    }
  }

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    // TODO(tomc): dumb workaround for the stupid chicken and its egg.
    //             dont call rex logging funcs here for now.
    fprintf(stderr, "cvar: CLI11  parse error: %s\n", e.what());
  }

  // Stash unrecognized --options for cvars that register later. Supported
  // forms: --name=value, --name (true), --no-name (false); a separated
  // "--name value" pair is ambiguous with a positional, so never consumed.
  std::vector<std::string> positional;
  for (const auto& arg : app.remaining()) {
    std::string_view view(arg);
    if (!view.starts_with("--")) {
      positional.push_back(arg);
      continue;
    }
    view.remove_prefix(2);
    std::string name;
    std::string value = "true";
    if (auto eq = view.find('='); eq != std::string_view::npos) {
      name.assign(view.substr(0, eq));
      value.assign(view.substr(eq + 1));
    } else if (view.starts_with("no-")) {
      name.assign(view.substr(3));
      value = "false";
    } else {
      name.assign(view);
    }
    std::lock_guard lock(GetRegistryMutex());
    GetPendingValuesStorage()[name].cmdline = std::move(value);
  }
  g_init_done = true;
  return positional;
}

namespace {
std::mutex g_config_reports_mutex;
std::vector<ConfigLoadReport> g_config_reports;

// The bare key a `key = value` line sets (empty for anything else: comments,
// table headers, continuation lines). Keys as SaveConfig writes them: letters,
// digits, '_', '-' and '.'.
std::string_view ConfigLineKey(std::string_view line) {
  size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  const size_t start = i;
  while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) ||
                             line[i] == '_' || line[i] == '-' || line[i] == '.')) {
    ++i;
  }
  if (i == start) {
    return {};
  }
  const size_t end = i;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  if (i >= line.size() || line[i] != '=') {
    return {};
  }
  return line.substr(start, end - start);
}
}  // namespace

std::string DropEarlierConfigDuplicates(const std::string& text,
                                        std::vector<std::string>& dropped) {
  std::vector<std::string_view> lines;
  for (size_t pos = 0; pos <= text.size();) {
    const size_t newline = text.find('\n', pos);
    const size_t end = newline == std::string::npos ? text.size() : newline;
    lines.push_back(std::string_view(text).substr(pos, end - pos));
    if (newline == std::string::npos) {
      break;
    }
    pos = newline + 1;
  }
  // "table\nkey" -> its last line. Each [[array]] element is its own table.
  std::unordered_map<std::string, size_t> last;
  std::vector<std::string> keys(lines.size());
  std::string table;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = lines[i];
    const size_t first = line.find_first_not_of(" \t");
    if (first != std::string_view::npos && line[first] == '[') {
      table = std::string(line.substr(first));
      if (line.substr(first, 2) == "[[") {
        table += fmt::format("#{}", i);
      }
      continue;
    }
    const std::string_view key = ConfigLineKey(line);
    if (key.empty()) {
      continue;
    }
    keys[i] = table + "\n" + std::string(key);
    last[keys[i]] = i;
  }
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    if (!keys[i].empty() && last[keys[i]] != i) {
      std::string key = keys[i].substr(keys[i].find('\n') + 1);
      if (std::find(dropped.begin(), dropped.end(), key) == dropped.end()) {
        dropped.push_back(std::move(key));
      }
      continue;
    }
    out.append(lines[i]);
    if (i + 1 < lines.size()) {
      out.push_back('\n');
    }
  }
  return out;
}

namespace {

// Where the value of a `key = value` line that starts at `start` ends: after a
// quoted string, or before a `#` comment outside quotes (arrays and inline
// tables included), trailing spaces excluded. npos for a multi-line string
// whose closing delimiter is on a later line.
size_t ConfigValueEnd(std::string_view line, size_t start) {
  const std::string_view rest = line.substr(start);
  if (rest.starts_with("\"\"\"") || rest.starts_with("'''")) {
    const size_t close = line.find(rest.substr(0, 3), start + 3);
    return close == std::string_view::npos ? std::string_view::npos : close + 3;
  }
  char quote = 0;
  size_t end = line.size();
  for (size_t i = start; i < line.size(); ++i) {
    const char c = line[i];
    if (quote) {
      if (quote == '"' && c == '\\') {
        ++i;
      } else if (c == quote) {
        quote = 0;
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '#') {
      end = i;
      break;
    }
  }
  while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
    --end;
  }
  return end;
}

bool IsConfigTableHeader(std::string_view line) {
  const size_t first = line.find_first_not_of(" \t");
  return first != std::string_view::npos && line[first] == '[';
}

std::string_view WithoutCarriageReturn(std::string_view line) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  return line;
}

}  // namespace

std::string EditConfigText(std::string_view text, const std::vector<ConfigEdit>& edits) {
  const bool crlf = text.find("\r\n") != std::string_view::npos;
  const bool ends_with_newline = !text.empty() && text.back() == '\n';
  std::vector<std::string> lines;
  for (size_t pos = 0; pos < text.size();) {
    const size_t newline = text.find('\n', pos);
    const size_t end = newline == std::string_view::npos ? text.size() : newline;
    lines.emplace_back(text.substr(pos, end - pos));  // a CR stays at the end of its line
    pos = newline == std::string_view::npos ? text.size() : newline + 1;
  }
  size_t root_end = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (IsConfigTableHeader(lines[i])) {
      root_end = i;
      break;
    }
  }

  std::vector<std::pair<std::string, std::string>> added;  // name, value
  for (const ConfigEdit& edit : edits) {
    // The last line of the root table for the name: the one the loader uses.
    size_t found = SIZE_MAX;
    for (size_t i = 0; i < root_end; ++i) {
      if (ConfigLineKey(WithoutCarriageReturn(lines[i])) == edit.name) {
        found = i;
      }
    }
    if (found == SIZE_MAX) {
      auto it = std::find_if(added.begin(), added.end(),
                             [&](const auto& line) { return line.first == edit.name; });
      if (it != added.end()) {
        it->second = edit.value;
      } else {
        added.emplace_back(edit.name, edit.value);
      }
      continue;
    }
    std::string& line = lines[found];
    const bool cr = !line.empty() && line.back() == '\r';
    const std::string_view view = WithoutCarriageReturn(line);
    const size_t eq = view.find('=');
    const size_t value_start = view.find_first_not_of(" \t", eq + 1);
    std::string replaced;
    if (value_start == std::string_view::npos) {
      replaced = std::string(view.substr(0, eq + 1)) + " " + edit.value;
    } else {
      size_t value_end = ConfigValueEnd(view, value_start);
      std::string suffix;
      if (value_end != std::string_view::npos) {
        suffix = std::string(view.substr(value_end));
      } else {
        // A multi-line string: drop its other lines, keep what follows it.
        const std::string delimiter(view.substr(value_start, 3));
        size_t last = found + 1;
        while (last < root_end && lines[last].find(delimiter) == std::string::npos) {
          ++last;
        }
        if (last < root_end) {
          const std::string_view closing = WithoutCarriageReturn(lines[last]);
          suffix = std::string(closing.substr(closing.find(delimiter) + 3));
          ++last;
          lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(found + 1),
                      lines.begin() + static_cast<std::ptrdiff_t>(last));
          root_end -= last - (found + 1);
        }
        // Never closed: only this line is replaced.
      }
      replaced = std::string(view.substr(0, value_start)) + edit.value + suffix;
    }
    if (cr) {
      replaced.push_back('\r');
    }
    lines[found] = std::move(replaced);
  }

  bool added_at_end = false;
  if (!added.empty()) {
    std::vector<std::string> new_lines;
    for (const auto& [name, value] : added) {
      new_lines.push_back(name + " = " + value + (crlf ? "\r" : ""));
    }
    size_t insert_at = lines.size();
    if (root_end < lines.size()) {
      // Before the first table: after the root's last setting.
      insert_at = root_end;
      for (size_t i = root_end; i-- > 0;) {
        if (!ConfigLineKey(WithoutCarriageReturn(lines[i])).empty()) {
          insert_at = i + 1;
          break;
        }
      }
      if (IsConfigTableHeader(lines[insert_at])) {
        new_lines.push_back(crlf ? "\r" : "");
      }
    } else {
      added_at_end = true;
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), new_lines.begin(),
                 new_lines.end());
  }

  std::string out;
  out.reserve(text.size() + 64 * added.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size()) {
      out.push_back('\n');
    }
  }
  if (!lines.empty() && (ends_with_newline || added_at_end)) {
    out.push_back('\n');
  }
  return out;
}

std::vector<ConfigLoadReport> GetConfigLoadReports() {
  std::lock_guard lock(g_config_reports_mutex);
  return g_config_reports;
}

void LoadConfig(const std::filesystem::path& config_path) {
  if (!std::filesystem::exists(config_path)) {
    REXLOG_DEBUG("Config file not found: {}", rex::path_to_utf8(config_path));
    return;
  }

  // Read through the path: path::string() is in the ANSI code page on
  // Windows, which toml++ would read as UTF-8 (or which throws for characters
  // outside it), so a config under a non-ASCII profile folder was skipped.
  const std::string name = rex::path_to_utf8(config_path);
  try {
    std::ifstream file(config_path, std::ios::binary);
    if (!file) {
      REXLOG_ERROR("Failed to open config {}", name);
      return;
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    file.close();
    auto apply = [&](const toml::table& config) {
      std::unordered_map<std::string, ConfigLine> lines;
      ApplyTomlTable(config, "", lines);
      RecordConfigFile(config_path, std::move(lines), /*unreadable=*/false);
    };
    try {
      apply(toml::parse(text, name));
      REXLOG_DEBUG("Loaded config from {}", name);
      return;
    } catch (const toml::parse_error& err) {
      ConfigLoadReport report;
      report.path = config_path;
      // A setting written twice is the usual hand-edit mistake (and an error
      // in TOML): read the file with the last value of each.
      std::vector<std::string> dropped;
      const std::string deduplicated = DropEarlierConfigDuplicates(text, dropped);
      bool recovered = false;
      if (!dropped.empty()) {
        try {
          apply(toml::parse(deduplicated, name));
          recovered = true;
          report.kind = ConfigLoadReport::Kind::kDuplicates;
          report.duplicate_keys = std::move(dropped);
          REXLOG_WARN("Config {}: settings written more than once, the last value used", name);
        } catch (const toml::parse_error&) {
        }
      }
      if (!recovered) {
        // Unreadable: defaults apply, and the next save replaces the file, so
        // keep the player's copy first.
        report.kind = ConfigLoadReport::Kind::kUnreadable;
        report.line = static_cast<int>(err.source().begin.line);
        report.error = std::string(err.description());
        std::filesystem::path bad_copy = config_path;
        bad_copy += ".bad";
        std::error_code copy_ec;
        std::filesystem::copy_file(config_path, bad_copy,
                                   std::filesystem::copy_options::overwrite_existing, copy_ec);
        if (!copy_ec) {
          report.bad_copy = std::move(bad_copy);
        }
        RecordConfigFile(config_path, {}, /*unreadable=*/true);
        REXLOG_ERROR("Failed to parse config {}: {}", name, err.what());
      }
      std::lock_guard lock(g_config_reports_mutex);
      g_config_reports.push_back(std::move(report));
    }
  } catch (const std::exception& e) {
    REXLOG_ERROR("Failed to read config {}: {}", name, e.what());
  }
}

void ApplyEnvironment() {
  int count = 0;
  for (auto& entry : GetRegistryStorage()) {
    std::string env_name = FlagNameToEnvVar(entry.name);
    auto env_value = rex::platform::env::get(env_name);
    if (!env_value.has_value()) {
      continue;
    }
    if (!Outranks(Source::kEnvironment, entry)) {
      REXLOG_DEBUG("Env: {} ignored, already set on the command line", entry.name);
      continue;
    }
    if (ApplyFromSource(entry, *env_value, Source::kEnvironment)) {
      REXLOG_DEBUG("Env: {} = {} (from {})", entry.name, *env_value, env_name);
      ++count;
    } else {
      REXLOG_WARN("Env: failed to parse {} = {}", env_name, *env_value);
    }
  }

  if (count > 0) {
    REXLOG_INFO("Applied {} environment variable override(s)", count);
  }
}

void FinalizeInit() {
  std::lock_guard lock(g_mutex);
  g_finalized = true;
  for (const auto& [name, values] : GetPendingValuesStorage()) {
    (void)values;
    REXLOG_WARN("Config: unknown cvar '{}'", name);
  }
  REXLOG_DEBUG("cvar: initialization finalized");
}

bool IsFinalized() {
  return g_finalized;
}

namespace {

// A value as a save writes it after `=`: a string quoted and escaped, a double
// in its shortest form ("0.3" rather than std::to_string's "0.300000").
std::string FormatConfigValue(const FlagEntry& entry, const std::string& value) {
  if (entry.type == FlagType::String) {
    return TomlString(value);
  }
  if (entry.type == FlagType::Double) {
    double parsed = 0;
    if (ParseDouble(value, parsed)) {
      std::string text = fmt::format("{}", parsed);
      if (text.find_first_of(".eEin") == std::string::npos) {
        text += ".0";  // stays a TOML float
      }
      return text;
    }
  }
  return value;
}

// Whether the file's value (as LoadConfig read it) is the flag's value.
bool SameConfigValue(const FlagEntry& entry, const std::string& file_value,
                     const std::string& value) {
  if (file_value == value) {
    return true;
  }
  if (entry.type == FlagType::Double) {
    double a = 0;
    double b = 0;
    return ParseDouble(file_value, a) && ParseDouble(value, b) && a == b;
  }
  return false;
}

// Writes to a temporary sibling and renames it over `path`, so an interrupted
// write (crash, power loss, full disk) cannot leave a truncated file behind.
// Binary, so the line endings are the content's own.
bool WriteConfigFile(const std::filesystem::path& path, const std::string& content) {
  try {
    if (path.has_parent_path()) {
      // A failure here shows up as the open failure below.
      std::error_code create_ec;
      std::filesystem::create_directories(path.parent_path(), create_ec);
    }
    std::filesystem::path temp_path = path;
    temp_path += ".tmp";
    {
      std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
      if (!file) {
        REXLOG_ERROR("SaveConfig: failed to open {}", rex::path_to_utf8(temp_path));
        return false;
      }
      file << content;
      file.flush();
      if (!file) {
        REXLOG_ERROR("SaveConfig: failed to write {}", rex::path_to_utf8(temp_path));
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        return false;
      }
    }
    std::error_code ec;
    std::filesystem::rename(temp_path, path, ec);
    if (ec) {
      REXLOG_ERROR("SaveConfig: failed to replace {}: {}", rex::path_to_utf8(path), ec.message());
      std::error_code remove_ec;
      std::filesystem::remove(temp_path, remove_ec);
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    REXLOG_ERROR("SaveConfig: {}", e.what());
    return false;
  }
}

// A path no settings file was read from (a new file, an export, a file that
// could not be parsed): written whole, every setting that differs from its
// default.
void SaveWholeConfig(const std::filesystem::path& config_path) {
  std::string content;
  std::unordered_map<std::string, ConfigLine> written;
  {
    std::lock_guard lock(GetRegistryMutex());
    content = SerializeLines(nullptr, &written);
  }
  if (content.empty()) {
    REXLOG_DEBUG("SaveConfig: no modified flags to save");
    return;
  }
  if (!WriteConfigFile(config_path, "# Auto-generated cvar configuration\n" + content)) {
    return;
  }
  // The file now has these lines; a later save in this run edits them.
  RecordConfigFile(config_path, std::move(written), /*unreadable=*/false);
  REXLOG_INFO("Saved config to {}", rex::path_to_utf8(config_path));
}

}  // namespace

void SaveConfig(const std::filesystem::path& config_path) {
  const std::string target_key = ConfigFileKey(config_path);
  struct FileEdits {
    std::filesystem::path path;
    std::string key;
    std::vector<ConfigEdit> edits;
    std::vector<std::pair<std::string, ConfigLine>> lines;
  };
  std::vector<FileEdits> files_edits;
  {
    std::lock_guard lock(GetRegistryMutex());
    const auto& files = GetConfigFilesStorage();
    size_t target = SIZE_MAX;
    for (size_t i = 0; i < files.size(); ++i) {
      if (files[i].key == target_key && !files[i].unreadable) {
        target = i;
      }
    }
    if (target != SIZE_MAX) {
      for (const ConfigFile& file : files) {
        files_edits.push_back(FileEdits{file.path, file.key, {}, {}});
      }
      for (const FlagEntry& entry : GetRegistryStorage()) {
        if (entry.type == FlagType::Command) {
          continue;
        }
        const std::string value = entry.getter();
        if (IsOneOffSource(entry.source)) {
          auto one_off = GetOneOffValuesStorage().find(entry.name);
          if (one_off == GetOneOffValuesStorage().end() || one_off->second == value) {
            continue;  // the run's one-off value: the files keep theirs
          }
        }
        // The file whose line for the flag was applied last gets the change;
        // a flag no file has goes to config_path.
        size_t owner = target;
        bool has_line = false;
        for (size_t i = files.size(); i-- > 0;) {
          auto line = files[i].lines.find(entry.name);
          if (line != files[i].lines.end()) {
            has_line = true;
            owner = SameConfigValue(entry, line->second.value, value) ? SIZE_MAX : i;
            break;
          }
        }
        if (owner == SIZE_MAX || (!has_line && value == entry.default_value)) {
          continue;
        }
        files_edits[owner].edits.push_back(ConfigEdit{entry.name, FormatConfigValue(entry, value)});
        files_edits[owner].lines.emplace_back(
            entry.name, ConfigLine{value, entry.type == FlagType::String});
      }
    }
  }
  if (files_edits.empty()) {
    SaveWholeConfig(config_path);
    return;
  }

  // Each file is edited in place: only the changed lines and the added ones
  // differ, comments, order, other keys and tables stay as they are.
  size_t saved = 0;
  for (FileEdits& file_edits : files_edits) {
    if (file_edits.edits.empty()) {
      continue;
    }
    std::string text;
    {
      std::ifstream file(file_edits.path, std::ios::binary);
      if (file) {
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
      }
    }
    if (!WriteConfigFile(file_edits.path, EditConfigText(text, file_edits.edits))) {
      continue;
    }
    {
      std::lock_guard lock(GetRegistryMutex());
      for (ConfigFile& file : GetConfigFilesStorage()) {
        if (file.key == file_edits.key) {
          for (auto& [name, line] : file_edits.lines) {
            file.lines[name] = std::move(line);
          }
        }
      }
    }
    saved += file_edits.edits.size();
    REXLOG_INFO("Saved {} setting(s) to {}", file_edits.edits.size(),
                rex::path_to_utf8(file_edits.path));
  }
  if (saved == 0) {
    REXLOG_DEBUG("SaveConfig: no changed settings to save");
  }
}

namespace testing {

ScopedLifecycleOverride::ScopedLifecycleOverride() {
  g_lifecycle_override = true;
}

ScopedLifecycleOverride::~ScopedLifecycleOverride() {
  g_lifecycle_override = false;
}

void ResetAllForTesting() {
  ResetAllToDefaults();
  ClearPendingRestartFlags();
  GetPendingValuesStorage().clear();
  GetConfigFilesStorage().clear();
  GetOneOffValuesStorage().clear();
  {
    std::lock_guard lock(g_config_reports_mutex);
    g_config_reports.clear();
  }
  g_init_done = false;
  g_finalized = false;
}

}  // namespace testing

}  // namespace rex::cvar
