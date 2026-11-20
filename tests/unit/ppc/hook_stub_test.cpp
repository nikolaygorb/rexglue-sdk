#include <catch2/catch_test_macros.hpp>
#include <rex/hook.h>

namespace {
int calls = 0;
uint64_t NextStubResult() {
  ++calls;
  return 0x123456789ABCDEF0ull;
}
}  // namespace

REX_STUB_RETURN(test_stub_return_once, NextStubResult())

TEST_CASE("A return stub evaluates its result once", "[ppc][hook]") {
  rex::InitLogging();
  auto logger = rex::GetLogger(rex::log::krnl());
  REQUIRE(logger);
  struct RestoreLevel {
    spdlog::logger& logger;
    spdlog::level::level_enum level;
    ~RestoreLevel() { logger.set_level(level); }
  } restore{*logger, logger->level()};
  logger->set_level(spdlog::level::warn);
  PPCContext ctx{};
  calls = 0;
  test_stub_return_once(ctx, nullptr);
  CHECK(calls == 1);
  CHECK(ctx.r3.u64 == 0x123456789ABCDEF0ull);
}
