#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <sys/ioctl.h>
#include <unistd.h>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  // Guest commands are Winsock's values; translate the ones titles use, as
  // xenia-canary does. Windows takes them unchanged (socket_win.cpp).
  unsigned long native_cmd = cmd;
  if (cmd == 0x8004667E) {
    native_cmd = FIONBIO;
  } else if (cmd == 0x4004667F) {
    native_cmd = FIONREAD;
  }
  return ioctl(static_cast<int>(handle), native_cmd, arg);
}

}  // namespace rex::net
