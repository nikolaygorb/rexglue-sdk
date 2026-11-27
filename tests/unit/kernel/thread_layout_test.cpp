#include <catch2/catch_test_macros.hpp>

#include <rex/system/xthread.h>

#include <cstddef>

TEST_CASE("KTHREAD critical region nesting is stored in guest byte order", "[kernel][thread]") {
  rex::system::X_KTHREAD thread{};
  STATIC_REQUIRE(offsetof(rex::system::X_KTHREAD, apc_disable_count) == 0xB0);
  --thread.apc_disable_count;
  --thread.apc_disable_count;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&thread);
  REQUIRE(bytes[0xB0] == 0xFF);
  REQUIRE(bytes[0xB1] == 0xFF);
  REQUIRE(bytes[0xB2] == 0xFF);
  REQUIRE(bytes[0xB3] == 0xFE);
  REQUIRE(++thread.apc_disable_count == -1);
  REQUIRE(++thread.apc_disable_count == 0);
}

TEST_CASE("KAPC type is stored in guest byte order", "[kernel][thread]") {
  rex::system::XAPC apc{};
  apc.type = 18;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&apc);
  REQUIRE(bytes[0] == 0);
  REQUIRE(bytes[1] == 18);
  REQUIRE(uint16_t(apc.type) == 18);
}
