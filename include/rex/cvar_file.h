/**
 * @file        cvar_file.h
 * @brief       How flags and the settings file meet: app defaults, file values
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *              Ported for the ReXGlue fork from rexglue-sdk PR #524 (cod3 0042).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// What SaveConfig (rex/cvar.h) writes. A value given on the command line or
// in the environment is a one-off for that run, so the files keep their own
// line for that flag (or none); a change made while the app runs
// (SetFlagByName) is saved as usual, over a command-line value too.
//
// - Saving to a settings file LoadConfig read (a title reads several, e.g.
//   settings/hardware.toml and settings/mapping.toml) edits the files in
//   place with EditConfigText: each changed setting goes back to the file
//   whose line for it was applied last, one no file has (and that differs
//   from its default) is added to the file saved to. Comments, blank lines,
//   order, settings that didn't change, lines of flags nobody registered in
//   this run (a plugin not loaded, another program's settings) and tables
//   stay as they are. A setting put back to its default keeps its line, with
//   the default value.
// - Saving to any other path (a new file, an export, a file that could not be
//   parsed and was kept as `.bad`) writes the whole file: every flag that
//   differs from its default, then the settings files' lines for flags nobody
//   registered.
//
// Kept out of rex/cvar.h so that adding these doesn't change a header every
// recompiled game file includes (through rex/chrono/clock.h).

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::cvar {

/// One setting for EditConfigText. `value` is the TOML text that goes after
/// `=`: `true`, `2`, `0.5`, `"vulkan"` (a string quoted and escaped).
struct ConfigEdit {
  std::string name;
  std::string value;
};

/// The text of a flat settings file with the edits applied and everything
/// else kept byte for byte: comments, blank lines, order, other settings,
/// tables. A setting the root table (above the first `[table]` header) has a
/// `name = value` line for gets the new value on that line (on the last one
/// when the name is written more than once: the one the loader uses), keeping
/// the indentation, the spacing around `=` and a trailing comment. A setting
/// the root has no line for is added as `name = value`: at the end of the
/// file, or after the root's last setting when the file has tables. New lines
/// use the file's line ending (CRLF when it has any).
std::string EditConfigText(std::string_view text, const std::vector<ConfigEdit>& edits);

/// Make `value` the default of a registered flag: the value it has while
/// nothing else set it, the value ResetToDefault goes back to, and the value
/// the settings file leaves out. For an app whose defaults differ from the
/// SDK's: the player can then save the SDK's own value (it gets a line in
/// the file) and the app's default doesn't come back at the next start. The
/// current value changes too (change callbacks run) while the flag is still at
/// its default.
/// @return false for an unknown flag or a value it doesn't take
bool SetDefaultValue(std::string_view name, std::string_view value);

/// The value the settings file has for this flag (as of the last LoadConfig
/// or SaveConfig), even when a higher-priority source won over it this run;
/// nullopt when the file has no line for it.
std::optional<std::string> GetConfigFileValue(std::string_view name);

}  // namespace rex::cvar
