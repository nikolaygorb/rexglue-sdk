// Copyright (c) 2026 Tom Clay. BSD 3-Clause; see LICENSE.
// Adapted from rexglue/rexglue-sdk 84e0e45d for this fork's config schema.
#include "config_keys.h"

#include <algorithm>
#include <array>
#include <span>

#include <fmt/format.h>

namespace rex::codegen {

namespace {

enum class Kind { kString, kInteger, kBoolean, kArray, kTable, kIntegerOrArray };

struct KeySpec {
  std::string_view name;
  Kind kind;
};

constexpr std::array kProjectKeys = {
    KeySpec{"name", Kind::kString},
    KeySpec{"sdk_version", Kind::kString},
    KeySpec{"game_root", Kind::kString},
};

constexpr std::array kBinaryKeys = {
    KeySpec{"includes", Kind::kArray},
    KeySpec{"guest_path", Kind::kString},
    KeySpec{"project_name", Kind::kString},
    KeySpec{"file_path", Kind::kString},
    KeySpec{"out_directory_path", Kind::kString},
    KeySpec{"template_dir", Kind::kString},
    KeySpec{"longjmp_address", Kind::kIntegerOrArray},
    KeySpec{"setjmp_address", Kind::kIntegerOrArray},
    KeySpec{"skip_lr", Kind::kBoolean},
    KeySpec{"skip_msr", Kind::kBoolean},
    KeySpec{"lock_free_reservations", Kind::kBoolean},
    KeySpec{"fast_dot_products", Kind::kBoolean},
    KeySpec{"non_volatile_gpr_as_local", Kind::kBoolean},
    KeySpec{"non_argument_gpr_as_local", Kind::kBoolean},
    KeySpec{"auto_share_registers", Kind::kBoolean},
    KeySpec{"fpu_flush_denormals", Kind::kBoolean},
    KeySpec{"fpscr_result_bits", Kind::kBoolean},
    KeySpec{"single_denormal_operand_quirk", Kind::kBoolean},
    KeySpec{"ctr_as_local", Kind::kBoolean},
    KeySpec{"xer_as_local", Kind::kBoolean},
    KeySpec{"reserved_as_local", Kind::kBoolean},
    KeySpec{"cr_as_local", Kind::kBoolean},
    KeySpec{"non_argument_as_local", Kind::kBoolean},
    KeySpec{"non_volatile_as_local", Kind::kBoolean},
    KeySpec{"generate_exception_handlers", Kind::kBoolean},
    KeySpec{"is_dll", Kind::kBoolean},
    KeySpec{"analysis", Kind::kTable},
    KeySpec{"rexcrt", Kind::kTable},
    KeySpec{"functions", Kind::kTable},
    KeySpec{"invalid_instructions", Kind::kArray},
    KeySpec{"switch_tables", Kind::kArray},
    KeySpec{"midasm_hook", Kind::kArray},
    KeySpec{"indirect_calls", Kind::kArray},
};

constexpr std::array kAnalysisKeys = {
    KeySpec{"max_jump_extension", Kind::kInteger},
    KeySpec{"data_region_threshold", Kind::kInteger},
    KeySpec{"large_function_threshold", Kind::kInteger},
    KeySpec{"exception_handler_funcs", Kind::kArray},
};

constexpr std::array kFunctionKeys = {
    KeySpec{"size", Kind::kInteger},
    KeySpec{"end", Kind::kInteger},
    KeySpec{"name", Kind::kString},
    KeySpec{"parent", Kind::kInteger},
    KeySpec{"share_registers", Kind::kBoolean},
};

constexpr std::array kInvalidInstructionKeys = {
    KeySpec{"data", Kind::kInteger},
    KeySpec{"size", Kind::kInteger},
};

constexpr std::array kSwitchTableKeys = {
    KeySpec{"address", Kind::kInteger},
    KeySpec{"register", Kind::kInteger},
    KeySpec{"labels", Kind::kArray},
};

constexpr std::array kMidAsmHookKeys = {
    KeySpec{"address", Kind::kInteger},
    KeySpec{"name", Kind::kString},
    KeySpec{"registers", Kind::kArray},
    KeySpec{"return", Kind::kBoolean},
    KeySpec{"return_on_true", Kind::kBoolean},
    KeySpec{"return_on_false", Kind::kBoolean},
    KeySpec{"jump_address", Kind::kInteger},
    KeySpec{"jump_address_on_true", Kind::kInteger},
    KeySpec{"jump_address_on_false", Kind::kInteger},
    KeySpec{"after_instruction", Kind::kBoolean},
};

std::string_view KindName(Kind kind) {
  switch (kind) {
    case Kind::kString:
      return "a string";
    case Kind::kInteger:
      return "an integer";
    case Kind::kBoolean:
      return "a boolean";
    case Kind::kArray:
      return "an array";
    case Kind::kTable:
      return "a table";
    case Kind::kIntegerOrArray:
      return "an integer or an array of integers";
  }
  return "";
}

bool Matches(const toml::node& node, Kind kind) {
  switch (kind) {
    case Kind::kString:
      return node.is_string();
    case Kind::kInteger:
      return node.is_integer();
    case Kind::kBoolean:
      return node.is_boolean();
    case Kind::kArray:
      return node.is_array();
    case Kind::kTable:
      return node.is_table();
    case Kind::kIntegerOrArray:
      return node.is_integer() || node.is_array();
  }
  return false;
}

std::string Join(std::string_view path, std::string_view key) {
  return path.empty() ? std::string(key) : fmt::format("{}.{}", path, key);
}

/**
 * Checks the keys of 1 table against the keys the loader reads.
 *
 * @param tbl The table to check.
 * @param specs The keys the loader reads, with their types.
 * @param path The key path of the table, empty for the root.
 * @param source The start of every warning.
 * @param warnings Receives the warnings.
 */
void CheckTable(const toml::table& tbl, std::span<const KeySpec> specs, std::string_view path,
                std::string_view source, std::vector<std::string>& warnings) {
  for (const auto& [key, node] : tbl) {
    const std::string name(key.str());
    const auto spec = std::ranges::find(specs, name, &KeySpec::name);
    if (spec == specs.end()) {
      warnings.push_back(fmt::format("{}: unknown key '{}'", source, Join(path, name)));
    } else if (!Matches(node, spec->kind)) {
      warnings.push_back(
          fmt::format("{}: key '{}' should be {}", source, Join(path, name), KindName(spec->kind)));
    }
  }
}

void CheckEntries(const toml::node* node, std::span<const KeySpec> specs, std::string_view path,
                  std::string_view source, std::vector<std::string>& warnings) {
  const auto* entries = node ? node->as_array() : nullptr;
  if (!entries) {
    return;
  }
  size_t index = 0;
  for (const auto& entry : *entries) {
    if (const auto* tbl = entry.as_table()) {
      CheckTable(*tbl, specs, fmt::format("{}[{}]", path, index), source, warnings);
    } else {
      warnings.push_back(fmt::format("{}: key '{}[{}]' should be a table", source, path, index));
    }
    ++index;
  }
}

}  // namespace

void CheckConfigKeys(const toml::table& root, std::string_view source,
                     std::vector<std::string>& warnings) {
  CheckTable(root, kBinaryKeys, "", source, warnings);

  if (const auto* analysis = root["analysis"].as_table()) {
    CheckTable(*analysis, kAnalysisKeys, "analysis", source, warnings);
  }
  if (const auto* shim = root["rexcrt"].as_table()) {
    for (const auto& [name, node] : *shim) {
      if (!node.is_integer()) {
        warnings.push_back(fmt::format("{}: key 'rexcrt.{}' should be an integer", source,
                                       std::string(name.str())));
      }
    }
  }
  if (const auto* functions = root["functions"].as_table()) {
    for (const auto& [address, node] : *functions) {
      if (const auto* entry = node.as_table()) {
        CheckTable(*entry, kFunctionKeys, Join("functions", address.str()), source, warnings);
      } else {
        warnings.push_back(fmt::format("{}: key '{}' should be a table", source,
                                       Join("functions", address.str())));
      }
    }
  }
  CheckEntries(root.get("invalid_instructions"), kInvalidInstructionKeys, "invalid_instructions",
               source, warnings);
  CheckEntries(root.get("switch_tables"), kSwitchTableKeys, "switch_tables", source, warnings);
  CheckEntries(root.get("midasm_hook"), kMidAsmHookKeys, "midasm_hook", source, warnings);
}

void CheckProjectKeys(const toml::table& project, std::string_view source,
                      std::vector<std::string>& warnings) {
  CheckTable(project, kProjectKeys, "", source, warnings);
}

}  // namespace rex::codegen
