/**
 * @file        rex/chrono/guest_tick_accumulator.h
 * @brief       Host tick count to guest tick count, forwards only, remainder carried
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

namespace rex::chrono {

// The guest clock advances by the host ticks since the last reading, scaled by
// numerator / denominator. Threads read the host count before taking the clock's
// lock, so a reading can be older than one already counted: it is ignored, as
// storing it made the next update count the same stretch twice (the guest clock
// ran fast by 0.03-0.53 %, different on every PC; Perfect Dark XBLA PC Recomp).
// The division's remainder is carried into the next update instead of dropped,
// which ran the clock slow on host counters that don't divide the guest rate.
struct GuestTickAccumulator {
  uint64_t last_host_ticks = 0;
  uint64_t guest_ticks = 0;
  // In units of 1 / denominator of a guest tick.
  uint64_t remainder = 0;

  uint64_t Advance(uint64_t host_ticks, uint64_t numerator, uint64_t denominator) {
    if (host_ticks > last_host_ticks) {
      const uint64_t scaled = (host_ticks - last_host_ticks) * numerator + remainder;
      last_host_ticks = host_ticks;
      guest_ticks += scaled / denominator;
      remainder = scaled % denominator;
    }
    return guest_ticks;
  }
};

}  // namespace rex::chrono
