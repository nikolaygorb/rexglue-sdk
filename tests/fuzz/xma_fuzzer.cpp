/**
 * @file        fuzz/xma_fuzzer.cpp
 * @brief       libFuzzer target for the XMA decoder context: the guest's
 *              context data, its packets and frames
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/audio/xma/context.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

extern "C" {
#include "libavutil/log.h"
}  // extern "C"

using rex::audio::XMA_CONTEXT_DATA;
using rex::audio::XmaContext;

// Input layout:
//   0: bits 0..2  - kicks (XmaContext::Work calls) - 1;
//      bit 3      - between kicks, the guest marks both input buffers valid
//                   again (refilled with the same packets);
//      bit 4      - between kicks, the guest takes all the output (read offset
//                   = write offset) and marks the output buffer valid again;
//      bit 5      - between kicks, the guest flips is_stereo;
//      bit 6, 7   - input buffer 0, 1 at address 0;
//   1..64         - the context as the guest stores it (big endian), XOR one
//                   that decodes 2 packets of input buffer 0 into 8 output
//                   blocks; packet counts are taken modulo kMaxPackets + 1;
//   then          - the packets of input buffer 0, then of input buffer 1
//                   (2048 bytes each), missing bytes are 0.
// The buffer addresses are the fuzzer's. tools/fuzz/make_xma_corpus.py writes
// inputs with well-formed packet headers and frame chains.
//
// The context is kicked like by the XMA decoder thread, then released (which
// resets the decoder for the next input). Checked on top of ASan: the input
// buffers and the output buffer end where the context says, at a page the
// fuzzer made inaccessible (one is also in front of each), so reading packets
// or writing output past them crashes. BitStream::Peek loads 8 bytes at a time
// by design, so the input buffers end kPeekSlack zero bytes before theirs.

namespace {

constexpr uint32_t kMaxPackets = 16;
constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kPeekSlack = 16;
// Input buffer 0, input buffer 1, output buffer: a guard page, the data, a
// guard page.
constexpr uint32_t kSlotSize = 0x10000;
constexpr uint32_t kSlotCount = 3;
static_assert(kMaxPackets * XmaContext::kBytesPerPacket + kPeekSlack <= kSlotSize - 2 * kPageSize);
static_assert(XmaContext::kOutputMaxSizeBytes <= kSlotSize - 2 * kPageSize);

struct Guest {
  rex::memory::Memory memory;
  uint32_t context_address = 0;
  uint32_t slots_physical = 0;
  XmaContext context;

  // The end of a slot's data, where the guard page starts.
  uint32_t SlotDataEnd(uint32_t slot) const {
    return slots_physical + (slot + 1) * kSlotSize - kPageSize;
  }
};

void Guard(Guest& guest, uint32_t physical) {
  if (!rex::memory::Protect(guest.memory.TranslatePhysical(physical), kPageSize,
                            rex::memory::PageAccess::kNoAccess)) {
    std::fprintf(stderr, "xma_fuzzer: can't protect a guard page\n");
    std::abort();
  }
}

Guest* GetGuest() {
  static Guest* guest = [] {
    // Most fuzzed frames are rejected, each with a message on stderr.
    av_log_set_level(AV_LOG_QUIET);
    auto* guest = new Guest;
    if (!guest->memory.Initialize()) {
      std::fprintf(stderr, "xma_fuzzer: can't reserve the guest address space\n");
      std::abort();
    }
    guest->context_address = guest->memory.SystemHeapAlloc(sizeof(XMA_CONTEXT_DATA), 256);
    uint32_t slots_address = guest->memory.SystemHeapAlloc(kSlotSize * kSlotCount, kSlotSize,
                                                           rex::memory::kSystemHeapPhysical);
    if (!guest->context_address || !slots_address) {
      std::fprintf(stderr, "xma_fuzzer: can't allocate guest memory\n");
      std::abort();
    }
    guest->slots_physical = guest->memory.GetPhysicalAddress(slots_address);
    for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
      Guard(*guest, guest->slots_physical + slot * kSlotSize);
      Guard(*guest, guest->SlotDataEnd(slot));
    }
    if (guest->context.Setup(0, &guest->memory, guest->context_address)) {
      std::fprintf(stderr, "xma_fuzzer: can't set up the XMA context\n");
      std::abort();
    }
    return guest;
  }();
  return guest;
}

XMA_CONTEXT_DATA DefaultContext() {
  const uint8_t zero[sizeof(XMA_CONTEXT_DATA)] = {};
  XMA_CONTEXT_DATA data(zero);
  data.input_buffer_0_packet_count = 2;
  data.input_buffer_0_valid = 1;
  data.output_buffer_block_count = 8;
  data.sample_rate = 3;
  data.output_buffer_valid = 1;
  data.input_buffer_read_offset = XmaContext::kBitsPerPacketHeader;
  return data;
}

uint8_t* Context(Guest& guest) {
  return guest.memory.TranslateVirtual(guest.context_address);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  constexpr size_t kHeaderSize = 1 + sizeof(XMA_CONTEXT_DATA);
  if (size < kHeaderSize) {
    return 0;
  }
  Guest* guest = GetGuest();
  uint8_t flags = data[0];
  uint32_t kicks = (flags & 7) + 1;

  static const XMA_CONTEXT_DATA default_context = DefaultContext();
  uint8_t stored[sizeof(XMA_CONTEXT_DATA)];
  XMA_CONTEXT_DATA initial = default_context;
  initial.Store(stored);
  for (size_t i = 0; i < sizeof(stored); ++i) {
    stored[i] ^= data[1 + i];
  }
  data += kHeaderSize;
  size -= kHeaderSize;

  XMA_CONTEXT_DATA context(stored);
  context.input_buffer_0_packet_count %= kMaxPackets + 1;
  context.input_buffer_1_packet_count %= kMaxPackets + 1;
  for (uint8_t buffer = 0; buffer < 2; ++buffer) {
    uint32_t bytes = context.GetInputBufferPacketCount(buffer) * XmaContext::kBytesPerPacket;
    uint32_t address = guest->SlotDataEnd(buffer) - kPeekSlack - bytes;
    uint8_t* packets = guest->memory.TranslatePhysical(address);
    size_t copied = std::min(size_t(bytes), size);
    std::memcpy(packets, data, copied);
    std::memset(packets + copied, 0, bytes - copied + kPeekSlack);
    data += copied;
    size -= copied;
    if (flags & (1 << (6 + buffer))) {
      address = 0;
    }
    (buffer ? context.input_buffer_1_ptr : context.input_buffer_0_ptr) = address;
  }
  uint32_t output_bytes = context.output_buffer_block_count * XmaContext::kOutputBytesPerBlock;
  context.output_buffer_ptr = guest->SlotDataEnd(2) - output_bytes;
  std::memset(guest->memory.TranslatePhysical(context.output_buffer_ptr), 0, output_bytes);
  context.Store(Context(*guest));

  XmaContext& xma = guest->context;
  xma.set_is_allocated(true);
  for (uint32_t kick = 0; kick < kicks; ++kick) {
    if (kick) {
      // What the title does between kicks.
      XMA_CONTEXT_DATA between(Context(*guest));
      if (flags & (1 << 3)) {
        between.input_buffer_0_valid = 1;
        between.input_buffer_1_valid = 1;
      }
      if (flags & (1 << 4)) {
        between.output_buffer_read_offset = between.output_buffer_write_offset;
        between.output_buffer_valid = 1;
      }
      if (flags & (1 << 5)) {
        between.is_stereo ^= 1;
      }
      between.Store(Context(*guest));
    }
    xma.Enable();
    xma.Work();
  }
  xma.Release();
  return 0;
}
