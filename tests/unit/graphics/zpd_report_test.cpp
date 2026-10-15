/**
 * @file        graphics/zpd_report_test.cpp
 * @brief       Tests for the EVENT_WRITE_ZPD sample counter report helpers
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/xenos_zpd_report.h>

using namespace rex::graphics;

TEST_CASE("ZPD report normalizes host counts by the draw scale area", "[graphics][zpd]") {
  XenosZPDReport report;
  report.z_pass = 10;
  report.z_fail = 1;
  report.stencil_fail = 0;

  // Native scale keeps the counts.
  CHECK(report.Normalized(1) == report);

  // Rounds to nearest, a non-zero count never drops to zero, zero stays zero.
  XenosZPDReport normalized = report.Normalized(4);
  CHECK(normalized.z_pass == 3);
  CHECK(normalized.z_fail == 1);
  CHECK(normalized.stencil_fail == 0);

  report.z_pass = 9;
  CHECK(report.Normalized(4).z_pass == 2);
  report.z_pass = 18;
  CHECK(report.Normalized(4).z_pass == 5);
}

TEST_CASE("ZPD report derives ZFail from pre-test coverage", "[graphics][zpd]") {
  XenosZPDReport report = XenosZPDReport::FromNativeQueryAndTotal(6, 10);
  CHECK(report.z_pass == 6);
  CHECK(report.z_fail == 4);
  CHECK(report.stencil_fail == 0);
  CHECK(report.total() == 10);

  // Coverage below the passed count can't make ZFail negative.
  report = XenosZPDReport::FromNativeQueryAndTotal(8, 5);
  CHECK(report.z_pass == 8);
  CHECK(report.z_fail == 0);
}

TEST_CASE("ZPD report reads interlock counter slots", "[graphics][zpd]") {
  uint32_t slot[XenosZPDReport::kCount] = {};
  slot[XenosZPDReport::kTotal] = 100;
  slot[XenosZPDReport::kZFail] = 3;
  slot[XenosZPDReport::kZPass] = 5;
  slot[XenosZPDReport::kStencilFail] = 7;
  XenosZPDReport report = XenosZPDReport::FromCounterSlot(slot);
  CHECK(report.z_fail == 3);
  CHECK(report.z_pass == 5);
  CHECK(report.stencil_fail == 7);
  // Total is always the sum, the slot's Total lane is ignored.
  CHECK(report.total() == 15);

  XenosZPDReport sum = report;
  sum += XenosZPDReport::FromNativeQuery(10);
  CHECK(sum.z_pass == 15);
  CHECK(sum.z_fail == 3);
  CHECK(sum.stencil_fail == 7);
}

TEST_CASE("ZPD report splits counters across the A and B lanes", "[graphics][zpd]") {
  XenosZPDReport report;
  report.z_pass = 7;
  report.z_fail = 2;
  report.stencil_fail = 1;

  xenos::xe_gpu_depth_sample_counts guest;
  std::memset(&guest, 0xFF, sizeof(guest));
  report.WriteTo(&guest);

  // Lane A gets the odd sample, D3D sums both lanes.
  CHECK(uint32_t(guest.ZPass_A) == 4);
  CHECK(uint32_t(guest.ZPass_B) == 3);
  CHECK(uint32_t(guest.ZFail_A) == 1);
  CHECK(uint32_t(guest.ZFail_B) == 1);
  CHECK(uint32_t(guest.StencilFail_A) == 1);
  CHECK(uint32_t(guest.StencilFail_B) == 0);
  CHECK(uint32_t(guest.Total_A) == 5);
  CHECK(uint32_t(guest.Total_B) == 5);

  // The hardware counters wrap at 32 bits.
  report = XenosZPDReport::FromNativeQuery((uint64_t(1) << 32) + 3);
  report.WriteTo(&guest);
  CHECK(uint32_t(guest.ZPass_A) == 2);
  CHECK(uint32_t(guest.ZPass_B) == 1);
}
