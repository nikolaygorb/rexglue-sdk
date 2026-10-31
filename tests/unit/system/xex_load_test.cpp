/**
 * @file        system/xex_load_test.cpp
 * @brief       Damaged XEX files are refused without reading outside the file
 *              (found by xex_fuzzer)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/function_dispatcher.h>
#include <rex/system/util/xex2_info.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

#include "test_memory.h"

using namespace rex;

namespace {

constexpr uint32_t kHeaderSize = 0x1000;
constexpr uint32_t kSecurityOffset = 0x200;
constexpr uint32_t kFormatOffset = 0x100;
constexpr uint32_t kCountOffset = kSecurityOffset + 0x180;
// In the user heap, which no other test allocates from: loading resets the heap.
constexpr uint32_t kLoadAddress = 0x00100000;

void StoreBe32(std::vector<uint8_t>& file, size_t offset, uint32_t value) {
  file[offset] = uint8_t(value >> 24);
  file[offset + 1] = uint8_t(value >> 16);
  file[offset + 2] = uint8_t(value >> 8);
  file[offset + 3] = uint8_t(value);
}

// A XEX2 with the file format header and no page descriptors, compressed
// normally, not encrypted, with a first block of 0x20 bytes after the header.
std::vector<uint8_t> MakeXex() {
  std::vector<uint8_t> file(kHeaderSize + 0x20);
  std::memcpy(file.data(), "XEX2", 4);
  StoreBe32(file, 4, 1);
  StoreBe32(file, 8, kHeaderSize);
  StoreBe32(file, 16, kSecurityOffset);
  StoreBe32(file, 20, 1);
  StoreBe32(file, 0x18, XEX_HEADER_FILE_FORMAT_INFO);
  StoreBe32(file, 0x1C, kFormatOffset);
  StoreBe32(file, kFormatOffset, 36);
  // Encryption none, compression normal.
  StoreBe32(file, kFormatOffset + 4, XEX_COMPRESSION_NORMAL);
  StoreBe32(file, kFormatOffset + 8, 0x8000);
  StoreBe32(file, kFormatOffset + 12, 0x20);
  StoreBe32(file, kSecurityOffset + 0x110, kLoadAddress);
  return file;
}

bool Load(const std::vector<uint8_t>& file) {
  runtime::FunctionDispatcher dispatcher(&testing::GetTestMemory(), nullptr);
  runtime::XexModule module(&dispatcher, nullptr);
  return module.Load("test", "test", file.data(), file.size());
}

}  // namespace

TEST_CASE("XEX shorter than its fixed header is refused", "[xex]") {
  std::vector<uint8_t> file = MakeXex();
  file.resize(8);
  CHECK_FALSE(Load(file));
}

TEST_CASE("XEX header larger than the file is refused", "[xex]") {
  std::vector<uint8_t> file = MakeXex();
  StoreBe32(file, 8, 0x7FFFFFFF);
  CHECK_FALSE(Load(file));
}

TEST_CASE("XEX header table larger than the header is refused", "[xex]") {
  std::vector<uint8_t> file = MakeXex();
  StoreBe32(file, 20, 0x10000000);
  CHECK_FALSE(Load(file));
}

TEST_CASE("XEX optional header outside the header is refused", "[xex]") {
  std::vector<uint8_t> file = MakeXex();
  StoreBe32(file, 0x1C, kHeaderSize + 0x100000);
  CHECK_FALSE(Load(file));
}

TEST_CASE("XEX optional header with a size past the header is refused", "[xex]") {
  std::vector<uint8_t> file = MakeXex();
  StoreBe32(file, kFormatOffset, kHeaderSize);
  CHECK_FALSE(Load(file));
}

TEST_CASE("XEX security info outside the header is refused", "[xex]") {
  std::vector<uint8_t> file = MakeXex();
  StoreBe32(file, 16, kHeaderSize);
  CHECK_FALSE(Load(file));
}

TEST_CASE("XEX page descriptors past the header are refused", "[xex]") {
  std::vector<uint8_t> file = MakeXex();
  StoreBe32(file, kCountOffset, 0x01000000);
  CHECK_FALSE(Load(file));
}

TEST_CASE("XEX with a base address in no heap is refused", "[xex]") {
  std::vector<uint8_t> file = MakeXex();
  StoreBe32(file, kSecurityOffset + 0x110, 0x7F000000);
  CHECK_FALSE(Load(file));
}

TEST_CASE("XEX compressed block past the file is refused", "[xex]") {
  std::vector<uint8_t> file = MakeXex();
  StoreBe32(file, kFormatOffset + 12, 0x7FFFFFFF);
  CHECK_FALSE(Load(file));
}

TEST_CASE("XEX compressed block too small for its header is refused", "[xex]") {
  std::vector<uint8_t> file = MakeXex();
  StoreBe32(file, kFormatOffset + 12, 8);
  CHECK_FALSE(Load(file));
}

TEST_CASE("XEX file format info too small for its compression is refused", "[xex]") {
  std::vector<uint8_t> file = MakeXex();
  StoreBe32(file, kFormatOffset, 8);
  CHECK_FALSE(Load(file));
}
