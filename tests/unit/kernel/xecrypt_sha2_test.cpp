#include <catch2/catch_test_macros.hpp>
#include <rex/kernel/xboxkrnl/xecrypt_sha2.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
using namespace rex::kernel::xboxkrnl;

TEST_CASE("SHA-256 Final preserves the console context", "[kernel][crypt]") {
  struct Case {
    uint32_t size;
    const char* hex;
  };
  const Case cases[] = {
      {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {3, "9834876dcfb05cb167a5c24953eba58c4ac89b1adf57f28f2f9d09af107ee8f0"},
      {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
      {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
      {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
      {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
      {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
  };
  for (const auto& item : cases) {
    CAPTURE(item.size);
    std::string message(item.size, 'a');
    XECRYPT_SHA256_STATE state{};
    auto ptr = ppc_ptr_t<XECRYPT_SHA256_STATE>::from_host(&state);
    XeCryptSha256Init_entry(ptr);
    XeCryptSha256Update_entry(ptr, mapped_void::from_host(message.data()), item.size);
    std::array<uint8_t, 33> output{};
    output.back() = 0xA5;
    XeCryptSha256Final_entry(ptr, ppc_ptr_t<uint8_t>::from_host(output.data()), 32);
    CHECK(uint32_t(state.count) == item.size);
    CHECK(output.back() == 0xA5);
    CHECK(std::memcmp(output.data(), state.state, 32) == 0);
    for (size_t i = 0; i < 32; ++i) {
      const uint8_t expected =
          static_cast<uint8_t>(std::stoul(std::string(item.hex + 2 * i, 2), nullptr, 16));
      CHECK(output[i] == expected);
    }
    CHECK(state.buffer[63] == uint8_t(item.size << 3));
    CHECK(state.buffer[62] == uint8_t(item.size >> 5));
    CHECK(state.buffer[59] == 0);
    auto before = state;
    XeCryptSha256Final_entry(ptr, {}, 0);
    CHECK(uint32_t(state.count) == item.size);
    CHECK(std::memcmp(before.state, state.state, sizeof(state.state)) != 0);
  }
}

TEST_CASE("SHA-512 Final preserves the console context", "[kernel][crypt]") {
  struct Case {
    uint32_t size;
    const char* hex;
  };
  const Case cases[] = {
      {0,
       "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877"
       "eec2f63b931bd47417a81a538327af927da3e"},
      {3,
       "d6f644b19812e97b5d871658d6d3400ecd4787faeb9b8990c1e7608288664be77257104a58d033bcf1a0e0945ff"
       "06468ebe53e2dff36e248424c7273117dac09"},
      {111,
       "fa9121c7b32b9e01733d034cfc78cbf67f926c7ed83e82200ef86818196921760b4beff48404df811b953828274"
       "461673c68d04e297b0eb7b2b4d60fc6b566a2"},
      {112,
       "c01d080efd492776a1c43bd23dd99d0a2e626d481e16782e75d54c2503b5dc32bd05f0f1ba33e568b88fd2d9709"
       "29b719ecbb152f58f130a407c8830604b70ca"},
      {127,
       "828613968b501dc00a97e08c73b118aa8876c26b8aac93df128502ab360f91bab50a51e088769a5c1eff4782ace"
       "147dce3642554199876374291f5d921629502"},
      {128,
       "b73d1929aa615934e61a871596b3f3b33359f42b8175602e89f7e06e5f658a243667807ed300314b95cacdd579f"
       "3e33abdfbe351909519a846d465c59582f321"},
      {129,
       "4f681e0bd53cda4b5a2041cc8a06f2eabde44fb16c951fbd5b87702f07aeab611565b19c47fde30587177ebb852"
       "e3971bbd8d3fd30da18d71037dfbd98420429"},
  };
  for (const auto& item : cases) {
    CAPTURE(item.size);
    std::string message(item.size, 'a');
    XECRYPT_SHA512_STATE state{};
    auto ptr = ppc_ptr_t<XECRYPT_SHA512_STATE>::from_host(&state);
    XeCryptSha512Init_entry(ptr);
    state.pad = 0x12345678;
    XeCryptSha512Update_entry(ptr, mapped_void::from_host(message.data()), item.size);
    std::array<uint8_t, 65> output{};
    output.back() = 0xA5;
    XeCryptSha512Final_entry(ptr, ppc_ptr_t<uint8_t>::from_host(output.data()), 64);
    CHECK(uint32_t(state.count) == item.size);
    CHECK(output.back() == 0xA5);
    CHECK(std::memcmp(output.data(), state.state, 64) == 0);
    for (size_t i = 0; i < 64; ++i) {
      const uint8_t expected =
          static_cast<uint8_t>(std::stoul(std::string(item.hex + 2 * i, 2), nullptr, 16));
      CHECK(output[i] == expected);
    }
    CHECK(state.buffer[127] == uint8_t(item.size << 3));
    CHECK(state.buffer[126] == uint8_t(item.size >> 5));
    CHECK(state.buffer[123] == 0);
    auto before = state;
    XeCryptSha512Final_entry(ptr, {}, 0);
    CHECK(uint32_t(state.count) == item.size);
    CHECK(std::memcmp(before.state, state.state, sizeof(state.state)) != 0);
    CHECK(state.pad == 0x12345678);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&state);
    CHECK(bytes[0] == 0);
    CHECK(bytes[3] == uint8_t(item.size));
  }
}
