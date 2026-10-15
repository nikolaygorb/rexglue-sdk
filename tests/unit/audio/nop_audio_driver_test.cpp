/**
 * @file        audio/nop_audio_driver_test.cpp
 * @brief       Tests for the null audio backend's driver
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <rex/audio/nop/nop_audio_driver.h>
#include <rex/thread.h>

TEST_CASE("The null audio driver releases a slot for every frame", "[audio]") {
  auto semaphore = rex::thread::Semaphore::Create(0, 4);
  REQUIRE(semaphore);
  rex::audio::nop::NopAudioDriver driver(nullptr, semaphore.get());

  // No slot is free before a frame is submitted.
  CHECK(rex::thread::Wait(semaphore.get(), false, std::chrono::milliseconds(0)) ==
        rex::thread::WaitResult::kTimeout);

  driver.SubmitFrame(0);
  driver.SubmitFrame(0);
  CHECK(rex::thread::Wait(semaphore.get(), false, std::chrono::milliseconds(0)) ==
        rex::thread::WaitResult::kSuccess);
  CHECK(rex::thread::Wait(semaphore.get(), false, std::chrono::milliseconds(0)) ==
        rex::thread::WaitResult::kSuccess);
  CHECK(rex::thread::Wait(semaphore.get(), false, std::chrono::milliseconds(0)) ==
        rex::thread::WaitResult::kTimeout);
}
