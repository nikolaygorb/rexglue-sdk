/**
 * @file        system/xsocket_accept_test.cpp
 * @brief       Tests for XSocket::Accept address handling
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

#include <rex/net/socket.h>
#include <rex/platform.h>
#include <rex/system/xsocket.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

using rex::X_STATUS;
using rex::system::N_XSOCKADDR;
using rex::system::object_ref;
using rex::system::XSocket;

namespace {

// A loopback TCP client connected to the listener's port.
rex::net::SocketHandle ConnectClient(uint16_t port_be) {
  const auto handle = static_cast<rex::net::SocketHandle>(socket(AF_INET, SOCK_STREAM, 0));
  REQUIRE(handle != rex::net::kInvalidSocket);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = port_be;
  REQUIRE(connect(handle, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  return handle;
}

}  // namespace

TEST_CASE("XSocket::Accept takes an optional address and keeps to its length", "[net]") {
#if REX_PLATFORM_WIN32
  WSADATA wsa;
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
  {
    // No kernel state: the objects are not put into an object table.
    auto listener = object_ref<XSocket>(new XSocket(nullptr));
    REQUIRE(listener->Initialize(XSocket::X_AF_INET, XSocket::X_SOCK_STREAM,
                                 XSocket::X_IPPROTO_TCP) == X_STATUS_SUCCESS);
    const auto listen_handle = static_cast<rex::net::SocketHandle>(listener->native_handle());
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(listen_handle, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t addr_len = sizeof(addr);
    REQUIRE(getsockname(listen_handle, reinterpret_cast<sockaddr*>(&addr), &addr_len) == 0);
    REQUIRE(listener->Listen(4) == X_STATUS_SUCCESS);

    const auto client_a = ConnectClient(addr.sin_port);
    const auto client_b = ConnectClient(addr.sin_port);
    const auto client_c = ConnectClient(addr.sin_port);

    // accept(s, NULL, NULL) is valid.
    int name_len = 0;
    auto accepted_a = listener->Accept(nullptr, &name_len);
    CHECK(accepted_a);

    // A full 16-byte address.
    N_XSOCKADDR name{};
    name_len = static_cast<int>(sizeof(name));
    auto accepted_b = listener->Accept(&name, &name_len);
    CHECK(accepted_b);
    CHECK(name_len == static_cast<int>(sizeof(name)));
    CHECK(name.address_family == AF_INET);

    // A shorter buffer is not written past its length.
    uint8_t buffer[32];
    std::memset(buffer, 0xCD, sizeof(buffer));
    name_len = 8;
    auto accepted_c = listener->Accept(reinterpret_cast<N_XSOCKADDR*>(buffer), &name_len);
    CHECK(accepted_c);
    CHECK(name_len == 8);
    bool tail_untouched = true;
    for (size_t i = 8; i < sizeof(buffer); ++i) {
      tail_untouched &= buffer[i] == 0xCD;
    }
    CHECK(tail_untouched);

    rex::net::socket_close(client_a);
    rex::net::socket_close(client_b);
    rex::net::socket_close(client_c);
  }
#if REX_PLATFORM_WIN32
  WSACleanup();
#endif
}
