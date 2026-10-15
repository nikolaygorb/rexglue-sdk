/**
 * @file        audio/xma_packet_test.cpp
 * @brief       Tests for XMA packet parsing in XmaContext
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <rex/audio/xma/context.h>

namespace {

using rex::audio::XmaContext;

// BitStream::Peek reads 8 bytes at a time, so keep some slack past the end.
constexpr size_t kSlackBytes = 8;

std::vector<uint8_t> MakePackets(uint32_t count) {
  return std::vector<uint8_t>(count * XmaContext::kBytesPerPacket + kSlackBytes, 0);
}

uint8_t* Packet(std::vector<uint8_t>& buffer, uint32_t index) {
  return buffer.data() + index * XmaContext::kBytesPerPacket;
}

// Packet header: 6-bit frame count, 15-bit first frame offset (in bits, minus
// the 32-bit header), 3-bit metadata, 8-bit skip count.
void SetHeader(uint8_t* packet, uint32_t first_frame_offset_bits, uint8_t skip_count,
               uint8_t metadata = 0) {
  const uint32_t field = first_frame_offset_bits - XmaContext::kBitsPerPacketHeader;
  packet[0] = static_cast<uint8_t>((field >> 13) & 0x3);
  packet[1] = static_cast<uint8_t>(field >> 5);
  packet[2] = static_cast<uint8_t>(((field & 0x1F) << 3) | (metadata & 0x7));
  packet[3] = skip_count;
}

// No frame starts in this packet: the offset field is all ones.
void SetHeaderNoFrameStart(uint8_t* packet, uint8_t skip_count) {
  packet[0] = 0x3;
  packet[1] = 0xFF;
  packet[2] = 0xF8;
  packet[3] = skip_count;
}

// Writes `value` as `num_bits` bits starting at bit `offset`, most significant first.
void WriteBits(uint8_t* packet, uint32_t offset, uint32_t num_bits, uint32_t value) {
  for (uint32_t i = 0; i < num_bits; i++) {
    const uint32_t bit = offset + i;
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (bit & 7));
    if ((value >> (num_bits - 1 - i)) & 1) {
      packet[bit >> 3] |= mask;
    } else {
      packet[bit >> 3] &= static_cast<uint8_t>(~mask);
    }
  }
}

}  // namespace

TEST_CASE("A frame whose header crosses the packet end is counted", "[audio][xma]") {
  auto buffer = MakePackets(1);
  uint8_t* packet = Packet(buffer, 0);
  SetHeader(packet, XmaContext::kBitsPerPacketHeader, 0);

  // The first frame ends 5 bits before the packet does, so the next frame's
  // 15-bit size header runs into the next packet.
  const uint32_t first = XmaContext::kBitsPerPacketHeader;
  const uint32_t second = XmaContext::kBitsPerPacket - 5;
  const uint32_t first_size = second - first;
  WriteBits(packet, first, XmaContext::kBitsPerFrameHeader, first_size);
  WriteBits(packet, second - 1, 1, 1);  // Another frame follows.

  const auto first_info = XmaContext::GetPacketInfo(packet, first);
  CHECK(first_info.frame_count_ == 2);
  CHECK(first_info.current_frame_ == 0);
  CHECK(first_info.current_frame_size_ == first_size);
  // Not the last frame, so the decoder does not jump past the second one.
  CHECK_FALSE(first_info.isLastFrameInPacket());

  const auto second_info = XmaContext::GetPacketInfo(packet, second);
  CHECK(second_info.frame_count_ == 2);
  CHECK(second_info.current_frame_ == 1);
  // Size 0 sends the frame to the split-header path.
  CHECK(second_info.current_frame_size_ == 0);
}

TEST_CASE("The next frame start follows the sub-stream's skip chain", "[audio][xma]") {
  auto buffer = MakePackets(4);
  // Packet 1 only continues a frame; its skip count points at packet 3.
  // Packet 2 belongs to another sub-stream and must not be picked.
  SetHeaderNoFrameStart(Packet(buffer, 1), 1);
  SetHeader(Packet(buffer, 2), 100, 0);
  SetHeader(Packet(buffer, 3), 200, 0);

  CHECK(XmaContext::GetNextPacketReadOffset(buffer.data(), 1, 4) ==
        3 * XmaContext::kBitsPerPacket + 200);
}

TEST_CASE("The skip chain ends at a full-skip packet", "[audio][xma]") {
  auto buffer = MakePackets(3);
  SetHeaderNoFrameStart(Packet(buffer, 1), 0xFF);
  SetHeader(Packet(buffer, 2), 100, 0);

  CHECK(XmaContext::GetNextPacketReadOffset(buffer.data(), 1, 3) ==
        XmaContext::kBitsPerPacketHeader);
}

TEST_CASE("A packet with a frame start is returned directly", "[audio][xma]") {
  auto buffer = MakePackets(2);
  SetHeader(Packet(buffer, 1), 64, 0);

  CHECK(XmaContext::GetNextPacketReadOffset(buffer.data(), 1, 2) ==
        XmaContext::kBitsPerPacket + 64);
}

TEST_CASE("An offset short of a frame start resolves to that frame", "[audio][xma]") {
  auto buffer = MakePackets(1);
  uint8_t* packet = Packet(buffer, 0);
  SetHeader(packet, XmaContext::kBitsPerPacketHeader, 0);

  const uint32_t first = XmaContext::kBitsPerPacketHeader;
  const uint32_t first_size = 1000;
  const uint32_t second = first + first_size;
  WriteBits(packet, first, XmaContext::kBitsPerFrameHeader, first_size);
  WriteBits(packet, second - 1, 1, 1);  // Another frame follows.
  WriteBits(packet, second, XmaContext::kBitsPerFrameHeader, 500);

  // Titles can write loop_start one bit short of the frame start.
  const auto short_info = XmaContext::GetPacketInfo(packet, second - 1);
  CHECK(short_info.current_frame_offset_ == second);

  const auto exact_info = XmaContext::GetPacketInfo(packet, second);
  CHECK(exact_info.current_frame_offset_ == second);
  CHECK(exact_info.current_frame_ == 1);
  CHECK(exact_info.current_frame_size_ == 500);
}
