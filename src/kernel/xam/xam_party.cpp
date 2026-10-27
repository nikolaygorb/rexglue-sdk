/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstddef>
#include <cstring>

#include <rex/kernel/xam/private.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {

// XPARTY_USER_LIST: a count padded to 8 bytes and 32 XPARTY_USER_INFO of
// 0x78 bytes.
constexpr size_t kPartyUserListSize = 0xF08;

u32 XamPartyGetUserList_entry(u32 player_count, mapped_u32 party_list) {
  // Titles read the count without checking the result.
  if (party_list) {
    std::memset(party_list.host_address(), 0, kPartyUserListSize);
  }
  // 5345085D wants specifically this code to skip loading party data.
  // This code is not documented in NT_STATUS code list
  return 0x807D0003;
}

// The internal list has the same count and 32 users of 0x80 bytes.
constexpr size_t kPartyUserListInternalSize = 0x1008;

u32 XamPartyGetUserListInternal_entry(mapped_void party_list) {
  // An empty list rather than whatever the buffer held (the Metro dashboard
  // crashed on it).
  if (party_list) {
    std::memset(party_list.host_address(), 0, kPartyUserListInternalSize);
  }
  return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
}

u32 XamPartySendGameInvites_entry(u32 r3, u32 r4, u32 r5) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 XamPartySetCustomData_entry(u32 r3, u32 r4, u32 r5) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 XamPartyGetBandwidth_entry(u32 r3, u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamPartyGetUserList, rex::kernel::xam::XamPartyGetUserList_entry)
REX_EXPORT(__imp__XamPartyGetUserListInternal,
           rex::kernel::xam::XamPartyGetUserListInternal_entry)
REX_EXPORT(__imp__XamPartySendGameInvites, rex::kernel::xam::XamPartySendGameInvites_entry)
REX_EXPORT(__imp__XamPartySetCustomData, rex::kernel::xam::XamPartySetCustomData_entry)
REX_EXPORT(__imp__XamPartyGetBandwidth, rex::kernel::xam::XamPartyGetBandwidth_entry)

REX_EXPORT_STUB(__imp__XamPartyAddLocalUsers);
REX_EXPORT_STUB(__imp__XamPartyAutomationInprocCall);
REX_EXPORT_STUB(__imp__XamPartyCreate);
REX_EXPORT_STUB(__imp__XamPartyGetAccessLevel);
REX_EXPORT_STUB(__imp__XamPartyGetFormation);
REX_EXPORT_STUB(__imp__XamPartyGetInfo);
REX_EXPORT_STUB(__imp__XamPartyGetInfoEx);
REX_EXPORT_STUB(__imp__XamPartyGetJoinable);
REX_EXPORT_STUB(__imp__XamPartyGetNetworkCounters);
REX_EXPORT_STUB(__imp__XamPartyGetRoutingTable);
REX_EXPORT_STUB(__imp__XamPartyGetState);
REX_EXPORT_STUB(__imp__XamPartyIsCoordinator);
REX_EXPORT_STUB(__imp__XamPartyJoin);
REX_EXPORT_STUB(__imp__XamPartyJoinEx);
REX_EXPORT_STUB(__imp__XamPartyKickUser);
REX_EXPORT_STUB(__imp__XamPartyLeave);
REX_EXPORT_STUB(__imp__XamPartyOverrideNatType);
REX_EXPORT_STUB(__imp__XamPartyRemoveLocalUsers);
REX_EXPORT_STUB(__imp__XamPartySendInvite);
REX_EXPORT_STUB(__imp__XamPartySendInviteDeprecated);
REX_EXPORT_STUB(__imp__XamPartySetConnectivityGraph);
REX_EXPORT_STUB(__imp__XamPartySetJoinable);
REX_EXPORT_STUB(__imp__XamPartySetTestDelay);
REX_EXPORT_STUB(__imp__XamPartySetTestFlags);
