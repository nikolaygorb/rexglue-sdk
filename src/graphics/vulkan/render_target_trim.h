#pragma once
/**
 * @file        graphics/vulkan/render_target_trim.h
 * @brief       When to free the cached render targets that hold no EDRAM data
 *              (vulkan_render_target_trim_percent)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *              Ported for the ReXGlue fork from ForzaHorizon1-ReImagined SDK
 *              patch 0112 (MIT, Copyright (c) 2026 Dyotson and Forza Horizon 1:
 *              ReImagined contributors).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>

namespace rex::graphics::vulkan::render_target_trim {

// The render target cache keeps one image per EDRAM layout for the whole
// session; at resolution scale 3 each can be about 90 MB, and after hours of
// play device memory runs out (render targets and resolves fail).

// Frames between budget queries, and before the next trim (a trim waits for
// the GPU).
constexpr uint64_t kCheckInterval = 30;
constexpr uint64_t kAfterFailure = 60;
constexpr uint64_t kAfterTrim = 300;
constexpr uint64_t kAfterSmallTrim = 1800;
constexpr uint64_t kSmallTrimBytes = uint64_t(256) << 20;

// Called at every frame end. True: query the memory budget now (after a failed
// allocation, trim whatever it says).
inline bool ShouldQuery(uint64_t frame, uint64_t& next_check_frame, uint64_t next_allowed_frame,
                        uint32_t percent, bool allocation_failed, bool have_budget) {
  if (!percent || frame < next_allowed_frame) {
    return false;
  }
  if (allocation_failed) {
    return true;
  }
  if (!have_budget || frame < next_check_frame) {
    return false;
  }
  next_check_frame = frame + kCheckInterval;
  return true;
}

inline bool OverBudget(uint64_t usage, uint64_t budget, uint32_t percent) {
  return percent && budget && usage * 100 >= budget * percent;
}

// The memory figures a trim decision uses (FH1 0138).
struct Memory {
  uint64_t usage = 0;  // device-local, all heaps
  uint64_t budget = 0;
  bool heap_over_budget = false;  // a device-local heap at or over its own whole budget
  uint64_t system_usage = 0;      // the system memory heap
  uint64_t system_budget = 0;
  uint64_t render_targets_in_system = 0;  // render target memory placed in system memory
};

enum class Reason { kNone, kAllocationFailed, kMemoryFull, kRenderTargetsInSystemMemory };

// At most this much render target memory in system memory on an integrated GPU
// before a trim: 512 MB, or an eighth of the system heap's budget when that is
// more. Above the carve-out the cache's images go to system RAM.
inline uint64_t RenderTargetsInSystemLimit(uint64_t system_budget) {
  const uint64_t floor = uint64_t(512) << 20;
  return system_budget / 8 > floor ? system_budget / 8 : floor;
}

// Whether to trim now, and why. On integrated GPUs with a small carve-out
// (memory_placement.h) new textures, and render targets and guest memory pages
// once a device-local heap is half full, go to system memory instead of
// overfilling it, so a nearly full device-local heap is no reason to free render
// targets there. There a trim follows a device-local heap over its whole budget
// (memory that could not move), the system memory heap reaching `percent` of its
// budget, or the render targets in system memory passing their limit. Elsewhere
// the rule is the device-local budget alone.
inline Reason ShouldTrim(bool integrated, bool allocation_failed, const Memory& m,
                         uint32_t percent) {
  if (allocation_failed) {
    return Reason::kAllocationFailed;
  }
  if (!integrated) {
    return OverBudget(m.usage, m.budget, percent) ? Reason::kMemoryFull : Reason::kNone;
  }
  if (m.heap_over_budget || OverBudget(m.system_usage, m.system_budget, percent)) {
    return Reason::kMemoryFull;
  }
  if (m.render_targets_in_system >= RenderTargetsInSystemLimit(m.system_budget)) {
    return Reason::kRenderTargetsInSystemMemory;
  }
  return Reason::kNone;
}

// The first frame of the next trim. Little freed: the memory is full of other
// things, so wait longer before the next GPU wait.
inline uint64_t NextAllowedFrame(uint64_t frame, bool after_failure, uint64_t freed_bytes) {
  return frame + (freed_bytes < kSmallTrimBytes ? kAfterSmallTrim
                  : after_failure              ? kAfterFailure
                                               : kAfterTrim);
}

}  // namespace rex::graphics::vulkan::render_target_trim
