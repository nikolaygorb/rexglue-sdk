/**
 * @file        graphics/util/draw_phase_timer.h
 * @brief       frame_stats_interval: a draw's host time split by phase
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/chrono/clock.h>

namespace rex::graphics {

// Adds the host ticks between phase changes in a draw to the phase they
// belong to; the rest, up to any return, to the last one. Off (no clock
// reads) when not enabled: renderers time one draw in kDrawPhaseSampleRate
// and count its ticks that many times, as clock reads on every draw cost
// 11-14% of IssueDraw (native renderer, RDR).
template <typename Phase>
class DrawPhaseTimer {
 public:
  DrawPhaseTimer(uint64_t* ticks, bool enabled, Phase first)
      : ticks_(enabled ? ticks : nullptr), phase_(first) {
    if (ticks_) {
      last_ = rex::chrono::Clock::QueryHostTickCount();
    }
  }
  DrawPhaseTimer(const DrawPhaseTimer&) = delete;
  DrawPhaseTimer& operator=(const DrawPhaseTimer&) = delete;
  ~DrawPhaseTimer() { Next(phase_); }
  void Next(Phase phase) {
    if (!ticks_) {
      return;
    }
    uint64_t now = rex::chrono::Clock::QueryHostTickCount();
    ticks_[size_t(phase_)] += now - last_;
    last_ = now;
    phase_ = phase;
  }

 private:
  uint64_t* ticks_;
  Phase phase_;
  uint64_t last_ = 0;
};

}  // namespace rex::graphics
