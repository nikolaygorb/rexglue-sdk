/**
 * @file        memory/range_access_test.cpp
 * @brief       Tests for querying access to committed guest memory ranges
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xmemory.h>

#include "test_memory.h"

namespace {

using rex::memory::PageAccess;
using rex::testing::GetTestMemory;

constexpr uint32_t kReadWrite = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;

rex::memory::BaseHeap* MutableHeap(const rex::memory::BaseHeap* heap) {
  return const_cast<rex::memory::BaseHeap*>(heap);
}

}  // namespace

// xenia-canary 948f66c87.
TEST_CASE("Only committed pages are accessible", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));  // v00000000, 4KB pages
  REQUIRE(heap != nullptr);

  uint32_t addr = 0;
  REQUIRE(heap->Alloc(3 * 4096, 4096, rex::memory::kMemoryAllocationReserve, kReadWrite, false,
                      &addr));

  SECTION("Reserved pages have no access") {
    CHECK(heap->QueryRangeAccess(addr, addr) == PageAccess::kNoAccess);
    CHECK(memory.QueryRangeAccess(addr, 3 * 4096) == PageAccess::kNoAccess);
  }

  SECTION("Committed pages have their protection, decommitted ones none") {
    REQUIRE(heap->AllocFixed(addr, 2 * 4096, 4096, rex::memory::kMemoryAllocationCommit,
                             kReadWrite));
    CHECK(memory.QueryRangeAccess(addr, 2 * 4096) == PageAccess::kReadWrite);
    // The third page is still only reserved.
    CHECK(memory.QueryRangeAccess(addr, 3 * 4096) == PageAccess::kNoAccess);
    REQUIRE(heap->Protect(addr + 4096, 4096, rex::memory::kMemoryProtectRead));
    CHECK(memory.QueryRangeAccess(addr, 2 * 4096) == PageAccess::kReadOnly);
    REQUIRE(heap->Decommit(addr, 4096));
    CHECK(memory.QueryRangeAccess(addr, 4096) == PageAccess::kNoAccess);
    CHECK(memory.QueryRangeAccess(addr + 4096, 4096) == PageAccess::kReadOnly);
  }

  heap->Release(addr, nullptr);
}

// xenia-canary 948f66c87.
TEST_CASE("Empty and overflowing ranges have no access", "[memory][heap]") {
  auto& memory = GetTestMemory();
  auto* heap = MutableHeap(memory.LookupHeap(0x10000000));
  REQUIRE(heap != nullptr);
  uint32_t addr = 0;
  REQUIRE(heap->Alloc(4096, 4096,
                      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                      kReadWrite, false, &addr));
  CHECK(memory.QueryRangeAccess(addr, 4096) == PageAccess::kReadWrite);
  CHECK(memory.QueryRangeAccess(addr, 0) == PageAccess::kNoAccess);
  CHECK(memory.QueryRangeAccess(0xFFFFF000, 0x2000) == PageAccess::kNoAccess);
  heap->Release(addr, nullptr);
}
