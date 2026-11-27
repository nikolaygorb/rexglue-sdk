/**
 * @file        core/guest_tick_accumulator_test.cpp
 * @brief       Tests that the guest clock only moves forwards and keeps the conversion remainder
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/chrono/guest_tick_accumulator.h>

using rex::chrono::GuestTickAccumulator;

TEST_CASE("An older host reading doesn't count an interval twice", "[core][chrono]") {
  GuestTickAccumulator ticks{1000};
  CHECK(ticks.Advance(1100, 1, 1) == 100);
  // A thread that read 1050 before the lock, after another thread counted 1100.
  CHECK(ticks.Advance(1050, 1, 1) == 100);
  // The stretch 1050-1100 was counted twice before (100 + 0 + 100 = 200 at 1200).
  CHECK(ticks.Advance(1200, 1, 1) == 200);
}

TEST_CASE("The conversion remainder is carried into the next update", "[core][chrono]") {
  // 50 MHz guest from a 10 MHz host counter: 5 / 1 is exact; 50 MHz from 3 MHz
  // is 50 / 3, so 1 host tick is 16.67 guest ticks.
  GuestTickAccumulator ticks{0};
  for (uint64_t host = 1; host <= 3000; ++host) {
    ticks.Advance(host, 50, 3);
  }
  // Dropping the remainder gave 16 a tick: 48,000.
  CHECK(ticks.guest_ticks == 50000);
  CHECK(ticks.remainder == 0);
}

TEST_CASE("The same reading leaves the clock as it is", "[core][chrono]") {
  GuestTickAccumulator ticks{10};
  CHECK(ticks.Advance(20, 7, 2) == 35);
  CHECK(ticks.Advance(20, 7, 2) == 35);
  CHECK(ticks.remainder == 0);
  CHECK(ticks.Advance(21, 7, 2) == 38);
  CHECK(ticks.remainder == 1);
}
