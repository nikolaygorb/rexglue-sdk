/**
 * @file        graphics/pm4_event_write_test.cpp
 * @brief       PM4 event write packets in the disassembler: the cache flush
 *              done event (EVENT_WRITE_CFL) has EVENT_WRITE_SHD's layout
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/xenos.h>

namespace {

using rex::graphics::PacketDisassembler;
using rex::graphics::PacketInfo;

constexpr uint32_t Type3(uint32_t opcode, uint32_t count) {
  return 0xC0000000u | ((count - 1) << 16) | (opcode << 8);
}

// The packet as the guest writes it: big-endian words.
std::array<uint8_t, 16> Packet(uint32_t opcode) {
  const uint32_t words[] = {Type3(opcode, 3), 0x00000014u, 0x1F000002u, 0x12345678u};
  std::array<uint8_t, 16> bytes{};
  for (size_t i = 0; i < 4; ++i) {
    for (size_t b = 0; b < 4; ++b) {
      bytes[i * 4 + b] = uint8_t(words[i] >> (24 - 8 * b));
    }
  }
  return bytes;
}

}  // namespace

TEST_CASE("PM4 EVENT_WRITE_CFL is a known packet with SHD's size", "[graphics][pm4]") {
  for (const auto& [opcode, name] :
       {std::pair{uint32_t(rex::graphics::xenos::PM4_EVENT_WRITE_SHD), "PM4_EVENT_WRITE_SHD"},
        std::pair{uint32_t(rex::graphics::xenos::PM4_EVENT_WRITE_CFL), "PM4_EVENT_WRITE_CFL"}}) {
    INFO(name);
    const auto bytes = Packet(opcode);
    PacketInfo info{};
    REQUIRE(PacketDisassembler::DisasmPacket(bytes.data(), &info));
    REQUIRE(info.type_info != nullptr);
    CHECK(std::string(info.type_info->name) == name);
    CHECK(info.count == 4);  // header + initiator, address, value
  }
}
