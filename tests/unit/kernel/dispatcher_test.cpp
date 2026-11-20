#include <catch2/catch_test_macros.hpp>

#include <rex/system/xtypes.h>
#include <rex/types.h>

using rex::X_STATUS;

namespace rex::kernel::xboxkrnl {
u32 NtCreateTimer_entry(mapped_u32 handle_ptr, mapped_void attributes, u32 type);
u32 KeSetAffinityThread_entry(mapped_void thread, u32 affinity, mapped_u32 previous);
}  // namespace rex::kernel::xboxkrnl

TEST_CASE("Invalid affinity masks fail before touching the output or thread", "[kernel][thread]") {
  rex::be<uint32_t> previous = 0xA5A5A5A5;
  for (uint32_t mask : {0u, 0x40u, 0x80u, 0x81u, 0x100u, 0xFFFFFFFFu}) {
    CHECK(rex::kernel::xboxkrnl::KeSetAffinityThread_entry(
              {}, mask, mapped_u32(&previous, 0x10000)) == X_STATUS_INVALID_PARAMETER);
    CHECK(uint32_t(previous) == 0xA5A5A5A5);
  }
}

TEST_CASE("NtCreateTimer rejects invalid types before touching the output or kernel",
          "[kernel][timer]") {
  rex::be<uint32_t> handle = 0x12345678;
  for (uint32_t type : {2u, 8u, 9u, 0xFFFFFFFFu}) {
    CHECK(rex::kernel::xboxkrnl::NtCreateTimer_entry(mapped_u32(&handle, 0x10000), {}, type) ==
          X_STATUS_INVALID_PARAMETER);
    CHECK(uint32_t(handle) == 0x12345678);
  }
}
