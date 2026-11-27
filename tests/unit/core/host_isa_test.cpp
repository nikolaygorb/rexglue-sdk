/**
 * @file        core/host_isa_test.cpp
 * @brief       The host instruction set checks behind REXGLUE_RECOMP_ISA
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/host_isa.h>

TEST_CASE("x86-64-v3 implies FMA, and a baseline build needs nothing", "[core][isa]") {
  if (rex::host_isa::HasX86_64_V3()) {
    CHECK(rex::host_isa::HasFma());
  }
  // The unit tests are built without REXGLUE_RECOMP_ISA.
  CHECK(std::string(rex::host_isa::RequiredName()).empty());
  CHECK(rex::host_isa::HostRunsRequired());
}
