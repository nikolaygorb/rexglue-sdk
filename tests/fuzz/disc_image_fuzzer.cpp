/**
 * @file        fuzz/disc_image_fuzzer.cpp
 * @brief       libFuzzer target for GDFX / XISO disc images
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/filesystem/devices/disc_image_device.h>

#include "filesystem_common.h"
#include "last_input.h"

using namespace rex::filesystem;

// Input layout:
//   0: bits 0..1  - game partition offset: 0, 0xFB20 or 0x20600 (the larger
//                   ones DiscImageDevice tries would write 32+ MB per run);
//   then          - the file from byte 20 of the volume descriptor (sector 32)
//                   on: root directory sector and size, the rest of the
//                   sector, the sectors after it.
// The file has the magic in place, and is padded to a whole 4 KB page: the
// device reads the mapped file, which ASan doesn't watch, so reading past the
// end only shows if it leaves the mapping (access violation).
//
// DiscImageDevice::Initialize, then what a title does with a mounted disc
// (filesystem_common.h). tools/fuzz/make_synthetic_corpus.py disc_image
// writes well-formed seeds.

namespace {

constexpr size_t kSectorSize = 0x800;
constexpr size_t kVolumeDescriptorOffset = 32 * kSectorSize;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kMagicSize = sizeof(kMagic) - 1;
constexpr size_t kPageSize = 0x1000;
constexpr size_t kGameOffsets[] = {0, 0xFB20, 0x20600};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  rex::fuzz::SaveLastInput(data, size);
  if (size < 1) {
    return 0;
  }
  static rex::fuzz::TempDirectory directory("disc_image_fuzzer");
  size_t game_offset = kGameOffsets[(data[0] & 0b11) % 3];
  ++data;
  --size;

  size_t magic_offset = game_offset + kVolumeDescriptorOffset;
  std::vector<uint8_t> file(magic_offset + kMagicSize + size);
  std::memcpy(file.data() + magic_offset, kMagic, kMagicSize);
  if (size) {
    std::memcpy(file.data() + magic_offset + kMagicSize, data, size);
  }
  file.resize((file.size() + kPageSize - 1) & ~(kPageSize - 1));

  auto path = directory.path / "disc.iso";
  rex::fuzz::WriteFile(path, file.data(), file.size());

  DiscImageDevice device("\\Device\\Fuzz", path);
  if (device.Initialize()) {
    rex::fuzz::WalkDevice(device);
  }
  return 0;
}
