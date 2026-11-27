#pragma once
/**
 * @file        rex/net/xnaddr.h
 * @brief       The IN_ADDR an XNADDR stands for (XNetXnAddrToInAddr)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>

namespace rex::net {

// The address XNetXnAddrToInAddr gives for an XNADDR: its online address
// (inaOnline), else its LAN address (ina); 0 when neither is set, which the
// call reports as a failure. The values are the raw IN_ADDR words, in network
// byte order as the guest stores them (upstream rexglue ab7ed503, PR #570).
constexpr uint32_t XnAddrInAddr(uint32_t ina, uint32_t ina_online) {
  return ina_online ? ina_online : ina;
}

}  // namespace rex::net
