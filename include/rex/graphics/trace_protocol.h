/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Own single-frame format for ReXGlue
 */

#pragma once

#include <cstdint>

namespace rex::graphics {

// A frame trace holds everything the GPU read while drawing one frame: the
// packets it executed, the guest memory they read, and the GPU state at the
// start of the frame. Replaying it draws the same frame without the title.

inline constexpr char kTraceExtension[] = "xtr";
inline constexpr uint32_t kTraceMagic = 0x52545852;  // "RXTR"
// Bump on any change to the format or to what is recorded.
inline constexpr uint32_t kTraceFormatVersion = 1;

struct TraceHeader {
  uint32_t magic;
  uint32_t version;
  // 0 if the title is unknown.
  uint32_t title_id;
  uint32_t reserved;
};
static_assert(sizeof(TraceHeader) == 16);

enum class TraceCommandType : uint32_t {
  // The dwords of a packet, big-endian, header first. Executed at kPacketEnd,
  // after the memory reads recorded between the two.
  kPacketStart,
  kPacketEnd,
  // Guest physical memory at `address`.
  kMemoryRead,
  // EDRAM contents as tiles of row-major samples, the size of the EDRAM.
  kEdramSnapshot,
  // Register values from register `address`.
  kRegisters,
  // 256 DC_LUT_30_COLOR then 128 interleaved RGB DC_LUT_PWL_DATA; `param` is
  // the component the next gamma ramp access goes to.
  kGammaRamp,
  // Active shader ucode, big-endian; `param` is the xenos::ShaderType.
  kShader,
  // The swap that ends the frame.
  kSwap,
};

// Followed by `size` bytes of data, padded to 4 bytes.
struct TraceCommand {
  TraceCommandType type;
  uint32_t address;
  uint32_t param;
  uint32_t size;
};
static_assert(sizeof(TraceCommand) == 16);

}  // namespace rex::graphics
