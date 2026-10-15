/**
 * @file        graphics/register_file_test.cpp
 * @brief       Tests for the Xenos GPU register table
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include <rex/graphics/register_file.h>

using rex::graphics::RegisterFile;
using rex::graphics::RegisterInfo;

// xenia-canary 0b041cd2a.
TEST_CASE("RB_HSIO_INTERFACE_ALIGNER_VALUE is a known register", "[graphics][registers]") {
  const RegisterInfo* info = RegisterFile::GetRegisterInfo(0x0F2D);
  REQUIRE(info != nullptr);
  CHECK(info->type == RegisterInfo::Type::kDword);
  CHECK(std::string_view(info->name) == "RB_HSIO_INTERFACE_ALIGNER_VALUE");
}

TEST_CASE("Unknown register index has no info", "[graphics][registers]") {
  CHECK(RegisterFile::GetRegisterInfo(RegisterFile::kRegisterCount) == nullptr);
}
