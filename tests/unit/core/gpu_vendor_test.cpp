/**
 * @file        core/gpu_vendor_test.cpp
 * @brief       Tests for the gpu_vendor cvar parsing
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/ui/gpu_vendor.h>

using rex::ui::PreferredGpuVendorId;

TEST_CASE("gpu_vendor names and ids map to PCI vendor ids", "[gpu]") {
  CHECK(PreferredGpuVendorId("nvidia") == rex::ui::kGpuVendorNvidia);
  CHECK(PreferredGpuVendorId(" NVIDIA ") == rex::ui::kGpuVendorNvidia);
  CHECK(PreferredGpuVendorId("amd") == rex::ui::kGpuVendorAmd);
  CHECK(PreferredGpuVendorId("Radeon") == rex::ui::kGpuVendorAmd);
  CHECK(PreferredGpuVendorId("intel") == rex::ui::kGpuVendorIntel);
  CHECK(PreferredGpuVendorId("0x10de") == 0x10DEu);
  CHECK(PreferredGpuVendorId("1002") == 0x1002u);
}

TEST_CASE("gpu_vendor without a usable value means any card", "[gpu]") {
  CHECK(PreferredGpuVendorId("") == rex::ui::kGpuVendorNone);
  CHECK(PreferredGpuVendorId("auto") == rex::ui::kGpuVendorNone);
  CHECK(PreferredGpuVendorId("geforce rtx") == rex::ui::kGpuVendorNone);
  CHECK(PreferredGpuVendorId("0x10000") == rex::ui::kGpuVendorNone);
  CHECK(PreferredGpuVendorId("0") == rex::ui::kGpuVendorNone);
}
