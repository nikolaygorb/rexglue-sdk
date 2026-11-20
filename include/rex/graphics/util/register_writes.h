#pragma once
/**
 * @file        rex/graphics/util/register_writes.h
 * @brief       Register range writes that store only the values that change,
 *              shared by the Vulkan and Direct3D 12 command processors
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *              Idea from ReIGlue 0426 (gpu_elide_unchanged_registers, FH1 #607).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <rex/graphics/registers.h>
#include <rex/types.h>

namespace rex::graphics {

// The shader constant blocks: float (0x4000...0x47FF), fetch (0x4800...0x48BF),
// bool and loop (0x4900...0x4927). Their handlers depend on the values alone.
constexpr bool IsShaderConstantRegister(uint32_t index) {
  return (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
          index <= XE_GPU_REG_SHADER_CONSTANT_511_W) ||
         (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
          index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) ||
         (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
          index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31);
}

// Stores count big-endian dwords of source into values (host byte order), only
// where they differ from what values holds. Games rewrite most of their shader
// constants with the values they already have, and every write the command
// processor sees as a change makes the next draw upload a constant buffer
// again. Returns whether any value changed; if one did, first_changed and
// last_changed are the first and last changed index (relative to values).
// Swapped and compared 64 at a time; a chunk that differs is scanned for its
// exact first and last change.
inline bool StoreChangedRegisters(uint32_t* values, const uint32_t* source, uint32_t count,
                                  uint32_t& first_changed, uint32_t& last_changed) {
  constexpr uint32_t kChunk = 64;
  uint32_t swapped[kChunk];
  bool any_changed = false;
  for (uint32_t chunk = 0; chunk < count; chunk += kChunk) {
    const uint32_t chunk_count = std::min(kChunk, count - chunk);
    for (uint32_t i = 0; i < chunk_count; ++i) {
      swapped[i] = byte_swap(source[chunk + i]);
    }
    uint32_t* chunk_values = values + chunk;
    if (!std::memcmp(chunk_values, swapped, sizeof(uint32_t) * chunk_count)) {
      continue;
    }
    uint32_t first = 0;
    while (chunk_values[first] == swapped[first]) {
      ++first;
    }
    uint32_t last = chunk_count - 1;
    while (chunk_values[last] == swapped[last]) {
      --last;
    }
    std::memcpy(chunk_values + first, swapped + first, sizeof(uint32_t) * (last - first + 1));
    if (!any_changed) {
      first_changed = chunk + first;
      any_changed = true;
    }
    last_changed = chunk + last;
  }
  return any_changed;
}

}  // namespace rex::graphics
