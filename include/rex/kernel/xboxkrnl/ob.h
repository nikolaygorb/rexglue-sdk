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

#include <rex/string.h>
#include <rex/string/utf8.h>

namespace rex::kernel::xboxkrnl {

// Drops a leading \??\, the DOS devices directory the ObDosDevices root also
// names, or \System??\ in any case, so both \??\game:\file and
// \System??\game:\file are game:\file. IsValidPath rejects '?', so a name
// under \System??\ was not found before.
inline void xeObStripDosDevicesPrefix(std::string& path) {
  // Bytes, as a guest name need not be valid UTF-8.
  constexpr std::string_view kSystemDosDevices = "\\System??\\";
  if (path.starts_with("\\??\\")) {
    path.erase(0, 4);
  } else if (path.size() >= kSystemDosDevices.size() &&
             !rex::string::compare_case_n(path.c_str(), kSystemDosDevices.data(),
                                          kSystemDosDevices.size())) {
    path.erase(0, kSystemDosDevices.size());
  }
}

// Canonical link name without the \??\ or \System??\ object directory.
inline std::string xeObSymbolicLinkName(const std::string_view name) {
  auto path = rex::string::utf8_canonicalize_guest_path(name);
  xeObStripDosDevicesPrefix(path);
  return path;
}

}  // namespace rex::kernel::xboxkrnl
