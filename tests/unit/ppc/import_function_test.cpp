/**
 * Tests for ArgTranslator stack argument support and ImportFunction isolation.
 */

#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/system/format.h>

using namespace rex::ppc;

// Fake 64KB guest memory block for tests
alignas(64) static uint8_t g_test_mem[0x10000] = {};

TEST_CASE("SetIntegerArgumentValue writes stack args for index > 7", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 8, 0xDEADBEEF);

  uint64_t readback = ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 8);
  CHECK(readback == 0xDEADBEEF);
}

TEST_CASE("SetIntegerArgumentValue writes multiple stack args at correct offsets",
          "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  std::memset(g_test_mem + 0x8000, 0, 0x100);

  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 8, 0x11111111);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 9, 0x22222222);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 10, 0x33333333);

  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 8) == 0x11111111);
  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 9) == 0x22222222);
  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 10) == 0x33333333);
}

TEST_CASE("SetIntegerArgumentValue still works for register args 0-7", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 0, 0xAAAA);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 7, 0xBBBB);
  CHECK(ctx.r3.u64 == 0xAAAA);
  CHECK(ctx.r10.u64 == 0xBBBB);
}

// Guest callers store each argument past r10 as a big-endian doubleword from
// r1 + 0x50; a 32-bit value sits in the slot's low bytes (r1 + 0x54).
static void StoreGuestSlot(uint32_t r1, size_t arg, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    g_test_mem[r1 + 0x50 + (arg - 8) * 8 + i] = uint8_t(value >> (56 - i * 8));
  }
}

TEST_CASE("GetValue reads 64-bit stack args from the whole slot", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  StoreGuestSlot(0x8000, 8, 0x0123456789ABCDEFull);
  StoreGuestSlot(0x8000, 9, 0xFFFFFFFF00001234ull);

  CHECK(ArgTranslator::GetValue<uint64_t>(ctx, g_test_mem, 8) == 0x0123456789ABCDEFull);
  // Only the low word belongs to a 32-bit argument.
  CHECK(ArgTranslator::GetValue<uint32_t>(ctx, g_test_mem, 9) == 0x1234);
}

TEST_CASE("GetValue treats a stack pointer arg as null by its low word", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  StoreGuestSlot(0x8000, 8, 0xCDCDCDCD00000000ull);

  CHECK(ArgTranslator::GetValue<uint8_t*>(ctx, g_test_mem, 8) == nullptr);
}

TEST_CASE("SetIntegerArgumentValue writes the whole stack slot", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  std::memset(g_test_mem + 0x8000, 0xCD, 0x100);

  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 8, 0x1122334455667788ull);
  const uint8_t expected[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  CHECK(std::memcmp(g_test_mem + 0x8050, expected, 8) == 0);
  CHECK(ArgTranslator::GetValue<uint64_t>(ctx, g_test_mem, 8) == 0x1122334455667788ull);
}

TEST_CASE("StackArgList reads varargs past r10 from their own slots", "[ppc][format]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  ctx.r10.u64 = 7;
  StoreGuestSlot(0x8000, 8, 8);
  StoreGuestSlot(0x8000, 9, 0x0000000900000009ull);
  StoreGuestSlot(0x8000, 10, 10);

  rex::system::format::StackArgList args(ctx, g_test_mem, 7);
  CHECK(args.get32() == 7);
  CHECK(args.get32() == 8);
  CHECK(args.get64() == 0x0000000900000009ull);
  CHECK(args.get32() == 10);
}
