/**
 * @file        kernel/xnaddr_test.cpp
 * @brief       XNetXnAddrToInAddr gives the online address, else the LAN one
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/net/xnaddr.h>

using rex::net::XnAddrInAddr;

TEST_CASE("XNetXnAddrToInAddr takes the online address, else the LAN address", "[kernel][net]") {
  // Raw words as the guest stores them (192.168.1.2 and 10.0.0.7).
  static_assert(XnAddrInAddr(0x0201A8C0u, 0x0700000Au) == 0x0700000Au);
  CHECK(XnAddrInAddr(0x0201A8C0u, 0) == 0x0201A8C0u);
  // Neither set: 0, which the call reports as a failure.
  CHECK(XnAddrInAddr(0, 0) == 0);
}
