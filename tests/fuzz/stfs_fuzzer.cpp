/**
 * @file        fuzz/stfs_fuzzer.cpp
 * @brief       libFuzzer target for STFS and SVOD content packages
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

#include <fmt/format.h>

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/devices/stfs_xbox.h>

#include "filesystem_common.h"
#include "last_input.h"

using namespace rex::filesystem;

// Input layout (a package header is 0x971A bytes, nearly all of them never
// read, so only those that are come from the input):
//   0: bits 0..1  - magic: CON, LIVE or PIRS;
//      bit 2      - SVOD (otherwise STFS);
//      bit 3      - mount the directory holding the package (the device looks
//                   for it, reading the magic of every file there);
//      bit 4      - one data fragment fewer than the header says;
//   1..0x24       - the volume descriptor, as in the header;
//   0x25..0x28    - header_size, big endian;
//   0x29          - data_file_count (bits 0..2);
//   0x2A..0x2B    - with fragments: the size of the first / 16, big endian;
//   then          - what follows the header at 0xA000, or with
//                   data_file_count >= 2, the data fragments: the first of the
//                   given size, the rest split equally.
//
// StfsContainerDevice::Initialize, then what a title does with mounted content
// (filesystem_common.h). tools/fuzz/make_synthetic_corpus.py stfs writes
// well-formed seeds.

namespace {

constexpr size_t kInputHeaderSize = 0x2C;
constexpr size_t kVolumeDescriptorSize = 0x24;
constexpr size_t kDataOffset = 0xA000;

uint32_t LoadBe32(const uint8_t* data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) |
         uint32_t(data[3]);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  rex::fuzz::SaveLastInput(data, size);
  if (size < kInputHeaderSize) {
    return 0;
  }
  static rex::fuzz::TempDirectory directory("stfs_fuzzer");
  directory.Clear();

  uint8_t flags = data[0];
  static constexpr XContentPackageType kMagics[] = {
      XContentPackageType::kCon, XContentPackageType::kLive, XContentPackageType::kPirs};
  uint32_t data_file_count = data[0x29] & 0b111;
  size_t first_fragment_size = ((size_t(data[0x2A]) << 8) | data[0x2B]) << 4;
  const uint8_t* content = data + kInputHeaderSize;
  size_t content_size = size - kInputHeaderSize;

  std::vector<uint8_t> file(sizeof(StfsHeader));
  auto* header = reinterpret_cast<StfsHeader*>(file.data());
  header->header.magic = kMagics[(flags & 0b11) % 3];
  header->header.header_size = LoadBe32(data + 0x25);
  std::memcpy(&header->metadata.volume_descriptor, data + 1, kVolumeDescriptorSize);
  header->metadata.data_file_count = data_file_count;
  header->metadata.volume_type =
      (flags & (1 << 2)) ? XContentVolumeType::kSvod : XContentVolumeType::kStfs;

  auto package_path = directory.path / "package";
  if (data_file_count >= 2) {
    rex::fuzz::WriteFile(package_path, file.data(), file.size());
    auto fragments_path = package_path;
    fragments_path += ".data";
    std::filesystem::create_directory(fragments_path);
    uint32_t fragment_count = data_file_count - ((flags & (1 << 4)) ? 1 : 0);
    size_t first_size = std::min(first_fragment_size, content_size);
    size_t rest_size =
        fragment_count > 1 ? (content_size - first_size) / (fragment_count - 1) : 0;
    size_t offset = 0;
    for (uint32_t i = 0; i < fragment_count; ++i) {
      size_t fragment_size = i ? rest_size : first_size;
      rex::fuzz::WriteFile(fragments_path / fmt::format("Data{:04}", i), content + offset,
                           fragment_size);
      offset += fragment_size;
    }
  } else {
    file.resize(kDataOffset + content_size);
    if (content_size) {
      std::memcpy(file.data() + kDataOffset, content, content_size);
    }
    rex::fuzz::WriteFile(package_path, file.data(), file.size());
  }

  StfsContainerDevice device("\\Device\\Fuzz",
                             (flags & (1 << 3)) ? directory.path : package_path);
  if (device.Initialize()) {
    rex::fuzz::WalkDevice(device);
  }
  return 0;
}
