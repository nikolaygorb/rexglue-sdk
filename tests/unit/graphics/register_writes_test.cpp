/**
 * @file        graphics/register_writes_test.cpp
 * @brief       Tests for StoreChangedRegisters (register ranges stored only where they change)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <vector>

#include <rex/graphics/util/register_writes.h>
#include <rex/types.h>

using rex::graphics::StoreChangedRegisters;

namespace {

std::vector<uint32_t> ToBigEndian(const std::vector<uint32_t>& host) {
  std::vector<uint32_t> result(host.size());
  for (size_t i = 0; i < host.size(); ++i) {
    result[i] = rex::byte_swap(host[i]);
  }
  return result;
}

}  // namespace

TEST_CASE("StoreChangedRegisters: unchanged values report no change", "[graphics][registers]") {
  std::vector<uint32_t> values(200);
  for (uint32_t i = 0; i < values.size(); ++i) {
    values[i] = 0x3F800000u + i;
  }
  const std::vector<uint32_t> before = values;
  const std::vector<uint32_t> source = ToBigEndian(values);
  uint32_t first = 1234, last = 5678;
  REQUIRE_FALSE(StoreChangedRegisters(values.data(), source.data(), uint32_t(values.size()), first,
                                      last));
  CHECK(values == before);
  CHECK(first == 1234);
  CHECK(last == 5678);
}

TEST_CASE("StoreChangedRegisters: exact first and last change across chunks",
          "[graphics][registers]") {
  std::vector<uint32_t> values(300, 0);
  std::vector<uint32_t> wanted(300, 0);
  wanted[5] = 1;
  wanted[70] = 2;
  wanted[255] = 3;
  const std::vector<uint32_t> source = ToBigEndian(wanted);
  uint32_t first = 0, last = 0;
  REQUIRE(StoreChangedRegisters(values.data(), source.data(), uint32_t(values.size()), first,
                                last));
  CHECK(values == wanted);
  CHECK(first == 5);
  CHECK(last == 255);
}

TEST_CASE("StoreChangedRegisters: single register and chunk edges", "[graphics][registers]") {
  for (uint32_t count : {1u, 63u, 64u, 65u, 128u, 129u}) {
    for (uint32_t changed : {0u, count / 2, count - 1}) {
      std::vector<uint32_t> values(count, 7);
      std::vector<uint32_t> wanted = values;
      wanted[changed] = 8;
      const std::vector<uint32_t> source = ToBigEndian(wanted);
      uint32_t first = 0, last = 0;
      REQUIRE(StoreChangedRegisters(values.data(), source.data(), count, first, last));
      CHECK(values == wanted);
      CHECK(first == changed);
      CHECK(last == changed);
    }
  }
}

TEST_CASE("StoreChangedRegisters: matches a plain swap on random data", "[graphics][registers]") {
  std::mt19937 random(426);
  for (int round = 0; round < 200; ++round) {
    const uint32_t count = 1 + random() % 512;
    std::vector<uint32_t> values(count);
    for (uint32_t& value : values) {
      value = random() % 4;
    }
    std::vector<uint32_t> wanted = values;
    uint32_t expected_first = UINT32_MAX, expected_last = 0;
    for (uint32_t i = 0; i < count; ++i) {
      if (random() % 16 == 0) {
        const uint32_t value = random() % 4;
        if (value != wanted[i]) {
          wanted[i] = value;
          if (expected_first == UINT32_MAX) {
            expected_first = i;
          }
          expected_last = i;
        }
      }
    }
    const std::vector<uint32_t> source = ToBigEndian(wanted);
    uint32_t first = 0, last = 0;
    const bool changed = StoreChangedRegisters(values.data(), source.data(), count, first, last);
    CHECK(values == wanted);
    REQUIRE(changed == (expected_first != UINT32_MAX));
    if (changed) {
      CHECK(first == expected_first);
      CHECK(last == expected_last);
    }
  }
}

TEST_CASE("IsShaderConstantRegister: the three constant blocks only", "[graphics][registers]") {
  using rex::graphics::IsShaderConstantRegister;
  CHECK(IsShaderConstantRegister(0x4000));
  CHECK(IsShaderConstantRegister(0x47FF));
  CHECK(IsShaderConstantRegister(0x4800));
  CHECK(IsShaderConstantRegister(0x48BF));
  CHECK_FALSE(IsShaderConstantRegister(0x48C0));
  CHECK_FALSE(IsShaderConstantRegister(0x48FF));
  CHECK(IsShaderConstantRegister(0x4900));
  CHECK(IsShaderConstantRegister(0x4927));
  CHECK_FALSE(IsShaderConstantRegister(0x4928));
  CHECK_FALSE(IsShaderConstantRegister(0x3FFF));
  CHECK_FALSE(IsShaderConstantRegister(0x2000));
}
