/**
 * @file        graphics/native_transfer_filter_test.cpp
 * @brief       native_drop_transfers rules: parsing and which transfers they
 *              leave out
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/native/transfer_filter.h>

using rex::graphics::native::ParseTransferFilter;

TEST_CASE("native_drop_transfers: empty and malformed values drop nothing",
          "[graphics][native][edram]") {
  for (const char* text : {"", "  ", " , ,"}) {
    INFO(text);
    const auto filter = ParseTransferFilter(text);
    REQUIRE(filter);
    CHECK(filter->empty());
  }
  for (const char* text : {"D", "D>", ">D", "X>D", "D>>C", "D>C>D", "D64>C", "D@3>C", "D@>C",
                           "D1x>C", "D>D@8", "C6>D,junk"}) {
    INFO(text);
    CHECK_FALSE(ParseTransferFilter(text));
  }
}

TEST_CASE("native_drop_transfers: kinds, formats and sample counts", "[graphics][native][edram]") {
  // Arguments: source depth, format, samples, then the destination's.
  const auto depth_to_4x = ParseTransferFilter("D>D@4");
  REQUIRE(depth_to_4x);
  REQUIRE(depth_to_4x->rules.size() == 1);
  CHECK(depth_to_4x->Drops(true, 0, 1, true, 0, 4));
  CHECK(depth_to_4x->Drops(true, 1, 2, true, 1, 4));
  CHECK_FALSE(depth_to_4x->Drops(true, 0, 4, true, 0, 1));  // the other direction
  CHECK_FALSE(depth_to_4x->Drops(false, 6, 1, true, 0, 4));  // a colour source

  // The two RDR depth round trips one way each: D24S8 (0) 1x -> 4x and
  // D24FS8 (1) 2x -> 4x.
  const auto two = ParseTransferFilter("d0@1>D0@4,  D1@2>D1@4");
  REQUIRE(two);
  CHECK(two->rules.size() == 2);
  CHECK(two->Drops(true, 0, 1, true, 0, 4));
  CHECK(two->Drops(true, 1, 2, true, 1, 4));
  CHECK_FALSE(two->Drops(true, 0, 4, true, 0, 1));
  CHECK_FALSE(two->Drops(true, 1, 1, true, 1, 4));

  // k_16_16 colour (6) into depth, and anything into colour.
  const auto mixed = ParseTransferFilter("C6>D *>c");
  REQUIRE(mixed);
  CHECK(mixed->Drops(false, 6, 1, true, 0, 1));
  CHECK_FALSE(mixed->Drops(false, 0, 1, true, 0, 1));
  CHECK(mixed->Drops(true, 0, 1, false, 0, 1));
  CHECK(mixed->Drops(false, 6, 4, false, 2, 1));
  CHECK_FALSE(mixed->Drops(true, 0, 1, true, 0, 1));
}
