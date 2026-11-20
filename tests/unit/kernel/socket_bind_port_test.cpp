/**
 * @file        kernel/socket_bind_port_test.cpp
 * @brief       A bind to a privileged port takes the port 10000 above it
 *              (xenia-canary XSocket::Bind, upstream rexglue PR #589)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xsocket.h>

using rex::system::XSocket;

TEST_CASE("Privileged ports are bound 10000 up, others as given", "[kernel][net]") {
  static_assert(XSocket::HostPortForBind(987) == 10987);
  CHECK(XSocket::HostPortForBind(1) == 10001);
  CHECK(XSocket::HostPortForBind(1023) == 11023);
  // 0 asks for any free port, and 1024 up needs no privileges.
  CHECK(XSocket::HostPortForBind(0) == 0);
  CHECK(XSocket::HostPortForBind(1024) == 1024);
  CHECK(XSocket::HostPortForBind(3074) == 3074);
}
