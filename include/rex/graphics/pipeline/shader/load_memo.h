/**
 * @file        rex/graphics/pipeline/shader/load_memo.h
 * @brief       The shader last loaded from each guest address
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *              Ported for the ReXGlue fork from ForzaHorizon1-ReImagined SDK
 *              patch 0140 (MIT, Copyright (c) 2026 Dyotson and Forza Horizon 1:
 *              ReImagined contributors).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics {

// Games load a vertex and a pixel shader for nearly every draw (PM4_IM_LOAD),
// almost always the same few hundred, and each load hashes the microcode and
// looks the hash up. A load whose address, type and words match the last load
// from that address gets that load's shader: the one the hash would find, as
// long as the pipeline cache keeps its shaders (Clear when it deletes any).
//
// On a miss the words are copied into the slot first, and the hash, the
// shader's own microcode and the slot's key all come from that copy, so a guest
// write during the load cannot pair new words with the old shader.
//
// Used on the command processor thread only.
template <typename ShaderT, size_t kSlotCount = 256>
class ShaderLoadMemo {
  static_assert(kSlotCount && !(kSlotCount & (kSlotCount - 1)), "power of two");

 public:
  // load(words) loads the shader from a stable copy of the words.
  template <typename Load>
  ShaderT* FindOrLoad(xenos::ShaderType type, const uint32_t* words, uint32_t dword_count,
                      Load&& load) {
    Slot& slot = slots_[Index(words)];
    if (slot.shader && slot.address == words && slot.type == type &&
        slot.words.size() == dword_count &&
        (!dword_count || !std::memcmp(slot.words.data(), words, dword_count * sizeof(uint32_t)))) {
      return slot.shader;
    }
    slot.shader = nullptr;
    slot.words.assign(words, words + dword_count);
    ShaderT* shader = load(dword_count ? slot.words.data() : words);
    slot.address = words;
    slot.type = type;
    slot.shader = shader;
    return shader;
  }

  void Clear() {
    for (Slot& slot : slots_) {
      slot.shader = nullptr;
    }
  }

 private:
  struct Slot {
    const uint32_t* address = nullptr;
    xenos::ShaderType type = xenos::ShaderType::kVertex;
    std::vector<uint32_t> words;
    ShaderT* shader = nullptr;
  };
  static size_t Index(const uint32_t* address) {
    uint64_t key =
        uint64_t(reinterpret_cast<uintptr_t>(address) >> 2) * UINT64_C(0x9E3779B97F4A7C15);
    return size_t(key >> 40) & (kSlotCount - 1);
  }
  std::array<Slot, kSlotCount> slots_;
};

}  // namespace rex::graphics
