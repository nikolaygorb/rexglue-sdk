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

// The settings file's own lines (name -> value as the file has it), from the
// last LoadConfig or SaveConfig. A value from the command line or the
// environment is a one-off for that run, so the save writes the file's value
// for that flag instead; lines for names nobody registered in this run (a
// plugin not loaded, another program's settings) are written back as they
// were instead of being dropped.
struct ConfigLine {
  std::string value;
  bool is_string = false;  // a TOML string (quoted) in the file
};

std::unordered_map<std::string, ConfigLine>& GetConfigLinesStorage() {
  static std::unordered_map<std::string, ConfigLine> lines;
  return lines;
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

// Recursively apply TOML values
void ApplyTomlTable(const toml::table& table, const std::string& prefix) {
  for (const auto& [key, value] : table) {
    std::string full_key = prefix.empty() ? std::string(key) : prefix + "_" + std::string(key);

    if (value.is_table()) {
      ApplyTomlTable(*value.as_table(), full_key);
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

      {
        std::lock_guard lock(GetRegistryMutex());
        GetConfigLinesStorage()[full_key] = ConfigLine{value_str, value.is_string()};
      }

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

// The settings file's lines: each registered flag that differs from its
// default (a command-line or environment value replaced by the file's own,
// see GetConfigLinesStorage), then, for the whole file only, the file's lines
// for names nobody registered. `written` receives what was serialized.
// Called with the registry lock held.
std::string SerializeLines(const std::string_view* category,
                           std::unordered_map<std::string, ConfigLine>* written) {
  const auto& lines = GetConfigLinesStorage();
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
        auto it = lines.find(entry.name);
        if (it != lines.end() && it->second.value != entry.default_value) {
          add(entry.name, it->second.value, is_string);
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
    std::vector<std::pair<std::string, ConfigLine>> unknown;
    for (const auto& [name, line] : lines) {
      if (GetRegistryIndex().find(name) == GetRegistryIndex().end()) {
        unknown.emplace_back(name, line);
      }
    }
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
  const auto& lines = GetConfigLinesStorage();
  auto it = lines.find(std::string(name));
  if (it == lines.end()) {
    return std::nullopt;
  }
  return it->second.value;
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
      app.add_option_function<std::string>(
          "--" + entry.name,
          [&entry](const std::string& val) { ApplyFromSource(entry, val, Source::kCommandLine); },
          entry.description);
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
    auto config = toml::parse(text, name);
    {
      std::lock_guard lock(GetRegistryMutex());
      GetConfigLinesStorage().clear();  // this file's lines from now on
    }
    ApplyTomlTable(config, "");
    REXLOG_DEBUG("Loaded config from {}", name);
  } catch (const toml::parse_error& err) {
    REXLOG_ERROR("Failed to parse config {}: {}", name, err.what());
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

void SaveConfig(const std::filesystem::path& config_path) {
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

  try {
    // Write to a temporary sibling and rename it over the target, so an
    // interrupted write (crash, power loss, full disk) cannot leave a truncated
    // config behind.
    std::filesystem::path temp_path = config_path;
    temp_path += ".tmp";
    {
      std::ofstream file(temp_path, std::ios::trunc);
      if (!file) {
        REXLOG_ERROR("SaveConfig: failed to open {}", rex::path_to_utf8(temp_path));
        return;
      }
      file << "# Auto-generated cvar configuration\n";
      file << content;
      file.flush();
      if (!file) {
        REXLOG_ERROR("SaveConfig: failed to write {}", rex::path_to_utf8(temp_path));
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        return;
      }
    }
    std::error_code ec;
    std::filesystem::rename(temp_path, config_path, ec);
    if (ec) {
      REXLOG_ERROR("SaveConfig: failed to replace {}: {}", rex::path_to_utf8(config_path),
                   ec.message());
      std::error_code remove_ec;
      std::filesystem::remove(temp_path, remove_ec);
      return;
    }
    {
      // The file now has these lines; a later save in this run starts from them.
      std::lock_guard lock(GetRegistryMutex());
      GetConfigLinesStorage() = std::move(written);
    }
    REXLOG_INFO("Saved config to {}", rex::path_to_utf8(config_path));
  } catch (const std::exception& e) {
    REXLOG_ERROR("SaveConfig: {}", e.what());
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
  GetConfigLinesStorage().clear();
  GetOneOffValuesStorage().clear();
  g_init_done = false;
  g_finalized = false;
}

}  // namespace testing

}  // namespace rex::cvar
