/**
 * @file        fuzz/lzx_fuzzer.cpp
 * @brief       libFuzzer target for the XEX LZX decompressor
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/system/lzx.h>

#include "last_input.h"

// Input layout:
//   byte 0      - window size: 1 << (15 + byte % 7), 32 KB to 2 MB, as XEX
//                 normal compression allows;
//   bytes 1..2  - decompressed size in 4 KB pages (1..256), little endian;
//   the rest    - the compressed stream.
// XexModule::ReadImageCompressed passes no window data, so neither does this.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  rex::fuzz::SaveLastInput(data, size);
  if (size < 3) {
    return 0;
  }
  uint32_t window_size = uint32_t(1) << (15 + data[0] % 7);
  size_t dest_size = (size_t(data[1] | (data[2] << 8)) % 256 + 1) * 4096;
  std::vector<uint8_t> dest(dest_size);
  lzx_decompress(data + 3, size - 3, dest.data(), dest.size(), window_size, nullptr, 0);
  return 0;
}
