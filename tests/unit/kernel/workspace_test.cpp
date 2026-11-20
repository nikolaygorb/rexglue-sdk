#include <catch2/catch_test_macros.hpp>

#include <rex/system/xam/workspace_heap.h>
#include <rex/system/xenumerator.h>
#include <rex/types.h>
#include "test_memory.h"

using rex::X_HRESULT;
using rex::X_RESULT;
using rex::system::xam::WorkspaceHeap;
namespace rex::kernel::xam {
u32 XamAlloc_entry(u32 flags, u32 size, mapped_u32 output);
u32 XamAllocEx_entry(u32 attributes, u32 flags, u32 size, mapped_u32 output);
}  // namespace rex::kernel::xam

TEST_CASE("Workspace size follows the XEX default, minimum and granularity",
          "[kernel][workspace]") {
  CHECK(WorkspaceHeap::ResolveSize(0) == 384 * 1024);
  CHECK(WorkspaceHeap::ResolveSize(1) == 256 * 1024);
  CHECK(WorkspaceHeap::ResolveSize(256 * 1024 + 1) == 272 * 1024);
  CHECK(WorkspaceHeap::ResolveSize(UINT32_MAX) == 0);
}

TEST_CASE("Workspace reuses blocks, honors no-zero and coalesces free space",
          "[kernel][workspace]") {
  auto& memory = rex::testing::GetTestMemory();
  auto workspace = WorkspaceHeap::Create(memory);
  REQUIRE(workspace);
  const uint32_t first = workspace->Allocate(16);
  const uint32_t second = workspace->Allocate(17);
  REQUIRE(first);
  REQUIRE(second);
  CHECK((first & 15) == 0);
  CHECK(first < 0x2FFF0000);
  auto* bytes = memory.TranslateVirtual(first);
  for (int i = 0; i < 16; ++i) {
    CHECK(bytes[i] == 0);
    bytes[i] = 0xCC;
  }
  REQUIRE(workspace->Free(first));
  CHECK_FALSE(workspace->Free(first));
  CHECK_FALSE(workspace->Free(second + 1));
  REQUIRE(workspace->Allocate(16, false) == first);
  CHECK(bytes[0] == 0xCC);
  REQUIRE(workspace->Free(first));
  REQUIRE(workspace->Allocate(16) == first);
  CHECK(bytes[0] == 0);
  CHECK(workspace->Allocate(UINT32_MAX) == 0);
  REQUIRE(workspace->Free(first));
  REQUIRE(workspace->Free(second));
  const uint32_t all = workspace->Allocate(workspace->size());
  REQUIRE(all);
  CHECK(workspace->Allocate(1) == 0);
  CHECK(workspace->Free(all));
}

TEST_CASE("XAM allocation reports invalid arguments without accessing a kernel",
          "[kernel][workspace]") {
  using namespace rex::kernel::xam;
  CHECK(XamAlloc_entry(0, 1, {}) == X_E_INVALIDARG);
  CHECK(XamAllocEx_entry(0, 0x40000000, 1, {}) == X_E_INVALIDARG);
  rex::be<uint32_t> output = 0xDEADBEEF;
  CHECK(XamAlloc_entry(1, 8, mapped_u32(&output, 0x10000)) == X_E_UNEXPECTED);
  CHECK(uint32_t(output) == 0);
}

TEST_CASE("A static enumerator returns its configured batch", "[kernel][enumerator]") {
  // XamEnumerate's supplied buffer size is ignored by the console contract;
  // upstream 5bcc0101 tests this. Do not add a buffer-length clamp here.
  rex::system::XStaticUntypedEnumerator enumerator(nullptr, 2, 1);
  *enumerator.AppendItem() = 0xAB;
  *enumerator.AppendItem() = 0xCD;
  *enumerator.AppendItem() = 0xEF;
  uint8_t buffer[3]{};
  uint32_t count = 0;
  CHECK(enumerator.WriteItems(0x10000, buffer, &count) == X_ERROR_SUCCESS);
  CHECK(count == 2);
  CHECK(buffer[0] == 0xAB);
  CHECK(buffer[1] == 0xCD);
  CHECK(enumerator.WriteItems(0x10000, buffer, &count) == X_ERROR_SUCCESS);
  CHECK(count == 1);
  CHECK(buffer[0] == 0xEF);
}
