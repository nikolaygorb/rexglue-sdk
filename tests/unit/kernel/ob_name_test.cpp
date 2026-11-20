/**
 * @file        tests/unit/kernel/ob_name_test.cpp
 * @brief       Tests for the \??\ and \System??\ name helpers of xboxkrnl
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <rex/kernel/xboxkrnl/ob.h>

using rex::kernel::xboxkrnl::xeObStripDosDevicesPrefix;
using rex::kernel::xboxkrnl::xeObSymbolicLinkName;

TEST_CASE("xeObStripDosDevicesPrefix drops a leading DOS devices directory", "[kernel][ob]") {
  std::string path = "\\??\\game:\\file.bin";
  xeObStripDosDevicesPrefix(path);
  REQUIRE(path == "game:\\file.bin");

  std::string plain = "game:\\file.bin";
  xeObStripDosDevicesPrefix(plain);
  REQUIRE(plain == "game:\\file.bin");

  // Only a leading prefix.
  std::string inner = "game:\\??\\file.bin";
  xeObStripDosDevicesPrefix(inner);
  REQUIRE(inner == "game:\\??\\file.bin");

  // \System??\ in any case (xenia-edge d4c0f95a2).
  for (const char* prefix : {"\\System??\\", "\\system??\\", "\\SYSTEM??\\"}) {
    std::string system = std::string(prefix) + "game:\\file.bin";
    xeObStripDosDevicesPrefix(system);
    REQUIRE(system == "game:\\file.bin");
  }
  std::string short_name = "\\System??";
  xeObStripDosDevicesPrefix(short_name);
  REQUIRE(short_name == "\\System??");
  std::string one_mark = "\\System?\\game:";
  xeObStripDosDevicesPrefix(one_mark);
  REQUIRE(one_mark == "\\System?\\game:");
  // Bytes after the prefix are kept as they are.
  std::string bytes = "\\System??\\\xFF\xFE";
  xeObStripDosDevicesPrefix(bytes);
  REQUIRE(bytes == "\xFF\xFE");
}

TEST_CASE("xeObSymbolicLinkName gives one name for every spelling", "[kernel][ob]") {
  REQUIRE(xeObSymbolicLinkName("\\??\\D:") == "D:");
  REQUIRE(xeObSymbolicLinkName("\\System??\\D:") == "D:");
  REQUIRE(xeObSymbolicLinkName("\\system??\\UPDATE:") == "UPDATE:");
  REQUIRE(xeObSymbolicLinkName("GAME:") == "GAME:");
  REQUIRE(xeObSymbolicLinkName("\\??\\D:\\") == xeObSymbolicLinkName("D:"));
}
