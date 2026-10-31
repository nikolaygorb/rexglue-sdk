/**
 * @file        fuzz/filesystem_common.h
 * @brief       What the content and disc image fuzzers share: their files on
 *              disk and reading a mounted device like the title does
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/memory/mapped_memory.h>
#include <rex/platform.h>
#include <rex/string/buffer.h>

#if REX_PLATFORM_WIN32
#include <process.h>
#define REX_FUZZ_GETPID _getpid
#else
#include <unistd.h>
#define REX_FUZZ_GETPID getpid
#endif

namespace rex::fuzz {

// A directory of its own for each process (libFuzzer -fork and -jobs run
// several), removed at exit.
struct TempDirectory {
  std::filesystem::path path;
  explicit TempDirectory(const char* name)
      : path(std::filesystem::temp_directory_path() /
             fmt::format("rexglue_{}_{}", name, REX_FUZZ_GETPID())) {
    Clear();
  }
  ~TempDirectory() {
    std::error_code error_code;
    std::filesystem::remove_all(path, error_code);
  }
  void Clear() {
    std::error_code error_code;
    std::filesystem::remove_all(path, error_code);
    std::filesystem::create_directories(path, error_code);
  }
};

inline void WriteFile(const std::filesystem::path& path, const uint8_t* data, size_t size) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(data), std::streamsize(size));
}

// Reads a few places of up to kMaxFilesRead files, so a tree of thousands of
// entries pointing at the same data doesn't take the run's whole time.
class DeviceWalker {
 public:
  explicit DeviceWalker(filesystem::Device& device) : device_(device), buffer_(kReadSize) {}

  void Walk(filesystem::Entry& entry) {
    for (const auto& child : entry.children()) {
      if (child->attributes() & filesystem::kFileAttributeDirectory) {
        Walk(*child);
      } else if (files_read_ < kMaxFilesRead) {
        ++files_read_;
        ReadFile(*child);
      }
    }
  }

 private:
  static constexpr size_t kMaxFilesRead = 32;
  static constexpr size_t kReadSize = 0x2000;

  void ReadFile(filesystem::Entry& entry) {
    device_.ResolvePath(entry.path());
    filesystem::File* file = nullptr;
    if (entry.Open(rex::filesystem::FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS ||
        !file) {
      return;
    }
    size_t size = entry.size();
    const std::array<size_t, 3> offsets = {0, size / 2, size - std::min(size, kReadSize)};
    for (size_t offset : offsets) {
      size_t bytes_read = 0;
      file->ReadSync(std::span<uint8_t>(buffer_), offset, &bytes_read);
    }
    file->Destroy();
    if (entry.can_map()) {
      auto mapped = entry.OpenMapped(memory::MappedMemory::Mode::kRead);
      if (mapped && mapped->size()) {
        // What loading a module from the mapping does first and last.
        sink_ = sink_ + mapped->data()[0] + mapped->data()[mapped->size() - 1];
      }
    }
  }

  filesystem::Device& device_;
  std::vector<uint8_t> buffer_;
  size_t files_read_ = 0;
  volatile uint8_t sink_ = 0;
};

// What the kernel asks a mounted device for.
inline void WalkDevice(filesystem::Device& device) {
  device.total_allocation_units();
  device.available_allocation_units();
  device.is_read_only();
  string::StringBuffer dump;
  device.Dump(&dump);
  filesystem::Entry* root = device.ResolvePath("");
  if (root) {
    DeviceWalker(device).Walk(*root);
  }
}

}  // namespace rex::fuzz
