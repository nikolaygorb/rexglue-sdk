/**
 * @file        tests/unit/core/fiber_test.cpp
 * @brief       Unit tests for rex::thread::Fiber
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>
#include <rex/thread/fiber.h>

#include <latch>
#include <thread>

using rex::thread::Fiber;

// Globals - fiber entry functions cannot capture closures.
static Fiber* s_main = nullptr;
static int s_count = 0;

static void counting_fiber(void*) {
  ++s_count;  // first resume
  Fiber::SwitchTo(s_main);
  ++s_count;  // second resume
  Fiber::SwitchTo(s_main);
}

TEST_CASE("rex::thread::Fiber - basic context switch", "[fiber]") {
  s_count = 0;
  s_main = Fiber::ConvertCurrentThread();
  auto* f = Fiber::Create(256 * 1024, counting_fiber, nullptr);

  REQUIRE(s_count == 0);
  Fiber::SwitchTo(f);
  REQUIRE(s_count == 1);  // fiber ran, switched back
  Fiber::SwitchTo(f);
  REQUIRE(s_count == 2);  // fiber resumed, ran again, switched back

  // NOTE: Cleanup is not exception-safe. If a REQUIRE above fires, Catch2
  // throws and these Destroy() calls are skipped, leaking the host fiber
  // handles. Acceptable for a test-only scenario; fix if spurious failures occur.
  f->Destroy();
  s_main->Destroy();
  s_main = nullptr;
}

// An XThread can be released on another thread after its own ended, and its
// destructor destroys that thread's fiber there (upstream rexglue 6937495b).
TEST_CASE("rex::thread::Fiber - destroying another thread's fiber keeps the caller's", "[fiber]") {
  Fiber* other = nullptr;
  std::latch converted(1);
  std::latch destroyed(1);
  std::thread owner([&] {
    other = Fiber::ConvertCurrentThread();
    converted.count_down();
    destroyed.wait();
  });
  converted.wait();

  Fiber* mine = Fiber::ConvertCurrentThread();
  other->Destroy();
  Fiber* current = Fiber::Current();
  destroyed.count_down();
  owner.join();

  REQUIRE(current == mine);

  // The caller is still a fiber: it can switch to a created one and back.
  s_count = 0;
  s_main = mine;
  auto* f = Fiber::Create(256 * 1024, counting_fiber, nullptr);
  Fiber::SwitchTo(f);
  REQUIRE(s_count == 1);
  Fiber::SwitchTo(f);
  REQUIRE(s_count == 2);
  f->Destroy();
  mine->Destroy();
  s_main = nullptr;
}
