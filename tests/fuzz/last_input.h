/**
 * @file        fuzz/last_input.h
 * @brief       Keeps the input being run in a file, for deaths libFuzzer can't
 *              report
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

namespace rex::fuzz {

// A process ended by __fastfail runs no handler, so libFuzzer writes no
// artifact. With the environment variable REX_FUZZ_LAST_INPUT set to a path
// (one process only, not -fork), the file there is the input that killed it.
inline void SaveLastInput(const uint8_t* data, size_t size) {
  static const std::string path = [] {
    const char* value = std::getenv("REX_FUZZ_LAST_INPUT");
    return std::string(value ? value : "");
  }();
  if (path.empty()) {
    return;
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(data), std::streamsize(size));
}

}  // namespace rex::fuzz
