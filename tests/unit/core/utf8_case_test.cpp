/**
 * @file        core/utf8_case_test.cpp
 * @brief       Tests for the ASCII case conversion helpers of rex::string
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/string/utf8.h>

TEST_CASE("utf8_lower_ascii lowers ASCII letters only", "[utf8]") {
  CHECK(rex::string::utf8_lower_ascii("Hello, World 123") == "hello, world 123");
  CHECK(rex::string::utf8_lower_ascii("\xC3\x86" "BLE \xC3\x85" "B") ==
        "\xC3\x86" "ble \xC3\x85" "b");
}

TEST_CASE("utf8_upper_ascii raises ASCII letters only", "[utf8]") {
  CHECK(rex::string::utf8_upper_ascii("Hello, World 123") == "HELLO, WORLD 123");
  CHECK(rex::string::utf8_upper_ascii("HELLO") == "HELLO");
  CHECK(rex::string::utf8_upper_ascii("\xC3\xA6" "ble \xC3\xA5" "b") ==
        "\xC3\xA6" "BLE \xC3\xA5" "B");
}
