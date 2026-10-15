/**
 * @file        core/socket_test.cpp
 * @brief       Tests for the platform socket helpers
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <rex/net/socket.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>
#else
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace {

bool WouldBlock() {
#if REX_PLATFORM_WIN32
  return WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

}  // namespace

TEST_CASE("Guest FIONBIO makes a socket non-blocking", "[net]") {
#if REX_PLATFORM_WIN32
  WSADATA wsa;
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
  const auto handle = static_cast<rex::net::SocketHandle>(socket(AF_INET, SOCK_DGRAM, 0));
  REQUIRE(handle != rex::net::kInvalidSocket);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(bind(handle, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  // Winsock's FIONBIO, as the guest passes it to ioctlsocket. Any byte order
  // of 1 is non-zero, so the argument needs no swap. REQUIRE: a blocking
  // socket would hang the recv below.
  uint32_t enable = 1;
  REQUIRE(rex::net::socket_ioctl(handle, 0x8004667E, reinterpret_cast<uint8_t*>(&enable)) == 0);

  // Nothing was sent: a non-blocking read returns at once.
  char byte;
  CHECK(recv(handle, &byte, 1, 0) < 0);
  CHECK(WouldBlock());

  rex::net::socket_close(handle);
#if REX_PLATFORM_WIN32
  WSACleanup();
#endif
}
