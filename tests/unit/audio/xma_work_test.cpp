/**
 * @file        audio/xma_work_test.cpp
 * @brief       Tests for XmaContext::Work on contexts that never give output
 *
 * A regression in the first two tests hangs instead of failing.
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <rex/audio/xma/context.h>
#include <rex/system/xmemory.h>

#include "test_memory.h"

namespace {

using rex::audio::XMA_CONTEXT_DATA;
using rex::audio::XmaContext;

constexpr uint32_t kOutputBlocks = 8;
// BitStream::Peek reads 8 bytes at a time.
constexpr uint32_t kSlackBytes = 16;
constexpr uint8_t kZeroContext[sizeof(XMA_CONTEXT_DATA)] = {};

void WriteBits(uint8_t* data, uint32_t offset, uint32_t count, uint32_t value) {
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t bit = offset + i;
    uint8_t mask = uint8_t(0x80 >> (bit & 7));
    if ((value >> (count - 1 - i)) & 1) {
      data[bit >> 3] |= mask;
    } else {
      data[bit >> 3] &= ~mask;
    }
  }
}

// An XMA2 packet header: frames counted and where the first one starts.
void SetPacketHeader(uint8_t* packet, uint32_t frame_count, uint32_t first_frame_offset_bits) {
  const uint32_t field = first_frame_offset_bits - XmaContext::kBitsPerPacketHeader;
  packet[0] = uint8_t((frame_count << 2) | ((field >> 13) & 0x3));
  packet[1] = uint8_t(field >> 5);
  packet[2] = uint8_t(((field & 0x1F) << 3) | 1);
  packet[3] = 0;
}

// A frame of random bits (FFmpeg rejects those): its size, the body and the
// bit that says whether another frame follows in the packet.
void WriteFrame(uint8_t* packet, std::mt19937& rng, uint32_t offset, uint32_t size, bool more) {
  WriteBits(packet, offset, XmaContext::kBitsPerFrameHeader, size);
  for (uint32_t bit = offset + XmaContext::kBitsPerFrameHeader;
       bit < offset + size - 1 && bit < XmaContext::kBitsPerPacket; ++bit) {
    WriteBits(packet, bit, 1, rng() & 1);
  }
  if (offset + size <= XmaContext::kBitsPerPacket) {
    WriteBits(packet, offset + size - 1, 1, more ? 1 : 0);
  }
}

// One input packet and an output buffer in guest memory, and the context.
struct Stream {
  Stream() : memory(rex::testing::GetTestMemory()), data(kZeroContext) {
    context_address = memory.SystemHeapAlloc(sizeof(XMA_CONTEXT_DATA), 256);
    buffers_address = memory.SystemHeapAlloc(0x4000, 0x1000, rex::memory::kSystemHeapPhysical);
    REQUIRE(context_address);
    REQUIRE(buffers_address);
    buffers_physical = memory.GetPhysicalAddress(buffers_address);
    std::memset(packet(), 0, XmaContext::kBytesPerPacket + kSlackBytes);
    REQUIRE(xma.Setup(0, &memory, context_address) == 0);
    data.input_buffer_0_packet_count = 1;
    data.input_buffer_0_valid = 1;
    data.input_buffer_0_ptr = buffers_physical;
    data.output_buffer_ptr = buffers_physical + 0x1000;
    data.output_buffer_block_count = kOutputBlocks;
    data.output_buffer_valid = 1;
    data.sample_rate = 3;
    data.input_buffer_read_offset = XmaContext::kBitsPerPacketHeader;
  }
  ~Stream() {
    xma.Release();
    memory.SystemHeapFree(buffers_address);
    memory.SystemHeapFree(context_address);
  }

  uint8_t* packet() { return memory.TranslatePhysical(buffers_physical); }

  bool Kick() {
    data.Store(memory.TranslateVirtual(context_address));
    xma.set_is_allocated(true);
    xma.Enable();
    return xma.Work();
  }

  rex::memory::Memory& memory;
  uint32_t context_address = 0;
  uint32_t buffers_address = 0;
  uint32_t buffers_physical = 0;
  XMA_CONTEXT_DATA data;
  XmaContext xma;
};

}  // namespace

TEST_CASE("An endless loop over rejected frames ends the kick", "[audio][xma]") {
  Stream stream;
  std::mt19937 rng(0x584D41);
  uint8_t* packet = stream.packet();
  constexpr uint32_t kFrameSize = 800;
  const uint32_t first = XmaContext::kBitsPerPacketHeader;
  SetPacketHeader(packet, 3, first);
  WriteFrame(packet, rng, first, kFrameSize, true);
  WriteFrame(packet, rng, first + kFrameSize, kFrameSize, true);
  WriteFrame(packet, rng, first + 2 * kFrameSize, kFrameSize, false);
  stream.data.loop_count = 255;
  stream.data.loop_start = first;
  stream.data.loop_end = first + 2 * kFrameSize;

  CHECK(stream.Kick());
}

TEST_CASE("A frame count beyond the packet's frames ends the kick", "[audio][xma]") {
  Stream stream;
  std::mt19937 rng(0x584D41);
  uint8_t* packet = stream.packet();
  // Crosses the packet end, and the header says frames follow it, so the next
  // offset wraps back to the start of this packet.
  const uint32_t first = XmaContext::kBitsPerPacket - 200;
  SetPacketHeader(packet, 5, first);
  WriteFrame(packet, rng, first, 1000, true);

  CHECK(stream.Kick());
}

TEST_CASE("An output buffer of no blocks is not decoded into", "[audio][xma]") {
  Stream stream;
  stream.data.output_buffer_block_count = 0;

  CHECK(stream.Kick());
}
