/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Ported for the ReXGlue fork
 */

#pragma once

#include <string>
#include <string_view>

#include <rex/string/utf8.h>

namespace rex::kernel::xboxkrnl {

// Drops a leading \??\, the DOS devices directory the ObDosDevices root also
// names, so \??\game:\file is game:\file.
inline void xeObStripDosDevicesPrefix(std::string& path) {
  // Bytes, as a guest name need not be valid UTF-8.
  if (path.starts_with("\\??\\")) {
    path.erase(0, 4);
  }
}

// Canonical link name without the \??\ or \System??\ object directory.
inline std::string xeObSymbolicLinkName(const std::string_view name) {
  auto path = rex::string::utf8_canonicalize_guest_path(name);
  xeObStripDosDevicesPrefix(path);
  if (rex::string::utf8_starts_with_case(path, "\\System??\\")) {
    path.erase(0, 10);
  }
  return path;
}

}  // namespace rex::kernel::xboxkrnl
