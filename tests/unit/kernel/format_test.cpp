#include <array>
#include <bit>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/system/format.h>

namespace {
struct Args {
  std::vector<uint64_t> values;
  size_t index = 0;
  uint32_t get32() { return static_cast<uint32_t>(values.at(index++)); }
  uint64_t get64() { return values.at(index++); }
};
std::string Format(const char* pattern, std::initializer_list<uint64_t> values,
                   uint8_t* base = nullptr) {
  rex::system::format::StringFormatData data(reinterpret_cast<const uint8_t*>(pattern));
  Args args{values};
  CHECK(rex::system::format::format_core(base, data, args, false) >= 0);
  return data.str();
}
}  // namespace

TEST_CASE("The guest formatter handles full-width integers and bounded precision",
          "[kernel][format]") {
  CHECK(Format("%I64u", {UINT64_MAX}) == "18446744073709551615");
  CHECK(Format("%I64d", {0x8000000000000000ull}) == "-9223372036854775808");
  CHECK(Format("%I64x", {UINT64_MAX}) == "ffffffffffffffff");
  CHECK(Format("%llu", {0x1234567800000009ull}) == "9");
  CHECK(Format("%.512d", {1}).size() == 512);
  CHECK(Format("%#.512o", {1}).size() == 512);
  CHECK(Format("%.999999f", {std::bit_cast<uint64_t>(1.0)}).size() == 514);
  CHECK(Format("ab%Qcd", {}) == "abQcd");
}

TEST_CASE("Counted printf strings use their byte length without a terminator", "[kernel][format]") {
  alignas(16) std::array<uint8_t, 128> memory{};
  auto* length = reinterpret_cast<rex::be<uint16_t>*>(memory.data() + 16);
  auto* pointer = reinterpret_cast<rex::be<uint32_t>*>(memory.data() + 20);
  *length = 3;
  *pointer = 32;
  std::memcpy(memory.data() + 32, "abcd", 4);
  CHECK(Format("[%5Z]", {16}, memory.data()) == "[  abc]");
  CHECK(Format("%Z", {0}, memory.data()) == "(null)");
  *length = 4;
  auto* wide = reinterpret_cast<rex::be<uint16_t>*>(memory.data() + 32);
  wide[0] = 'A';
  wide[1] = 'B';
  CHECK(Format("%wZ", {16}, memory.data()) == "AB");
}
