#pragma once

#include <rex/types.h>

namespace rex::kernel::xboxkrnl {

struct XECRYPT_SHA256_STATE {
  rex::be<uint32_t> count;
  rex::be<uint32_t> state[8];
  uint8_t buffer[64];
};
static_assert_size(XECRYPT_SHA256_STATE, 0x64);

struct XECRYPT_SHA512_STATE {
  rex::be<uint32_t> count;
  uint32_t pad;
  rex::be<uint64_t> state[8];
  uint8_t buffer[128];
};
static_assert_size(XECRYPT_SHA512_STATE, 0xC8);

void XeCryptSha256Init_entry(ppc_ptr_t<XECRYPT_SHA256_STATE> state);
void XeCryptSha256Update_entry(ppc_ptr_t<XECRYPT_SHA256_STATE> state, mapped_void input, u32 size);
void XeCryptSha256Final_entry(ppc_ptr_t<XECRYPT_SHA256_STATE> state, ppc_ptr_t<uint8_t> out,
                              u32 size);
void XeCryptSha512Init_entry(ppc_ptr_t<XECRYPT_SHA512_STATE> state);
void XeCryptSha512Update_entry(ppc_ptr_t<XECRYPT_SHA512_STATE> state, mapped_void input, u32 size);
void XeCryptSha512Final_entry(ppc_ptr_t<XECRYPT_SHA512_STATE> state, ppc_ptr_t<uint8_t> out,
                              u32 size);

}  // namespace rex::kernel::xboxkrnl
