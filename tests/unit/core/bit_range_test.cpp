#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>
#include <rex/bit.h>

TEST_CASE("SetRange fills every bit across multiple blocks", "[core][bit]") {
  std::array<uint64_t, 5> bits{1, 0, 0, 0, 1ull << 63};
  rex::bit::SetRange(bits.data(), 61, 4 * 64 - 61 + 3);
  CHECK(bits[0] == 0xE000000000000001ull);
  CHECK(bits[1] == UINT64_MAX);
  CHECK(bits[2] == UINT64_MAX);
  CHECK(bits[3] == UINT64_MAX);
  CHECK(bits[4] == 0x8000000000000007ull);
}
