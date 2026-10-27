/**
 * @file        graphics/memory_placement_test.cpp
 * @brief       Tests for Vulkan memory placement on integrated GPUs, submission
 *              splitting and the integrated render target trim rule
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *              Cases ported from ForzaHorizon1-ReImagined tools
 *              test_vulkan_memory_placement.cpp, test_submission_watch.cpp and
 *              test_render_target_trim.cpp (MIT, Copyright (c) 2026 Dyotson and
 *              Forza Horizon 1: ReImagined contributors).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <initializer_list>

#include <catch2/catch_test_macros.hpp>

#include <graphics/vulkan/memory_placement.h>
#include <graphics/vulkan/render_target_trim.h>
#include <graphics/vulkan/submission_watch.h>

namespace p = rex::graphics::vulkan::memory_placement;
namespace w = rex::graphics::vulkan::submission_watch;
namespace trim = rex::graphics::vulkan::render_target_trim;

namespace {

constexpr uint64_t kMB = uint64_t(1) << 20;
constexpr uint64_t kGB = uint64_t(1) << 30;
constexpr uint32_t DL = p::kPropertyDeviceLocal, HV = p::kPropertyHostVisible,
                   HC = p::kPropertyHostCoherent, CACHED = p::kPropertyHostCached;
constexpr uint32_t kAmdCoherent = 0x40, kAmdUncached = 0x80, kLazy = 0x10;

struct Builder {
  p::MemoryInfo m;
  explicit Builder(uint32_t device_type) { m.device_type = device_type; }
  Builder& Heap(uint64_t size, bool device_local) {
    m.heap_size[m.heap_count] = size;
    m.heap_flags[m.heap_count] = device_local ? p::kHeapDeviceLocal : 0;
    ++m.heap_count;
    return *this;
  }
  Builder& Type(uint32_t heap, uint32_t flags) {
    m.type_heap[m.type_count] = heap;
    m.type_flags[m.type_count] = flags;
    ++m.type_count;
    return *this;
  }
};

// A Vega APU on AMD's Windows driver: the 512 MB carve-out split into a 256 MB
// device-local heap and a 256 MB host-visible device-local heap.
p::MemoryInfo VegaApuWindows() {
  return Builder(p::kDeviceTypeIntegrated)
      .Heap(256 * kMB, true)
      .Heap(7 * kGB, false)
      .Heap(256 * kMB, true)
      .Type(0, DL)
      .Type(1, HV | HC)
      .Type(2, DL | HV | HC)
      .Type(1, HV | HC | CACHED)
      .Type(0, DL | kAmdCoherent | kAmdUncached)
      .Type(1, HV | HC | kAmdCoherent | kAmdUncached)
      .m;
}

// A Steam Deck on RADV: two thirds of the shared memory reported device-local.
p::MemoryInfo RadvApu() {
  return Builder(p::kDeviceTypeIntegrated)
      .Heap(8 * kGB + 512 * kMB, true)
      .Heap(4 * kGB + 256 * kMB, false)
      .Type(0, DL)
      .Type(1, HV | HC)
      .Type(0, DL | HV | HC)
      .Type(1, HV | HC | CACHED)
      .m;
}

// A discrete GPU with resizable BAR (the system heap first).
p::MemoryInfo RadvDiscrete() {
  return Builder(p::kDeviceTypeDiscrete)
      .Heap(15 * kGB, false)
      .Heap(15 * kGB, true)
      .Type(1, DL)
      .Type(1, DL)
      .Type(0, HV | HC)
      .Type(1, DL | HV | HC)
      .Type(1, DL | HV | HC)
      .Type(0, HV | HC | CACHED)
      .Type(0, HV | HC | CACHED)
      .m;
}

p::HeapUse Use3(uint64_t u0, uint64_t b0, uint64_t u1, uint64_t b1, uint64_t u2, uint64_t b2) {
  p::HeapUse u;
  u.usage[0] = u0, u.budget[0] = b0;
  u.usage[1] = u1, u.budget[1] = b1;
  u.usage[2] = u2, u.budget[2] = b2;
  return u;
}

}  // namespace

TEST_CASE("Memory layouts of integrated and discrete GPUs", "[graphics][memory_placement]") {
  const p::Layout vega = p::Describe(VegaApuWindows());
  CHECK(vega.integrated);
  CHECK(vega.device_local_types == 0b101);  // not the AMD device-coherent type 4
  CHECK(vega.device_local_heaps == 0b101);
  CHECK(vega.device_local_bytes == 512 * kMB);
  CHECK(vega.system_heap == 1);
  CHECK(vega.system_types == 0b10);
  CHECK(vega.system_types_cached == 0b1000);
  CHECK(vega.tier_count == 2);
  CHECK((vega.tier_types[0] == 0b1 && vega.tier_heap[0] == 0));
  CHECK((vega.tier_types[1] == 0b100 && vega.tier_heap[1] == 2));

  const p::Layout discrete = p::Describe(RadvDiscrete());
  CHECK_FALSE(discrete.integrated);
  CHECK(discrete.can_move());
  CHECK(discrete.system_heap == 0);
  CHECK(discrete.device_local_types == 0b11011);

  // RADV's APUs: the device-local heap is not small next to the system heap.
  CHECK_FALSE(p::Describe(RadvApu()).integrated);
  // A single device-local heap (Intel): nothing to move to.
  const p::Layout intel = p::Describe(Builder(p::kDeviceTypeIntegrated)
                                          .Heap(16 * kGB, true)
                                          .Type(0, DL)
                                          .Type(0, DL | HV | HC)
                                          .m);
  CHECK_FALSE(intel.integrated);
  CHECK_FALSE(intel.can_move());
  // A carve-out is small up to half the system heap; a CPU device never counts.
  Builder small(p::kDeviceTypeIntegrated);
  small.Heap(1 * kGB, true).Heap(8 * kGB, false).Type(0, DL).Type(1, HV | HC);
  CHECK(p::Describe(small.m).integrated);
  Builder large(p::kDeviceTypeIntegrated);
  large.Heap(2 * kGB, true).Heap(3 * kGB + 512 * kMB, false).Type(0, DL).Type(1, HV | HC);
  CHECK_FALSE(p::Describe(large.m).integrated);
  Builder cpu(p::kDeviceTypeCpu);
  cpu.Heap(1 * kGB, true).Heap(8 * kGB, false).Type(0, DL).Type(1, HV | HC);
  CHECK_FALSE(p::Describe(cpu.m).integrated);
  // A system heap whose only type is lazily allocated is no place to move to.
  CHECK_FALSE(p::Describe(Builder(p::kDeviceTypeIntegrated)
                              .Heap(512 * kMB, true)
                              .Heap(8 * kGB, false)
                              .Type(0, DL)
                              .Type(1, kLazy)
                              .m)
                  .can_move());
}

TEST_CASE("Memory budget test cap", "[graphics][memory_placement]") {
  CHECK(p::CappedBudget(15 * kGB, 0) == 15 * kGB);
  CHECK(p::CappedBudget(15 * kGB, 486) == 486 * kMB);
  CHECK(p::CappedBudget(300 * kMB, 486) == 300 * kMB);
  CHECK(p::CappedBudget(0, 486) == 486 * kMB);
  const p::Layout l = p::Describe(VegaApuWindows());
  p::HeapUse u = Use3(0, 243 * kMB, 0, 6 * kGB, 0, 243 * kMB);
  p::ApplyCap(l, u, 300);
  CHECK(u.budget[0] == 150 * kMB);
  CHECK(u.budget[2] == 150 * kMB);
  CHECK(u.budget[1] == 6 * kGB);
  u = Use3(0, 0, 0, 0, 0, 0);
  p::ApplyCap(l, u, 486);
  CHECK(u.budget[0] == 243 * kMB);
  CHECK(u.budget[2] == 243 * kMB);
  CHECK(p::AllocatorBlockBytes(256 * kMB) == 32 * kMB);
  CHECK(p::AllocatorBlockBytes(1 * kGB) == 128 * kMB);
  CHECK(p::AllocatorBlockBytes(15 * kGB) == 256 * kMB);
}

TEST_CASE("Memory type order per heap budget", "[graphics][memory_placement]") {
  const uint64_t budget = 486 * kMB;
  CHECK(p::Fits(80, 300 * kMB, 64 * kMB, budget));
  CHECK_FALSE(p::Fits(80, 330 * kMB, 64 * kMB, budget));
  CHECK(p::Fits(80, 100 * kGB, 1 * kGB, 0));  // no budget known
  CHECK_FALSE(p::Fits(p::kTexturePercent, 0, 1, budget));
  CHECK(p::Fits(p::kHotPercent, 200 * kMB, 43 * kMB, budget));
  CHECK_FALSE(p::Fits(p::kHotPercent, 200 * kMB, 44 * kMB, budget));

  const p::Layout l = p::Describe(VegaApuWindows());
  const p::HeapUse u = Use3(240 * kMB, 243 * kMB, 900 * kMB, 6 * kGB, 10 * kMB, 243 * kMB);
  CHECK(p::FittingHeaps(l, 95, u, 16 * kMB, false) == 0b100);
  CHECK(p::FittingHeaps(l, 80, u, 1 * kMB, true) == 0b100);
  CHECK(p::FittingHeaps(l, 0, u, 1 * kMB, true) == 0);
  CHECK_FALSE(p::HeapOverBudget(l, u));
  CHECK(p::HeapOverBudget(l, Use3(243 * kMB, 243 * kMB, 0, 6 * kGB, 0, 243 * kMB)));

  // The host-visible device-local heap has room: it, system memory, then heap 0.
  p::Order o = p::Choose(l, 0xFF, 0b100);
  CHECK_FALSE(o.system_first);
  REQUIRE(o.count == 4);
  CHECK(o.masks[0] == 0b100);
  CHECK(o.masks[1] == 0b10);
  CHECK(o.masks[2] == 0b1000);
  CHECK(o.masks[3] == 0b1);
  // Both full: system memory first.
  o = p::Choose(l, 0xFF, 0);
  CHECK(o.system_first);
  CHECK(o.masks[0] == 0b10);
  // An image that takes only device-local types stays there.
  o = p::Choose(l, 0b101, 0);
  CHECK_FALSE(o.system_first);
  CHECK(o.count == 2);
  // The AMD-specific types are never chosen.
  CHECK(p::Choose(l, 0b110000, 0).count == 0);
}

TEST_CASE("A session on a Vega APU stays within the carve-out", "[graphics][memory_placement]") {
  const p::Layout l = p::Describe(VegaApuWindows());
  p::HeapUse u = Use3(60 * kMB, 243 * kMB, 0, 6 * kGB, 4 * kMB, 243 * kMB);
  uint32_t moved_textures = 0, moved_hot = 0;
  bool used_heap2 = false;
  for (int i = 0; i < 600; ++i) {
    const bool texture = i % 3 != 0;
    const uint64_t size = texture ? 6 * kMB : (i % 2 ? 16 * kMB : 4 * kMB);
    const uint32_t percent = texture ? p::kTexturePercent : p::kHotPercent;
    const p::Order o = p::Choose(l, 0xFF, p::FittingHeaps(l, percent, u, size, texture));
    REQUIRE(o.count > 0);
    const uint32_t type = p::LowestType(o.masks[0]);
    const uint32_t heap = type == 0 ? 0 : type == 2 ? 2 : 1;
    if (heap != 1) {
      REQUIRE_FALSE(texture);  // textures stay out of device-local memory
      u.usage[heap] += size;
      used_heap2 |= heap == 2;
    } else {
      u.usage[1] += size;
      ++(texture ? moved_textures : moved_hot);
    }
    for (uint32_t h : {0u, 2u}) {
      REQUIRE(u.usage[h] * 100 <= u.budget[h] * p::kHotPercent + 16 * kMB * 100);
    }
  }
  CHECK(moved_textures == 400);
  CHECK(moved_hot > 0);
  CHECK(used_heap2);
  CHECK_FALSE(p::HeapOverBudget(l, u));
}

TEST_CASE("Submissions are split by draw count on integrated GPUs",
          "[graphics][submission_watch]") {
  CHECK(w::DrawLimit(-1, true) == 1000);
  CHECK(w::DrawLimit(-1, false) == 0);
  CHECK(w::DrawLimit(0, true) == 0);
  CHECK(w::DrawLimit(250, false) == 250);
  CHECK_FALSE(w::ShouldSplit(999, 1000));
  CHECK(w::ShouldSplit(1000, 1000));
  CHECK_FALSE(w::ShouldSplit(100000, 0));
  // A 5500-draw frame reaches the GPU in six parts.
  uint32_t in_submission = 0, submissions = 1;
  for (int draw = 0; draw < 5500; ++draw) {
    if (w::ShouldSplit(in_submission, w::DrawLimit(-1, true))) {
      ++submissions;
      in_submission = 0;
    }
    ++in_submission;
  }
  CHECK(submissions == 6);
  CHECK(in_submission == 500);
}

TEST_CASE("Submission records and slow wait logging", "[graphics][submission_watch]") {
  w::Watch records;
  CHECK(records.Find(1) == nullptr);
  CHECK(records.Find(0) == nullptr);
  for (uint64_t s = 1; s <= 100; ++s) {
    records.Submitted(s, uint32_t(s * 10), int64_t(s));
  }
  REQUIRE(records.Find(100) != nullptr);
  CHECK(records.Find(100)->draws == 1000);
  CHECK(records.Find(37) != nullptr);
  CHECK(records.Find(36) == nullptr);  // overwritten by 100

  w::Watch watch;
  CHECK_FALSE(watch.ShouldLogWait(249, 0));
  CHECK(watch.ShouldLogWait(250, 1000));
  CHECK(watch.TakeSlowWaits() == 1);
  CHECK_FALSE(watch.ShouldLogWait(400, 5000));
  CHECK_FALSE(watch.ShouldLogWait(300, 10999));
  CHECK(watch.ShouldLogWait(260, 11000));
  CHECK(watch.TakeSlowWaits() == 3);
  int64_t t = 11000;
  int logged = 2;
  for (int i = 0; i < 100; ++i) {
    t += 10000;
    logged += watch.ShouldLogWait(300, t);
  }
  CHECK(logged == 50);
}

TEST_CASE("Render target trim on integrated GPUs", "[graphics][render_target_trim]") {
  using R = trim::Reason;
  auto m = [](uint64_t usage, uint64_t budget, bool heap_over, uint64_t system_usage,
              uint64_t system_budget, uint64_t render_targets) {
    trim::Memory x;
    x.usage = usage, x.budget = budget, x.heap_over_budget = heap_over;
    x.system_usage = system_usage, x.system_budget = system_budget;
    x.render_targets_in_system = render_targets;
    return x;
  };
  const uint64_t b = 486 * kMB, sb = 7000 * kMB;
  // A nearly full carve-out alone does not trim where memory moves.
  CHECK(trim::ShouldTrim(true, false, m(470 * kMB, b, false, 2000 * kMB, sb, 300 * kMB), 90) ==
        R::kNone);
  CHECK(trim::ShouldTrim(false, false, m(470 * kMB, b, false, 2000 * kMB, sb, 0), 90) ==
        R::kMemoryFull);
  CHECK(trim::ShouldTrim(false, false, m(400 * kMB, b, true, 6900 * kMB, sb, 2 * kGB), 90) ==
        R::kNone);
  CHECK(trim::ShouldTrim(true, false, m(250 * kMB, b, true, 2000 * kMB, sb, 0), 90) ==
        R::kMemoryFull);
  CHECK(trim::ShouldTrim(true, false, m(300 * kMB, b, false, 6400 * kMB, sb, 0), 90) ==
        R::kMemoryFull);
  CHECK(trim::ShouldTrim(true, true, m(100 * kMB, b, false, 100 * kMB, sb, 0), 90) ==
        R::kAllocationFailed);
  CHECK(trim::RenderTargetsInSystemLimit(sb) == 875 * kMB);
  CHECK(trim::ShouldTrim(true, false, m(470 * kMB, b, false, 2000 * kMB, sb, 874 * kMB), 90) ==
        R::kNone);
  CHECK(trim::ShouldTrim(true, false, m(470 * kMB, b, false, 2000 * kMB, sb, 875 * kMB), 90) ==
        R::kRenderTargetsInSystemMemory);
  CHECK(trim::RenderTargetsInSystemLimit(0) == 512 * kMB);
}
