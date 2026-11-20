#include <catch2/catch_test_macros.hpp>

#include <rex/memory.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {
uint32_t ToXdkProtectFlags(uint32_t protect);
uint32_t FromXdkProtectFlags(uint32_t protect);
u32 MmCreateKernelStack_entry(u32 size, u32 type);
}  // namespace rex::kernel::xboxkrnl

TEST_CASE("Kernel page flags report user read-only and preserve cache attributes",
          "[kernel][memory]") {
  using namespace rex;
  using namespace rex::kernel::xboxkrnl;
  CHECK(ToXdkProtectFlags(0) == X_PAGE_NOACCESS);
  CHECK(ToXdkProtectFlags(memory::kMemoryProtectRead) == (X_PAGE_READONLY | X_PAGE_USER_READONLY));
  const uint32_t read = memory::kMemoryProtectRead | memory::kMemoryProtectNoCache;
  CHECK(FromXdkProtectFlags(ToXdkProtectFlags(read)) == read);
  const uint32_t write =
      memory::kMemoryProtectRead | memory::kMemoryProtectWrite | memory::kMemoryProtectWriteCombine;
  CHECK(ToXdkProtectFlags(write) == (X_PAGE_READWRITE | X_PAGE_WRITECOMBINE));
  CHECK(FromXdkProtectFlags(ToXdkProtectFlags(write)) == write);
}

TEST_CASE("MmCreateKernelStack refuses invalid requests before allocating", "[kernel][memory]") {
  using rex::kernel::xboxkrnl::MmCreateKernelStack_entry;
  CHECK(MmCreateKernelStack_entry(0x1000, 3) == 0);
  CHECK(MmCreateKernelStack_entry(0x1000, 0xFFFFFFFF) == 0);
  CHECK(MmCreateKernelStack_entry(0, 0) == 0);
  CHECK(MmCreateKernelStack_entry(0xFFFFFFFF, 0) == 0);
}
