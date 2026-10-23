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

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <rex/filesystem/device.h>

namespace rex::filesystem {

// The runtime keeps a package thumbnail as a file inside the package directory.
// The console holds it in the STFS header, so the guest never sees it.
inline constexpr char kPackageThumbnailFileName[] = "__thumbnail.png";

class HostPathEntry;

class HostPathDevice : public Device {
 public:
  HostPathDevice(const std::string_view mount_path, const std::filesystem::path& host_path,
                 bool read_only, bool allow_share_delete = false, bool package = false);
  ~HostPathDevice() override;

  bool Initialize() override;
  void Dump(string::StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return read_only_; }
  bool is_io_queued() const override { return is_package_; }
  // NOTE(tomc): When true, host file handles open with FILE_SHARE_DELETE so an open read
  // handle (e.g. a save-slot preview) does not block an overwrite's
  // delete+recreate. Opt-in per device: content/save devices set it; game-data
  // and read-only devices leave it off (see a5c3a963 ghost-file fix).
  bool allow_share_delete() const { return allow_share_delete_; }
  const std::filesystem::path& host_path() const { return host_path_; }
  // Files under `overlay_path` at the same relative path stand in for the
  // device's own when it populates (Initialize): a translated copy of a
  // package's text without touching the package. Set before Initialize; empty
  // for none.
  void set_overlay_path(const std::filesystem::path& overlay_path) {
    overlay_path_ = overlay_path;
  }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 128 * 1024; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  // Walks one directory, with the canonical path of every directory already
  // on this branch, so a symlink (or junction) into one of them is not
  // followed again.
  void PopulateEntry(HostPathEntry* parent_entry, std::vector<std::filesystem::path>& ancestors);

  std::string name_;
  std::filesystem::path host_path_;
  std::filesystem::path overlay_path_;
  std::unique_ptr<Entry> root_entry_;
  bool read_only_;
  bool allow_share_delete_;
  bool is_package_;
};

}  // namespace rex::filesystem
