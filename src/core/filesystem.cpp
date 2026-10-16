/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>

#include <rex/filesystem.h>
#include <rex/string/utf8.h>

namespace rex {
namespace filesystem {

bool CreateParentFolder(const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    auto parent_path = path.parent_path();
    if (!std::filesystem::exists(parent_path)) {
      return std::filesystem::create_directories(parent_path);
    }
  }
  return true;
}

std::filesystem::path ResolvePathCaseInsensitive(const std::filesystem::path& base,
                                                 const std::filesystem::path& relative) {
  std::filesystem::path result = relative.is_absolute() ? relative.root_path() : base;
  bool matching = true;
  for (const auto& part : relative.relative_path()) {
    auto exact = result / part;
    std::error_code ec;
    if (!matching || part == "." || part == ".." || std::filesystem::exists(exact, ec)) {
      result = std::move(exact);
      continue;
    }
    // Only a casing mismatch reaches the directory scan.
    const auto part_utf8 = path_to_utf8(part);
    const auto child_infos = ListFiles(result.empty() ? std::filesystem::path(".") : result);
    auto match = std::find_if(child_infos.begin(), child_infos.end(), [&](const auto& info) {
      return string::utf8_equal_case(path_to_utf8(info.name), part_utf8);
    });
    if (match == child_infos.end()) {
      matching = false;
      result = std::move(exact);
      continue;
    }
    result /= match->name;
  }
  return result;
}

}  // namespace filesystem
}  // namespace rex
