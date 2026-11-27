/**
 * @file        kernel/encrypted_memory_test.cpp
 * @brief       NtFreeEncryptedMemory takes its base by value and checks it
 *              against the region of its type before touching memory
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace rex::kernel::xboxkrnl {
u32 NtFreeEncryptedMemory_entry(u32 region_type, u32 base_address);
}  // namespace rex::kernel::xboxkrnl

TEST_CASE("NtFreeEncryptedMemory refuses a base outside its type's region", "[kernel][memory]") {
  using rex::X_STATUS;
  using rex::kernel::xboxkrnl::NtFreeEncryptedMemory_entry;
  // Types past the system's (2).
  CHECK(NtFreeEncryptedMemory_entry(3, 0x8D000000) == X_STATUS_INVALID_PARAMETER);
  CHECK(NtFreeEncryptedMemory_entry(0xFFFFFFFF, 0x8C000000) == X_STATUS_INVALID_PARAMETER);
  // The title's region is 0x8D000000-0x8FFFFFFF, the system's 0x8C000000-0x8CFFFFFF.
  CHECK(NtFreeEncryptedMemory_entry(1, 0x8C000000) == X_STATUS_INVALID_PARAMETER);
  CHECK(NtFreeEncryptedMemory_entry(1, 0x90000000) == X_STATUS_INVALID_PARAMETER);
  CHECK(NtFreeEncryptedMemory_entry(2, 0x8D000000) == X_STATUS_INVALID_PARAMETER);
  CHECK(NtFreeEncryptedMemory_entry(2, 0x8BFFFFFF) == X_STATUS_INVALID_PARAMETER);
  // A page index, which the old code took the base for (it read the second
  // argument as a pointer to one).
  CHECK(NtFreeEncryptedMemory_entry(1, 0x10) == X_STATUS_INVALID_PARAMETER);
}
