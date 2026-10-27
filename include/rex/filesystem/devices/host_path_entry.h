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

#include <atomic>
#include <string>

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>

namespace rex::filesystem {

class HostPathDevice;

class HostPathEntry : public Entry {
 public:
  HostPathEntry(Device* device, Entry* parent, const std::string_view path,
                const std::filesystem::path& host_path);
  ~HostPathEntry() override;

  static HostPathEntry* Create(Device* device, Entry* parent,
                               const std::filesystem::path& full_path,
                               rex::filesystem::FileInfo file_info);

  const std::filesystem::path& host_path() const { return host_path_; }

  X_STATUS Open(uint32_t desired_access, File** out_file) override;
  bool Truncate() override;

  bool can_map() const override { return true; }
  std::unique_ptr<memory::MappedMemory> OpenMapped(memory::MappedMemory::Mode mode, size_t offset,
                                                   size_t length) override;
  void update() override;
  bool has_open_files() const override;
  void RetainFile() override { open_files_.fetch_add(1); }
  // The last handle closing deletes an entry set for deletion.
  void ReleaseFile() override;
  bool SetCreateTimestamp(uint64_t timestamp) override;
  bool SetAccessTimestamp(uint64_t timestamp) override;
  bool SetWriteTimestamp(uint64_t timestamp) override;

 private:
  friend class HostPathDevice;

  std::unique_ptr<Entry> CreateEntryInternal(const std::string_view name,
                                             uint32_t attributes) override;
  bool DeleteEntryInternal(Entry* entry) override;
  X_STATUS RenameEntryInternal(Entry* new_parent, const std::string_view name) override;

  std::filesystem::path host_path_;
  std::atomic<size_t> open_files_{0};
};

}  // namespace rex::filesystem
