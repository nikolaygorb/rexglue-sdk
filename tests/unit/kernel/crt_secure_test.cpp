#include <array>
#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include <rex/ppc/context.h>

REX_EXTERN(rexcrt_memcpy_s);
REX_EXTERN(rexcrt_memmove_s);
REX_EXTERN(rexcrt_strcpy_s);
REX_EXTERN(rexcrt_wcsncpy_s);
REX_EXTERN(rexcrt_wcscmp);

TEST_CASE("Secure CRT hooks report errors without invoking the host invalid handler",
          "[kernel][crt]") {
  alignas(16) std::array<uint8_t, 1024> memory{};
  PPCContext ctx{};
  ctx.r3.u64 = 16;
  ctx.r4.u64 = 8;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 1;
  std::memset(memory.data() + 16, 0xCC, 8);
  rexcrt_memcpy_s(ctx, memory.data());
  CHECK(ctx.r3.u32 == 22);
  for (size_t i = 16; i < 24; ++i)
    CHECK(memory[i] == 0);
  ctx.r3.u64 = 16;
  ctx.r5.u64 = 32;
  ctx.r6.u64 = 9;
  rexcrt_memmove_s(ctx, memory.data());
  CHECK(ctx.r3.u32 == 34);
  ctx.r3.u64 = 16;
  ctx.r5.u64 = 0;
  memory[16] = 'x';
  rexcrt_strcpy_s(ctx, memory.data());
  CHECK(ctx.r3.u32 == 22);
  CHECK(memory[16] == 0);
}

TEST_CASE("Wide CRT hooks respect guest endian order and truncate semantics", "[kernel][crt]") {
  alignas(16) std::array<uint8_t, 1024> memory{};
  auto* a = reinterpret_cast<rex::be<uint16_t>*>(memory.data() + 16);
  auto* b = reinterpret_cast<rex::be<uint16_t>*>(memory.data() + 32);
  a[0] = 0x100;
  a[1] = 0;
  b[0] = 0xFF;
  b[1] = 0;
  PPCContext ctx{};
  ctx.r3.u64 = 16;
  ctx.r4.u64 = 32;
  rexcrt_wcscmp(ctx, memory.data());
  CHECK(ctx.r3.u32 == 1);
  b[0] = 'A';
  b[1] = 'B';
  b[2] = 'C';
  b[3] = 0;
  ctx.r3.u64 = 16;
  ctx.r4.u64 = 2;
  ctx.r5.u64 = 32;
  ctx.r6.u64 = 0xFFFFFFFF;
  rexcrt_wcsncpy_s(ctx, memory.data());
  CHECK(ctx.r3.u32 == 80);
  CHECK(uint16_t(a[0]) == 'A');
  CHECK(uint16_t(a[1]) == 0);
}
