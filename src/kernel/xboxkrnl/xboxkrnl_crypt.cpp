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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <utility>

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/kernel/xboxkrnl/xecrypt_md5.h>
#include <rex/kernel/xboxkrnl/xecrypt_rsa.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

#include "crypto/TinySHA1.hpp"
#include "crypto/des/des.cpp"
#include "crypto/des/des.h"
#include "crypto/des/des3.h"
#include "crypto/des/descbc.h"
#include "crypto/sha256.cpp"
#include "crypto/sha256.h"

extern "C" {
#include "aes_128/aes.h"
#include "libavutil/mem.h"
#include "libavutil/sha512.h"
}

namespace rex::kernel::xboxkrnl {

typedef struct {
  uint8_t S[256];  // 0x0
  uint8_t i;       // 0x100
  uint8_t j;       // 0x101
} XECRYPT_RC4_STATE;
static_assert_size(XECRYPT_RC4_STATE, 0x102);

void XeCryptRc4Key_entry(ppc_ptr_t<XECRYPT_RC4_STATE> rc4_ctx, mapped_void key, u32 key_size) {
  // Setup RC4 state
  rc4_ctx->i = rc4_ctx->j = 0;
  for (uint32_t x = 0; x < 0x100; x++) {
    rc4_ctx->S[x] = (uint8_t)x;
  }

  // A key of no bytes has nothing to schedule and would divide by zero.
  if (!key_size) {
    return;
  }
  uint32_t idx = 0;
  for (uint32_t x = 0; x < 0x100; x++) {
    idx = (idx + rc4_ctx->S[x] + key[x % key_size]) % 0x100;
    uint8_t temp = rc4_ctx->S[idx];
    rc4_ctx->S[idx] = rc4_ctx->S[x];
    rc4_ctx->S[x] = temp;
  }
}

void XeCryptRc4Ecb_entry(ppc_ptr_t<XECRYPT_RC4_STATE> rc4_ctx, mapped_void data, u32 size) {
  // Crypt data
  for (uint32_t idx = 0; idx < size; idx++) {
    rc4_ctx->i = (rc4_ctx->i + 1) % 0x100;
    rc4_ctx->j = (rc4_ctx->j + rc4_ctx->S[rc4_ctx->i]) % 0x100;
    uint8_t temp = rc4_ctx->S[rc4_ctx->i];
    rc4_ctx->S[rc4_ctx->i] = rc4_ctx->S[rc4_ctx->j];
    rc4_ctx->S[rc4_ctx->j] = temp;

    uint8_t a = data[idx];
    uint8_t b = rc4_ctx->S[(rc4_ctx->S[rc4_ctx->i] + rc4_ctx->S[rc4_ctx->j]) % 0x100];
    data[idx] = (uint8_t)(a ^ b);
  }
}

void XeCryptRc4_entry(mapped_void key, u32 key_size, mapped_void data, u32 size) {
  XECRYPT_RC4_STATE rc4_ctx;
  XeCryptRc4Key_entry(ppc_ptr_t<XECRYPT_RC4_STATE>::from_host(&rc4_ctx), key, key_size);
  XeCryptRc4Ecb_entry(ppc_ptr_t<XECRYPT_RC4_STATE>::from_host(&rc4_ctx), data, size);
}

typedef struct {
  rex::be<uint32_t> count;     // 0x0
  rex::be<uint32_t> state[5];  // 0x4
  uint8_t buffer[64];          // 0x18
} XECRYPT_SHA_STATE;
static_assert_size(XECRYPT_SHA_STATE, 0x58);

void InitSha1(sha1::SHA1* sha, const XECRYPT_SHA_STATE* state) {
  uint32_t digest[5];
  std::copy(std::begin(state->state), std::end(state->state), digest);

  sha->init(digest, state->buffer, state->count);
}

void StoreSha1(const sha1::SHA1* sha, XECRYPT_SHA_STATE* state) {
  std::copy_n(sha->getDigest(), rex::countof(state->state), state->state);

  state->count = static_cast<uint32_t>(sha->getByteCount());
  std::copy_n(sha->getBlock(), sha->getBlockByteIndex(), state->buffer);
}

void XeCryptShaInit_entry(ppc_ptr_t<XECRYPT_SHA_STATE> sha_state) {
  sha_state.Zero();

  sha_state->state[0] = 0x67452301;
  sha_state->state[1] = 0xEFCDAB89;
  sha_state->state[2] = 0x98BADCFE;
  sha_state->state[3] = 0x10325476;
  sha_state->state[4] = 0xC3D2E1F0;
}

void XeCryptShaUpdate_entry(ppc_ptr_t<XECRYPT_SHA_STATE> sha_state, mapped_void input,
                            u32 input_size) {
  sha1::SHA1 sha;
  InitSha1(&sha, sha_state);

  sha.processBytes(input, input_size);

  StoreSha1(&sha, sha_state);
}

void XeCryptShaFinal_entry(ppc_ptr_t<XECRYPT_SHA_STATE> sha_state, ppc_ptr_t<uint8_t> out,
                           u32 out_size) {
  sha1::SHA1 sha;
  InitSha1(&sha, sha_state);

  uint8_t digest[0x14];
  sha.finalize(digest);

  std::copy_n(digest, std::min<size_t>(rex::countof(digest), out_size), static_cast<uint8_t*>(out));
  std::copy_n(sha.getDigest(), rex::countof(sha_state->state), sha_state->state);
  // The console pads in place, so the last block it compressed is left in the
  // context buffer, not the message tail. Only shows up when a title finalizes
  // the same context twice and the padding needed a second block, where the
  // tail no longer survives the first call.
  std::copy_n(sha.getBlock(), rex::countof(sha_state->buffer), sha_state->buffer);
}

void XeCryptSha_entry(mapped_void input_1, u32 input_1_size, mapped_void input_2, u32 input_2_size,
                      mapped_void input_3, u32 input_3_size, mapped_void output, u32 output_size) {
  sha1::SHA1 sha;

  if (input_1 && input_1_size) {
    sha.processBytes(input_1, input_1_size);
  }
  if (input_2 && input_2_size) {
    sha.processBytes(input_2, input_2_size);
  }
  if (input_3 && input_3_size) {
    sha.processBytes(input_3, input_3_size);
  }

  uint8_t digest[0x14];
  sha.finalize(digest);
  std::copy_n(digest, std::min<size_t>(rex::countof(digest), output_size), output.as<uint8_t*>());
}

// TODO: Size of this struct hasn't been confirmed yet.
typedef struct {
  rex::be<uint32_t> count;     // 0x0
  rex::be<uint32_t> state[8];  // 0x4
  uint8_t buffer[64];          // 0x24
} XECRYPT_SHA256_STATE;

void XeCryptSha256Init_entry(ppc_ptr_t<XECRYPT_SHA256_STATE> sha_state) {
  sha_state.Zero();

  sha_state->state[0] = 0x6a09e667;
  sha_state->state[1] = 0xbb67ae85;
  sha_state->state[2] = 0x3c6ef372;
  sha_state->state[3] = 0xa54ff53a;
  sha_state->state[4] = 0x510e527f;
  sha_state->state[5] = 0x9b05688c;
  sha_state->state[6] = 0x1f83d9ab;
  sha_state->state[7] = 0x5be0cd19;
}

void XeCryptSha256Update_entry(ppc_ptr_t<XECRYPT_SHA256_STATE> sha_state, mapped_void input,
                               u32 input_size) {
  sha256::SHA256 sha;
  std::copy(std::begin(sha_state->state), std::end(sha_state->state), sha.getHashValues());
  std::copy(std::begin(sha_state->buffer), std::end(sha_state->buffer), sha.getBuffer());
  sha.setTotalSize(sha_state->count);

  sha.add(input, input_size);

  std::copy_n(sha.getHashValues(), rex::countof(sha_state->state), sha_state->state);
  std::copy_n(sha.getBuffer(), rex::countof(sha_state->buffer), sha_state->buffer);
  sha_state->count = static_cast<uint32_t>(sha.getTotalSize());
}

void XeCryptSha256Final_entry(ppc_ptr_t<XECRYPT_SHA256_STATE> sha_state, ppc_ptr_t<uint8_t> out,
                              u32 out_size) {
  sha256::SHA256 sha;
  std::copy(std::begin(sha_state->state), std::end(sha_state->state), sha.getHashValues());
  std::copy(std::begin(sha_state->buffer), std::end(sha_state->buffer), sha.getBuffer());
  sha.setTotalSize(sha_state->count);

  uint8_t hash[32];
  sha.getHash(hash);

  std::copy_n(hash, std::min<size_t>(rex::countof(hash), out_size), static_cast<uint8_t*>(out));
  std::copy(std::begin(hash), std::end(hash), sha_state->buffer);
}

void XeCryptSha256_entry(mapped_void input_1, u32 input_1_size, mapped_void input_2,
                         u32 input_2_size, mapped_void input_3, u32 input_3_size,
                         mapped_void output, u32 output_size) {
  sha256::SHA256 sha;
  for (auto [input, size] : {std::pair{input_1, input_1_size}, std::pair{input_2, input_2_size},
                             std::pair{input_3, input_3_size}}) {
    if (input && size) {
      sha.add(input, size);
    }
  }

  uint8_t hash[32];
  sha.getHash(hash);
  std::copy_n(hash, std::min<size_t>(rex::countof(hash), output_size), output.as<uint8_t*>());
}

// Byteswaps each 8 bytes
void XeCryptBnQw_SwapDwQwLeBe_entry(ppc_ptr_t<uint64_t> qw_inp, ppc_ptr_t<uint64_t> qw_out,
                                    u32 size) {
  memory::copy_and_swap<uint64_t>(qw_out, qw_inp, size);
}

typedef struct {
  rex::be<uint32_t> size;  // size of modulus in 8 byte units
  rex::be<uint32_t> public_exponent;
  rex::be<uint64_t> pad_8;

  // followed by modulus, followed by any private-key data
} XECRYPT_RSA;
static_assert_size(XECRYPT_RSA, 0x10);

u32 XeCryptBnQwNeRsaPubCrypt_entry(ppc_ptr_t<uint64_t> qw_a, ppc_ptr_t<uint64_t> qw_b,
                                   ppc_ptr_t<XECRYPT_RSA> rsa) {
  // b = a^e mod n on every platform. The numbers stay in guest layout: big-endian
  // 64-bit limbs, least significant limb first; the modulus follows the header.
  const uint8_t* input_bytes = reinterpret_cast<const uint8_t*>(static_cast<uint64_t*>(qw_a));
  uint8_t* output_bytes = reinterpret_cast<uint8_t*>(static_cast<uint64_t*>(qw_b));
  const uint8_t* modulus_bytes = reinterpret_cast<const uint8_t*>(&rsa[1]);
  return ::XeCryptBnQwNeRsaPubCrypt(input_bytes, output_bytes, modulus_bytes, rsa->size,
                                    rsa->public_exponent);
}
#if REX_PLATFORM_WIN32

#else

#endif

u32 XeCryptBnDwLePkcs1Verify_entry(mapped_void hash, mapped_void sig, u32 size) {
  // BOOL return value
  return 1;
}

void XeCryptRandom_entry(mapped_void buf, u32 buf_size) {
  uint8_t* buf_data = buf.as<uint8_t*>();

  thread_local std::mt19937_64 gen(std::random_device{}());
  std::uniform_int_distribution<uint16_t> dist(0, std::numeric_limits<uint8_t>::max());

  std::generate(buf_data, buf_data + buf_size, [&]() { return static_cast<uint8_t>(dist(gen)); });
}

struct XECRYPT_DES_STATE {
  uint32_t keytab[16][2];
};

// Sets bit 0 to make the parity of each byte odd, from the other seven bits.
void XeCryptDesParity_entry(mapped_void inp, u32 inp_size, mapped_void out_ptr) {
  const uint8_t* in = inp.as<const uint8_t*>();
  uint8_t* out = out_ptr.as<uint8_t*>();
  for (uint32_t i = 0; i < inp_size; i++) {
    const uint8_t high = in[i] & 0xFE;
    out[i] = high | ((std::popcount(high) & 1) ^ 1);
  }
}

struct XECRYPT_DES3_STATE {
  XECRYPT_DES_STATE des_state[3];
};

void XeCryptDes3Key_entry(ppc_ptr_t<XECRYPT_DES3_STATE> state_ptr, mapped_u64 key) {
  DES3 des3(key[0], key[1], key[2]);
  DES* des = des3.getDES();

  // Store our DES state into the state.
  for (int i = 0; i < 3; i++) {
    std::memcpy(state_ptr->des_state[i].keytab, des[i].get_sub_key(), 128);
  }
}

void XeCryptDes3Ecb_entry(ppc_ptr_t<XECRYPT_DES3_STATE> state_ptr, mapped_u64 inp, mapped_u64 out,
                          u32 encrypt) {
  DES3 des3((ui64*)state_ptr->des_state[0].keytab, (ui64*)state_ptr->des_state[1].keytab,
            (ui64*)state_ptr->des_state[2].keytab);

  if (encrypt) {
    *out = des3.encrypt(*inp);
  } else {
    *out = des3.decrypt(*inp);
  }
}

void XeCryptDes3Cbc_entry(ppc_ptr_t<XECRYPT_DES3_STATE> state_ptr, mapped_u64 inp, u32 inp_size,
                          mapped_u64 out, mapped_u64 feed, u32 encrypt) {
  DES3 des3((ui64*)state_ptr->des_state[0].keytab, (ui64*)state_ptr->des_state[1].keytab,
            (ui64*)state_ptr->des_state[2].keytab);

  // DES works on 8-byte blocks; a partial block at the end is left alone.
  uint64_t last_block = *feed;
  for (uint32_t i = 0; i < inp_size / 8; i++) {
    uint64_t block = inp[i];
    if (encrypt) {
      last_block = des3.encrypt(block ^ last_block);
      out[i] = last_block;
    } else {
      out[i] = des3.decrypt(block) ^ last_block;
      last_block = block;
    }
  }

  *feed = last_block;
}

struct XECRYPT_AES_STATE {
  uint8_t keytabenc[11][4][4];  // 0x0
  uint8_t keytabdec[11][4][4];  // 0xB0
};
static_assert_size(XECRYPT_AES_STATE, 0x160);

static inline uint8_t xeXeCryptAesMul2(uint8_t a) {
  return (a & 0x80) ? ((a << 1) ^ 0x1B) : (a << 1);
}

void XeCryptAesKey_entry(ppc_ptr_t<XECRYPT_AES_STATE> state_ptr, mapped_void key) {
  aes_key_schedule_128(key, reinterpret_cast<uint8_t*>(state_ptr->keytabenc));
  // Decryption key schedule not needed by openluopworld/aes_128, but generated
  // to fill the context structure properly: the round keys in encryption round
  // order, with InvMixColumns applied to rounds 1 to 9.
  std::memcpy(state_ptr->keytabdec[0], state_ptr->keytabenc[0], 16);
  for (uint32_t i = 1; i < 10; ++i) {
    const uint8_t* enc = reinterpret_cast<const uint8_t*>(state_ptr->keytabenc[i]);
    uint8_t* dec = reinterpret_cast<uint8_t*>(state_ptr->keytabdec[i]);
    uint8_t t, u, v;
    t = enc[0] ^ enc[1] ^ enc[2] ^ enc[3];
    dec[0] = t ^ enc[0] ^ xeXeCryptAesMul2(enc[0] ^ enc[1]);
    dec[1] = t ^ enc[1] ^ xeXeCryptAesMul2(enc[1] ^ enc[2]);
    dec[2] = t ^ enc[2] ^ xeXeCryptAesMul2(enc[2] ^ enc[3]);
    dec[3] = t ^ enc[3] ^ xeXeCryptAesMul2(enc[3] ^ enc[0]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[0] ^ enc[2]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[1] ^ enc[3]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[0] ^= t ^ u;
    dec[1] ^= t ^ v;
    dec[2] ^= t ^ u;
    dec[3] ^= t ^ v;
    t = enc[4] ^ enc[5] ^ enc[6] ^ enc[7];
    dec[4] = t ^ enc[4] ^ xeXeCryptAesMul2(enc[4] ^ enc[5]);
    dec[5] = t ^ enc[5] ^ xeXeCryptAesMul2(enc[5] ^ enc[6]);
    dec[6] = t ^ enc[6] ^ xeXeCryptAesMul2(enc[6] ^ enc[7]);
    dec[7] = t ^ enc[7] ^ xeXeCryptAesMul2(enc[7] ^ enc[4]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[4] ^ enc[6]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[5] ^ enc[7]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[4] ^= t ^ u;
    dec[5] ^= t ^ v;
    dec[6] ^= t ^ u;
    dec[7] ^= t ^ v;
    t = enc[8] ^ enc[9] ^ enc[10] ^ enc[11];
    dec[8] = t ^ enc[8] ^ xeXeCryptAesMul2(enc[8] ^ enc[9]);
    dec[9] = t ^ enc[9] ^ xeXeCryptAesMul2(enc[9] ^ enc[10]);
    dec[10] = t ^ enc[10] ^ xeXeCryptAesMul2(enc[10] ^ enc[11]);
    dec[11] = t ^ enc[11] ^ xeXeCryptAesMul2(enc[11] ^ enc[8]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[8] ^ enc[10]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[9] ^ enc[11]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[8] ^= t ^ u;
    dec[9] ^= t ^ v;
    dec[10] ^= t ^ u;
    dec[11] ^= t ^ v;
    t = enc[12] ^ enc[13] ^ enc[14] ^ enc[15];
    dec[12] = t ^ enc[12] ^ xeXeCryptAesMul2(enc[12] ^ enc[13]);
    dec[13] = t ^ enc[13] ^ xeXeCryptAesMul2(enc[13] ^ enc[14]);
    dec[14] = t ^ enc[14] ^ xeXeCryptAesMul2(enc[14] ^ enc[15]);
    dec[15] = t ^ enc[15] ^ xeXeCryptAesMul2(enc[15] ^ enc[12]);
    u = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[12] ^ enc[14]));
    v = xeXeCryptAesMul2(xeXeCryptAesMul2(enc[13] ^ enc[15]));
    t = xeXeCryptAesMul2(u ^ v);
    dec[12] ^= t ^ u;
    dec[13] ^= t ^ v;
    dec[14] ^= t ^ u;
    dec[15] ^= t ^ v;
  }
  std::memcpy(state_ptr->keytabdec[10], state_ptr->keytabenc[10], 16);
}

void XeCryptAesEcb_entry(ppc_ptr_t<XECRYPT_AES_STATE> state_ptr, mapped_void inp_ptr,
                         mapped_void out_ptr, u32 encrypt) {
  const uint8_t* keytab = reinterpret_cast<const uint8_t*>(state_ptr->keytabenc);
  if (encrypt) {
    aes_encrypt_128(keytab, inp_ptr, out_ptr);
  } else {
    aes_decrypt_128(keytab, inp_ptr, out_ptr);
  }
}

void XeCryptAesCbc_entry(ppc_ptr_t<XECRYPT_AES_STATE> state_ptr, mapped_void inp_ptr, u32 inp_size,
                         mapped_void out_ptr, mapped_void feed_ptr, u32 encrypt) {
  const uint8_t* keytab = reinterpret_cast<const uint8_t*>(state_ptr->keytabenc);
  const uint8_t* inp = inp_ptr.as<const uint8_t*>();
  uint8_t* out = out_ptr.as<uint8_t*>();
  uint8_t* feed = feed_ptr.as<uint8_t*>();
  // A partial block at the end is left alone.
  inp_size &= ~15u;
  if (encrypt) {
    for (uint32_t i = 0; i < inp_size; i += 16) {
      for (uint32_t j = 0; j < 16; ++j) {
        feed[j] ^= inp[j];
      }
      aes_encrypt_128(keytab, feed, feed);
      std::memcpy(out, feed, 16);
      inp += 16;
      out += 16;
    }
  } else {
    for (uint32_t i = 0; i < inp_size; i += 16) {
      // In case inp == out.
      uint8_t tmp[16];
      std::memcpy(tmp, inp, 16);
      aes_decrypt_128(keytab, inp, out);
      for (uint32_t j = 0; j < 16; ++j) {
        out[j] ^= feed[j];
      }
      std::memcpy(feed, tmp, 16);
      inp += 16;
      out += 16;
    }
  }
}

void XeCryptHmacSha_entry(mapped_void key, u32 key_size_in, mapped_void inp_1, u32 inp_1_size,
                          mapped_void inp_2, u32 inp_2_size, mapped_void inp_3, u32 inp_3_size,
                          mapped_void out, u32 out_size);

// Keys
// TODO: Array of keys we need

// Retail key 0x19
static const uint8_t key19[] = {0xE1, 0xBC, 0x15, 0x9C, 0x73, 0xB1, 0xEA, 0xE9,
                                0xAB, 0x31, 0x70, 0xF3, 0xAD, 0x47, 0xEB, 0xF3};

u32 XeKeysHmacSha_entry(u32 key_num, mapped_void inp_1, u32 inp_1_size, mapped_void inp_2,
                        u32 inp_2_size, mapped_void inp_3, u32 inp_3_size, mapped_void out,
                        u32 out_size) {
  const uint8_t* key = nullptr;
  if (key_num == 0x19) {
    key = key19;
  }

  if (key) {
    XeCryptHmacSha_entry(mapped_void::from_host((void*)key), 0x10, inp_1, inp_1_size, inp_2,
                         inp_2_size, inp_3, inp_3_size, out, out_size);

    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

static const uint8_t xe_key_obfuscation_key[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

u32 XeKeysAesCbcUsingKey_entry(mapped_void obscured_key, mapped_void inp_ptr, u32 inp_size,
                               mapped_void out_ptr, mapped_void feed_ptr, u32 encrypt) {
  uint8_t key[16];

  // Deobscure key
  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes),
                      mapped_void::from_host((void*)xe_key_obfuscation_key));
  XeCryptAesEcb_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes), obscured_key,
                      mapped_void::from_host(key), 0);

  // Run CBC using deobscured key
  XeCryptAesKey_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes), mapped_void::from_host(key));
  XeCryptAesCbc_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes), inp_ptr, inp_size, out_ptr,
                      feed_ptr, encrypt);

  return X_STATUS_SUCCESS;
}

u32 XeKeysObscureKey_entry(mapped_void input, mapped_void output) {
  // Based on HvxKeysObscureKey
  // Seems to encrypt input with per-console KEY_OBFUSCATION_KEY (key 0x18)

  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes),
                      mapped_void::from_host((void*)xe_key_obfuscation_key));
  XeCryptAesEcb_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes), input, output, 1);

  return X_STATUS_SUCCESS;
}

u32 XeKeysHmacShaUsingKey_entry(mapped_void obscured_key, mapped_void inp_1, u32 inp_1_size,
                                mapped_void inp_2, u32 inp_2_size, mapped_void inp_3,
                                u32 inp_3_size, mapped_void out, u32 out_size) {
  if (!obscured_key) {
    return X_STATUS_INVALID_PARAMETER;
  }

  uint8_t key[16];

  // Deobscure key
  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes),
                      mapped_void::from_host((void*)xe_key_obfuscation_key));
  XeCryptAesEcb_entry(ppc_ptr_t<XECRYPT_AES_STATE>::from_host(&aes), obscured_key,
                      mapped_void::from_host(key), 0);

  XeCryptHmacSha_entry(mapped_void::from_host(key), 0x10, inp_1, inp_1_size, inp_2, inp_2_size,
                       inp_3, inp_3_size, out, out_size);
  return X_STATUS_SUCCESS;
}

//=============================================================================
// Ported from xenia-canary (xboxkrnl_crypt.cc) for the ReXGlue fork
//=============================================================================

namespace {

void ShaStateInit(XECRYPT_SHA_STATE* state) {
  std::memset(state, 0, sizeof(*state));
  state->state[0] = 0x67452301;
  state->state[1] = 0xEFCDAB89;
  state->state[2] = 0x98BADCFE;
  state->state[3] = 0x10325476;
  state->state[4] = 0xC3D2E1F0;
}

void ShaStateUpdate(XECRYPT_SHA_STATE* state, const void* data, size_t size) {
  sha1::SHA1 sha;
  InitSha1(&sha, state);
  sha.processBytes(data, size);
  StoreSha1(&sha, state);
}

// XeCryptShaFinal on a host state: the state words keep the digest, the buffer
// the last block compressed, the count is unchanged.
void ShaStateFinal(XECRYPT_SHA_STATE* state, uint8_t* out, uint32_t out_size) {
  sha1::SHA1 sha;
  InitSha1(&sha, state);
  uint8_t digest[0x14];
  sha.finalize(digest);
  std::copy_n(sha.getDigest(), rex::countof(state->state), state->state);
  std::copy_n(sha.getBlock(), rex::countof(state->buffer), state->buffer);
  if (out && out_size) {
    std::memcpy(out, digest, std::min<uint32_t>(out_size, sizeof(digest)));
  }
}

}  // namespace

// HMAC-SHA1 in steps: sha_state[0] is the inner hash, seeded with the key XOR
// ipad, and sha_state[1] the outer one, seeded with the key XOR opad. The
// console cuts a key longer than a block to 64 bytes instead of hashing it.
struct XECRYPT_HMACSHA_STATE {
  XECRYPT_SHA_STATE sha_state[2];
};
static_assert_size(XECRYPT_HMACSHA_STATE, 0xB0);

void XeCryptHmacShaInit_entry(ppc_ptr_t<XECRYPT_HMACSHA_STATE> state, mapped_void key,
                              u32 key_size_in) {
  uint8_t block[0x40] = {};
  const uint32_t key_size = std::min<uint32_t>(key_size_in, 0x40);
  if (key_size) {
    std::memcpy(block, key, key_size);
  }

  uint8_t pad[0x40];
  for (uint32_t i = 0; i < 0x40; i++) {
    pad[i] = block[i] ^ 0x36;
  }
  ShaStateInit(&state->sha_state[0]);
  ShaStateUpdate(&state->sha_state[0], pad, sizeof(pad));

  for (uint32_t i = 0; i < 0x40; i++) {
    pad[i] = block[i] ^ 0x5C;
  }
  ShaStateInit(&state->sha_state[1]);
  ShaStateUpdate(&state->sha_state[1], pad, sizeof(pad));
}

void XeCryptHmacShaUpdate_entry(ppc_ptr_t<XECRYPT_HMACSHA_STATE> state, mapped_void input,
                                u32 input_size) {
  ShaStateUpdate(&state->sha_state[0], input, input_size);
}

void XeCryptHmacShaFinal_entry(ppc_ptr_t<XECRYPT_HMACSHA_STATE> state, ppc_ptr_t<uint8_t> out,
                               u32 out_size) {
  uint8_t digest[0x14];
  ShaStateFinal(&state->sha_state[0], digest, sizeof(digest));
  ShaStateUpdate(&state->sha_state[1], digest, sizeof(digest));
  ShaStateFinal(&state->sha_state[1], out, out_size);
}

void XeCryptHmacSha_entry(mapped_void key, u32 key_size_in, mapped_void inp_1, u32 inp_1_size,
                          mapped_void inp_2, u32 inp_2_size, mapped_void inp_3, u32 inp_3_size,
                          mapped_void out, u32 out_size) {
  XECRYPT_HMACSHA_STATE state;
  auto state_ptr = ppc_ptr_t<XECRYPT_HMACSHA_STATE>::from_host(&state);
  XeCryptHmacShaInit_entry(state_ptr, key, key_size_in);
  for (auto [input, size] : {std::pair{inp_1, inp_1_size}, std::pair{inp_2, inp_2_size},
                             std::pair{inp_3, inp_3_size}}) {
    if (size) {
      XeCryptHmacShaUpdate_entry(state_ptr, input, size);
    }
  }
  XeCryptHmacShaFinal_entry(state_ptr, ppc_ptr_t<uint8_t>::from_host(out.as<uint8_t*>()),
                            out_size);
}

u32 XeCryptHmacShaVerify_entry(mapped_void key, u32 key_size, mapped_void inp_1, u32 inp_1_size,
                               mapped_void inp_2, u32 inp_2_size, mapped_void inp_3,
                               u32 inp_3_size, mapped_void ver, u32 ver_size) {
  uint8_t digest[0x14];
  XeCryptHmacSha_entry(key, key_size, inp_1, inp_1_size, inp_2, inp_2_size, inp_3, inp_3_size,
                       mapped_void::from_host(digest), sizeof(digest));

  const size_t compare_size = std::min<size_t>(sizeof(digest), ver_size);
  return std::memcmp(digest, ver, compare_size) == 0;
}

void XeCryptHmacMd5_entry(mapped_void key, u32 key_size, mapped_void inp_1, u32 inp_1_size,
                          mapped_void inp_2, u32 inp_2_size, mapped_void inp_3, u32 inp_3_size,
                          mapped_void out, u32 out_size) {
  XeCryptHmacMd5(key.as<const uint8_t*>(), key_size, inp_1.as<const uint8_t*>(), inp_1_size,
                 inp_2.as<const uint8_t*>(), inp_2_size, inp_3.as<const uint8_t*>(), inp_3_size,
                 out.as<uint8_t*>(), out_size);
}

// The guest SHA-512 state mirrors the private FFmpeg context (AVSHA512 in
// libavutil/sha512.c of thirdparty/FFmpeg), which is loaded from and stored back
// to guest memory around every call, as xenia-canary does.
struct XECRYPT_SHA512_STATE {
  rex::be<uint64_t> count;
  rex::be<uint64_t> state[8];
  uint8_t buffer[128];
};

namespace {

struct FfmpegSha512Layout {
  uint8_t digest_len;
  uint64_t count;
  uint8_t buffer[128];
  uint64_t state[8];
};

struct AvFree {
  void operator()(void* p) const { av_free(p); }
};

std::unique_ptr<AVSHA512, AvFree> LoadSha512(const XECRYPT_SHA512_STATE* state) {
  std::unique_ptr<AVSHA512, AvFree> sha(av_sha512_alloc());
  av_sha512_init(sha.get(), 512);
  auto* ctx = reinterpret_cast<FfmpegSha512Layout*>(sha.get());
  std::copy(std::begin(state->state), std::end(state->state), ctx->state);
  std::copy(std::begin(state->buffer), std::end(state->buffer), ctx->buffer);
  ctx->count = state->count;
  return sha;
}

void StoreSha512(AVSHA512* sha, XECRYPT_SHA512_STATE* state) {
  const auto* ctx = reinterpret_cast<const FfmpegSha512Layout*>(sha);
  std::copy(std::begin(ctx->state), std::end(ctx->state), state->state);
  std::copy(std::begin(ctx->buffer), std::end(ctx->buffer), state->buffer);
  state->count = ctx->count;
}

}  // namespace

// MD5 runs on the portable implementation with the console's 0x54-byte state
// (rex/kernel/xboxkrnl/xecrypt_md5.h).
void XeCryptMd5Init_entry(ppc_ptr_t<XECRYPT_MD5_STATE> md5_state) {
  XeCryptMd5Init(md5_state);
}

void XeCryptMd5Update_entry(ppc_ptr_t<XECRYPT_MD5_STATE> md5_state, mapped_void input,
                            u32 input_size) {
  XeCryptMd5Update(md5_state, input.as<const uint8_t*>(), input_size);
}

void XeCryptMd5Final_entry(ppc_ptr_t<XECRYPT_MD5_STATE> md5_state, ppc_ptr_t<uint8_t> out,
                           u32 out_size) {
  XeCryptMd5Final(md5_state, out, out_size);
}

void XeCryptMd5_entry(mapped_void input_1, u32 input_1_size, mapped_void input_2, u32 input_2_size,
                      mapped_void input_3, u32 input_3_size, mapped_void out, u32 out_size) {
  XeCryptMd5(input_1.as<const uint8_t*>(), input_1_size, input_2.as<const uint8_t*>(),
             input_2_size, input_3.as<const uint8_t*>(), input_3_size, out.as<uint8_t*>(),
             out_size);
}

void XeCryptSha512Init_entry(ppc_ptr_t<XECRYPT_SHA512_STATE> sha_state) {
  sha_state.Zero();
  sha_state->state[0] = 0x6A09E667F3BCC908ull;
  sha_state->state[1] = 0xBB67AE8584CAA73Bull;
  sha_state->state[2] = 0x3C6EF372FE94F82Bull;
  sha_state->state[3] = 0xA54FF53A5F1D36F1ull;
  sha_state->state[4] = 0x510E527FADE682D1ull;
  sha_state->state[5] = 0x9B05688C2B3E6C1Full;
  sha_state->state[6] = 0x1F83D9ABFB41BD6Bull;
  sha_state->state[7] = 0x5BE0CD19137E2179ull;
}

void XeCryptSha512Update_entry(ppc_ptr_t<XECRYPT_SHA512_STATE> sha_state, mapped_void input,
                               u32 input_size) {
  auto sha = LoadSha512(sha_state);
  av_sha512_update(sha.get(), static_cast<const uint8_t*>(static_cast<void*>(input)), input_size);
  StoreSha512(sha.get(), sha_state);
}

void XeCryptSha512Final_entry(ppc_ptr_t<XECRYPT_SHA512_STATE> sha_state, ppc_ptr_t<uint8_t> out,
                              u32 out_size) {
  auto sha = LoadSha512(sha_state);
  uint8_t hash[64];
  av_sha512_final(sha.get(), hash);
  std::copy_n(hash, std::min<size_t>(sizeof(hash), out_size), static_cast<uint8_t*>(out));
  std::copy(std::begin(hash), std::end(hash), sha_state->buffer);
}

void XeCryptDesKey_entry(ppc_ptr_t<XECRYPT_DES_STATE> state_ptr, mapped_u64 key) {
  DES des(key[0]);
  std::memcpy(state_ptr->keytab, des.get_sub_key(), sizeof(state_ptr->keytab));
}

void XeCryptDesEcb_entry(ppc_ptr_t<XECRYPT_DES_STATE> state_ptr, mapped_u64 inp, mapped_u64 out,
                         u32 encrypt) {
  DES des(reinterpret_cast<ui64*>(state_ptr->keytab));
  if (encrypt) {
    *out = des.encrypt(*inp);
  } else {
    *out = des.decrypt(*inp);
  }
}

// A partial block at the end is left alone.
void XeCryptDesCbc_entry(ppc_ptr_t<XECRYPT_DES_STATE> state_ptr, mapped_u64 inp, u32 inp_size,
                         mapped_u64 out, mapped_u64 feed, u32 encrypt) {
  DES des(reinterpret_cast<ui64*>(state_ptr->keytab));
  uint64_t last_block = *feed;
  for (uint32_t i = 0; i < inp_size / 8; i++) {
    uint64_t block = inp[i];
    if (encrypt) {
      last_block = des.encrypt(block ^ last_block);
      out[i] = last_block;
    } else {
      out[i] = des.decrypt(block) ^ last_block;
      last_block = block;
    }
  }
  *feed = last_block;
}

u32 XeCryptBnQwBeSigVerify_entry(mapped_void sig, mapped_void hash, mapped_string salt,
                                 mapped_void rsa) {
  // Signatures are not checked.
  return 1;
}

void XeCryptRotSumSha_entry(mapped_void inp_1, u32 inp_1_size, mapped_void inp_2, u32 inp_2_size,
                            mapped_void out, u32 out_size) {
  // Only feeds XeCryptBnQwBeSigVerify, which accepts everything.
}

//=============================================================================
// Key vault
//=============================================================================

namespace {

// Console serial number.
constexpr uint8_t kKey14[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '1', '0'};

// Roamable obfuscation key (retail).
constexpr uint8_t kKey19[] = {0xE1, 0xBC, 0x15, 0x9C, 0x73, 0xB1, 0xEA, 0xE9,
                              0xAB, 0x31, 0x70, 0xF3, 0xAD, 0x47, 0xEB, 0xF3};

// PIRS retail public key (XECRYPT_RSAPUB_2048).
constexpr uint8_t kKey39[] = {
    0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xE6, 0x3B, 0x32, 0xB2, 0x8D, 0x9E, 0x9E, 0xE7, 0x9D, 0xFC, 0x5C, 0x72, 0x41, 0x94, 0x58, 0x47,
    0xDE, 0x0D, 0x18, 0x40, 0x72, 0xD6, 0xE3, 0x46, 0x8E, 0xBA, 0x8E, 0xBC, 0x1A, 0x90, 0xAC, 0x20,
    0xBA, 0x03, 0x85, 0xB5, 0x1A, 0x3E, 0x25, 0xF9, 0xA6, 0x58, 0xEB, 0xB6, 0xA3, 0xC4, 0xA3, 0xEE,
    0xB2, 0xB0, 0xAE, 0x97, 0x69, 0xEB, 0xFE, 0x71, 0xFC, 0x02, 0xAB, 0x77, 0xBA, 0xC8, 0xE6, 0x74,
    0xE6, 0x7C, 0x63, 0x0E, 0xAF, 0x4C, 0xF7, 0xE7, 0x11, 0x4A, 0x80, 0x24, 0x72, 0x05, 0x7A, 0x63,
    0xD0, 0xF8, 0x91, 0x02, 0xA6, 0xE7, 0x7D, 0x77, 0xC5, 0xA7, 0x9B, 0x08, 0x11, 0x2E, 0xA0, 0x64,
    0x45, 0x60, 0x46, 0xBC, 0x36, 0xE1, 0x17, 0x71, 0xBE, 0x66, 0x49, 0x2F, 0xAE, 0x20, 0xA4, 0x76,
    0x9C, 0x27, 0x51, 0xCF, 0x4B, 0x34, 0x7A, 0x35, 0xBC, 0xA4, 0xAA, 0x1C, 0x47, 0x4B, 0xF4, 0x97,
    0x22, 0x4E, 0x13, 0x24, 0xD3, 0xC1, 0x57, 0xDF, 0x4D, 0x84, 0xB9, 0x18, 0x97, 0x99, 0xAC, 0x00,
    0xB3, 0x3D, 0x03, 0x25, 0x60, 0xC8, 0x7A, 0x59, 0xFE, 0x48, 0xFF, 0x28, 0x3D, 0x10, 0xBB, 0x9E,
    0x09, 0x06, 0x2A, 0x61, 0x20, 0x2C, 0xF8, 0x72, 0xEB, 0x87, 0xE6, 0xD1, 0xFB, 0xB3, 0x66, 0xFC,
    0x4A, 0x02, 0xAE, 0xD4, 0xD8, 0x37, 0xCF, 0xA6, 0x32, 0x25, 0x79, 0x36, 0x0E, 0xF4, 0xED, 0x19,
    0xA2, 0x10, 0x27, 0x96, 0x2F, 0x9F, 0xA9, 0x3D, 0xA4, 0x37, 0x30, 0x11, 0x51, 0x83, 0xBD, 0xF7,
    0xC7, 0xE5, 0xCE, 0xAA, 0xEC, 0xDE, 0x48, 0xA0, 0x84, 0xF7, 0xB0, 0xF6, 0x4B, 0x8E, 0xF0, 0x89,
    0xBD, 0x47, 0x7C, 0x90, 0xDD, 0x88, 0x12, 0x17, 0x40, 0xD2, 0x4E, 0xA6, 0xC6, 0x11, 0x04, 0x1B,
    0x57, 0xA8, 0x68, 0xB4, 0x61, 0xF4, 0x1B, 0xC6, 0x8B, 0xE8, 0xD9, 0x20, 0xF2, 0x05, 0xE0, 0x70};
static_assert(sizeof(kKey39) == 0x110);

std::span<const uint8_t> FindVaultKey(uint32_t key) {
  switch (key) {
    case 0x14:
      return kKey14;
    case 0x19:
      return kKey19;
    case 0x39:
      return kKey39;
    default:
      return {};
  }
}

// Key vault offset and size of each key index
// (https://github.com/emoose/ExCrypt/blob/master/src/exkeys.cpp).
struct KeyProperties {
  uint32_t index;
  uint32_t offset;
  uint32_t size;
};
constexpr KeyProperties kKeyProperties[] = {
    {0x00, 0x8, 0x1},       {0x01, 0x9, 0x1},      {0x02, 0xA, 0x1},     {0x03, 0xB, 0x1},
    {0x04, 0xC, 0x2},       {0x05, 0xE, 0x2},      {0x06, 0x10, 0x4},    {0x07, 0x14, 0x4},
    {0x08, 0x18, 0x4},      {0x09, 0x1C, 0x4},     {0x0A, 0x20, 0x8},    {0x0B, 0x28, 0x8},
    {0x0C, 0x30, 0x8},      {0x0D, 0x38, 0x8},     {0x0E, 0x40, 0x10},   {0x0F, 0x50, 0x10},
    {0x10, 0x60, 0x10},     {0x11, 0x70, 0x10},    {0x12, 0x80, 0x10},   {0x13, 0x90, 0x10},
    {0x14, 0xA0, 0xC},      {0x15, 0xAC, 0xC},     {0x16, 0xB8, 0x2},    {0x17, 0xC0, 0x10},
    {0x18, 0xD0, 0x10},     {0x19, 0xE0, 0x10},    {0x1A, 0xF0, 0x10},   {0x1B, 0x100, 0x18},
    {0x1C, 0x118, 0x10},    {0x1D, 0x128, 0x10},   {0x1E, 0x138, 0x10},  {0x1F, 0x148, 0x10},
    {0x20, 0x158, 0x10},    {0x21, 0x168, 0x10},   {0x22, 0x178, 0x10},  {0x23, 0x188, 0x10},
    {0x24, 0x198, 0x10},    {0x25, 0x1A8, 0x10},   {0x26, 0x1B8, 0x10},  {0x27, 0x1C8, 0x10},
    {0x28, 0x1D8, 0x10},    {0x29, 0x1E8, 0x10},   {0x2A, 0x1F8, 0x10},  {0x2B, 0x208, 0x10},
    {0x2C, 0x218, 0x10},    {0x2D, 0x228, 0x10},   {0x2E, 0x238, 0x10},  {0x2F, 0x248, 0x10},
    {0x30, 0x258, 0x10},    {0x31, 0x268, 0x10},   {0x32, 0x278, 0x10},  {0x33, 0x288, 0x1D0},
    {0x34, 0x458, 0x390},   {0x35, 0x7E8, 0x1D0},  {0x36, 0x9B8, 0x1A8}, {0x37, 0xB60, 0x1288},
    {0x38, 0x1EE8, 0x2108}, {0x44, 0x1DF8, 0x100},
};

constexpr uint32_t kConsoleTypeRetail = 2;

}  // namespace

#pragma pack(push, 1)
struct XE_CONSOLE_ID {
  union {
    struct {
      uint8_t RefurbBits : 4;
      uint8_t ManufactureMonth : 4;
      uint32_t ManufactureYear : 4;
      uint32_t MacIndex3 : 8;
      uint32_t MacIndex4 : 8;
      uint32_t MacIndex5 : 8;
      uint32_t Crc : 4;
    };
    uint8_t Data[5];
  };
};
#pragma pack(pop)

u32 XeKeysGetConsoleID_entry(ppc_ptr_t<XE_CONSOLE_ID> raw_bytes, mapped_string hex_string) {
  // A fixed ID; no real key vault is used.
  if (raw_bytes) {
    raw_bytes.Zero();
    raw_bytes->RefurbBits = 0b0011;
    raw_bytes->ManufactureMonth = 0b1001;
    raw_bytes->ManufactureYear = 0b0001;
    raw_bytes->MacIndex3 = 0b01000000;
    raw_bytes->MacIndex4 = 0b01100110;
    raw_bytes->MacIndex5 = 0b01111110;
    raw_bytes->Crc = 0b0000;
  }
  if (hex_string) {
    std::memcpy(hex_string, "245149100000", 0xC);
  }
  return X_STATUS_SUCCESS;
}

// Console certificate and signature, as in xenia-canary.
struct X_CONSOLE_PUBLIC_KEY {
  uint8_t public_exponent[0x4];  // 0x0
  uint8_t modulus[0x80];         // 0x4
};
static_assert_size(X_CONSOLE_PUBLIC_KEY, 0x84);

struct XECRYPT_SIG {
  rex::be<uint64_t> aqwPad[0x1C];
  uint8_t one;
  uint8_t salt[0xA];
  uint8_t hash[0x14];
  uint8_t end;
};
static_assert_size(XECRYPT_SIG, 0x100);

struct XE_CONSOLE_CERTIFICATE {
  rex::be<uint16_t> cert_size;              // 0x0
  XE_CONSOLE_ID console_id;                 // 0x2
  uint8_t console_part_number[11];          // 0x7
  uint8_t reserved[4];                      // 0x12
  rex::be<uint16_t> privileges;             // 0x16
  rex::be<uint32_t> console_type;           // 0x18
  uint8_t manufacture_date[8];              // 0x1C
  X_CONSOLE_PUBLIC_KEY console_public_key;  // 0x24
  XECRYPT_SIG signature;                    // 0xA8
};
static_assert_size(XE_CONSOLE_CERTIFICATE, 0x1A8);

// BOOL: the hash is "signed" with a plausible retail certificate in front of
// the signature (XE_CONSOLE_SIGNATURE starts with the certificate).
u32 XeKeysConsolePrivateKeySign_entry(mapped_void hash, ppc_ptr_t<XE_CONSOLE_CERTIFICATE> output) {
  if (!hash || !output) {
    return 0;
  }
  output.Zero();
  output->console_id.RefurbBits = 0b0011;
  output->console_id.ManufactureMonth = 0b1001;
  output->console_id.ManufactureYear = 0b0001;
  output->console_id.MacIndex3 = 0b01000000;
  output->console_id.MacIndex4 = 0b01100110;
  output->console_id.MacIndex5 = 0b01111110;
  output->console_id.Crc = 0b0000;
  output->console_type = kConsoleTypeRetail;
  const uint8_t manufacture_date[8] = {2, 0, 0, 5, 1, 1, 2, 2};
  std::memcpy(output->manufacture_date, manufacture_date, sizeof(manufacture_date));
  return 1;
}

// BOOL: signatures are not checked, every one is valid.
u32 XeKeysConsoleSignatureVerification_entry(mapped_void hash, mapped_void signature,
                                             mapped_void pubkey) {
  return 1;
}

u32 XeKeysGetConsoleType_entry(mapped_u32 type_out) {
  *type_out = kConsoleTypeRetail;
  return X_STATUS_SUCCESS;
}

u32 XeKeysGetKeyProperties_entry(u32 key) {
  for (const auto& properties : kKeyProperties) {
    if (properties.index == key) {
      return properties.size;
    }
  }
  REXKRNL_WARN("XeKeysGetKeyProperties: key 0x{:04X} not implemented", uint32_t(key));
  return 0;
}

u32 XeKeysGetKey_entry(u32 key, mapped_void key_buffer, mapped_u32 key_length) {
  const auto data = FindVaultKey(key & 0xFFFF);
  if (data.empty()) {
    REXKRNL_WARN("XeKeysGetKey: key 0x{:04X} not implemented", uint32_t(key) & 0xFFFF);
  } else if (key_buffer && key_length) {
    std::memcpy(key_buffer, data.data(), std::min<size_t>(data.size(), *key_length));
  }
  return X_STATUS_SUCCESS;
}

REX_EXPORT_STUB(__imp__XeKeysGetConsoleCertificate);
REX_EXPORT_STUB(__imp__XeCryptBnDwLeDhEqualBase);
REX_EXPORT_STUB(__imp__XeCryptBnDwLeDhInvalBase);
REX_EXPORT_STUB(__imp__XeCryptBnDwLeDhModExp);
REX_EXPORT_STUB(__imp__XeCryptBnDw_Copy);
REX_EXPORT_STUB(__imp__XeCryptBnDw_SwapLeBe);
REX_EXPORT_STUB(__imp__XeCryptBnDw_Zero);
REX_EXPORT_STUB(__imp__XeCryptBnDwLePkcs1Format);
REX_EXPORT_STUB(__imp__XeCryptBnQwBeSigCreate);
REX_EXPORT_STUB(__imp__XeCryptBnQwBeSigFormat);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeModExp);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeModExpRoot);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeModInv);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeModMul);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeRsaKeyGen);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeRsaPrvCrypt);
REX_EXPORT_STUB(__imp__XeCryptBnQw_Copy);
REX_EXPORT_STUB(__imp__XeCryptBnQw_SwapDwQw);
REX_EXPORT_STUB(__imp__XeCryptBnQw_SwapLeBe);
REX_EXPORT_STUB(__imp__XeCryptBnQw_Zero);
REX_EXPORT_STUB(__imp__XeCryptChainAndSumMac);
REX_EXPORT_STUB(__imp__XeCryptHmacMd5Init);
REX_EXPORT_STUB(__imp__XeCryptHmacMd5Update);
REX_EXPORT_STUB(__imp__XeCryptHmacMd5Final);
REX_EXPORT_STUB(__imp__XeCryptParveEcb);
REX_EXPORT_STUB(__imp__XeCryptParveCbcMac);
REX_EXPORT_STUB(__imp__XeCryptSha384Init);
REX_EXPORT_STUB(__imp__XeCryptSha384Update);
REX_EXPORT_STUB(__imp__XeCryptSha384Final);
REX_EXPORT_STUB(__imp__XeCryptSha384);
REX_EXPORT_STUB(__imp__XeCryptSha512);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeCompare);
REX_EXPORT_STUB(__imp__XeKeysGetFactoryChallenge);
REX_EXPORT_STUB(__imp__XeKeysSetFactoryResponse);
REX_EXPORT_STUB(__imp__XeKeysInitializeFuses);
REX_EXPORT_STUB(__imp__XeKeysSaveBootLoader);
REX_EXPORT_STUB(__imp__XeKeysSaveKeyVault);
REX_EXPORT_STUB(__imp__XeKeysGetStatus);
REX_EXPORT_STUB(__imp__XeKeysGeneratePrivateKey);
REX_EXPORT_STUB(__imp__XeKeysSetKey);
REX_EXPORT_STUB(__imp__XeKeysGenerateRandomKey);
REX_EXPORT_STUB(__imp__XeKeysGetDigest);
REX_EXPORT_STUB(__imp__XeKeysQwNeRsaPrvCrypt);
REX_EXPORT_STUB(__imp__XeKeysAesCbc);
REX_EXPORT_STUB(__imp__XeKeysDes2Cbc);
REX_EXPORT_STUB(__imp__XeKeysDesCbc);
REX_EXPORT_STUB(__imp__XeKeysSaveBootLoaderEx);
REX_EXPORT_STUB(__imp__XeKeysDes2CbcUsingKey);
REX_EXPORT_STUB(__imp__XeKeysDesCbcUsingKey);
REX_EXPORT_STUB(__imp__XeKeysObfuscate);
REX_EXPORT_STUB(__imp__XeKeysUnObfuscate);
REX_EXPORT_STUB(__imp__XeKeysVerifyRSASignature);
REX_EXPORT_STUB(__imp__XeKeysSaveSystemUpdate);
REX_EXPORT_STUB(__imp__XeKeysLockSystemUpdate);
REX_EXPORT_STUB(__imp__XeKeysExecute);
REX_EXPORT_STUB(__imp__XeKeysGetVersions);
REX_EXPORT_STUB(__imp__XeKeysSetRevocationList);
REX_EXPORT_STUB(__imp__XeKeysExSaveKeyVault);
REX_EXPORT_STUB(__imp__XeKeysExSetKey);
REX_EXPORT_STUB(__imp__XeKeysExGetKey);
REX_EXPORT_STUB(__imp__XeKeysSecurityInitialize);
REX_EXPORT_STUB(__imp__XeKeysSecurityLoadSettings);
REX_EXPORT_STUB(__imp__XeKeysSecuritySaveSettings);
REX_EXPORT_STUB(__imp__XeKeysSecuritySetDetected);
REX_EXPORT_STUB(__imp__XeKeysSecurityGetDetected);
REX_EXPORT_STUB(__imp__XeKeysSecuritySetActivated);
REX_EXPORT_STUB(__imp__XeKeysSecurityGetActivated);
REX_EXPORT_STUB(__imp__XeKeysDvdAuthAP25InstallTable);
REX_EXPORT_STUB(__imp__XeKeysDvdAuthAP25GetTableVersion);
REX_EXPORT_STUB(__imp__XeKeysGetProtectedFlag);
REX_EXPORT_STUB(__imp__XeKeysSetProtectedFlag);
REX_EXPORT_STUB(__imp__XeKeysGetUpdateSequence);
REX_EXPORT_STUB(__imp__XeKeysDvdAuthExActivate);
REX_EXPORT_STUB(__imp__XeKeysRevokeSaveSettings);
REX_EXPORT_STUB(__imp__XeKeysGetMediaID);
REX_EXPORT_STUB(__imp__XeKeysLoadKeyVault);
REX_EXPORT_STUB(__imp__XeKeysRevokeUpdateDynamic);
REX_EXPORT_STUB(__imp__XeKeysSecuritySetStat);
REX_EXPORT_STUB(__imp__XeKeysFcrtLoad);
REX_EXPORT_STUB(__imp__XeKeysFcrtSave);
REX_EXPORT_STUB(__imp__XeKeysFcrtSet);
REX_EXPORT_STUB(__imp__XeKeysRevokeIsDeviceRevoked);
REX_EXPORT_STUB(__imp__XeKeysDvdAuthExSave);
REX_EXPORT_STUB(__imp__XeKeysDvdAuthExInstall);
REX_EXPORT_STUB(__imp__XeKeysObfuscateEx);
REX_EXPORT_STUB(__imp__XeKeysUnObfuscateEx);
REX_EXPORT_STUB(__imp__XeKeysVerifyPIRSSignature);
REX_EXPORT_STUB(__imp__XeCryptAesCtr);
REX_EXPORT_STUB(__imp__XeCryptAesCbcMac);
REX_EXPORT_STUB(__imp__XeCryptAesDmMac);
REX_EXPORT_STUB(__imp__XeKeysGetTruncatedSecondaryConsoleId);
REX_EXPORT_STUB(__imp__XeCryptSha224Init);
REX_EXPORT_STUB(__imp__XeCryptAesCreateKeySchedule);
REX_EXPORT_STUB(__imp__XeCryptAesEncryptOne);
REX_EXPORT_STUB(__imp__XeCryptAesDecryptOne);
REX_EXPORT_STUB(__imp__XeCryptAesCbcEncrypt);
REX_EXPORT_STUB(__imp__XeCryptAesCbcDecrypt);
REX_EXPORT_STUB(__imp__XeCryptAesGcmInitialize);
REX_EXPORT_STUB(__imp__XeCryptAesGcmUpdate);
REX_EXPORT_STUB(__imp__XeCryptAesGcmFinalize);
REX_EXPORT_STUB(__imp__XeCryptEccGetCurveParameters);
REX_EXPORT_STUB(__imp__XeCryptEccEcdhGenerateKeypair);
REX_EXPORT_STUB(__imp__XeCryptEccEcdhExponentiate);
REX_EXPORT_STUB(__imp__XeCryptEccEcdsaGenerateSignature);
REX_EXPORT_STUB(__imp__XeCryptEccEcdsaVerifySignature);

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__XeCryptRc4Key, rex::kernel::xboxkrnl::XeCryptRc4Key_entry)
REX_EXPORT(__imp__XeCryptRc4Ecb, rex::kernel::xboxkrnl::XeCryptRc4Ecb_entry)
REX_EXPORT(__imp__XeCryptRc4, rex::kernel::xboxkrnl::XeCryptRc4_entry)
REX_EXPORT(__imp__XeCryptShaInit, rex::kernel::xboxkrnl::XeCryptShaInit_entry)
REX_EXPORT(__imp__XeCryptShaUpdate, rex::kernel::xboxkrnl::XeCryptShaUpdate_entry)
REX_EXPORT(__imp__XeCryptShaFinal, rex::kernel::xboxkrnl::XeCryptShaFinal_entry)
REX_EXPORT(__imp__XeCryptSha, rex::kernel::xboxkrnl::XeCryptSha_entry)
REX_EXPORT(__imp__XeCryptSha256, rex::kernel::xboxkrnl::XeCryptSha256_entry)
REX_EXPORT(__imp__XeCryptSha256Init, rex::kernel::xboxkrnl::XeCryptSha256Init_entry)
REX_EXPORT(__imp__XeCryptSha256Update, rex::kernel::xboxkrnl::XeCryptSha256Update_entry)
REX_EXPORT(__imp__XeCryptSha256Final, rex::kernel::xboxkrnl::XeCryptSha256Final_entry)
REX_EXPORT(__imp__XeCryptBnQw_SwapDwQwLeBe, rex::kernel::xboxkrnl::XeCryptBnQw_SwapDwQwLeBe_entry)
REX_EXPORT(__imp__XeCryptBnQwNeRsaPubCrypt, rex::kernel::xboxkrnl::XeCryptBnQwNeRsaPubCrypt_entry)
REX_EXPORT(__imp__XeCryptBnDwLePkcs1Verify, rex::kernel::xboxkrnl::XeCryptBnDwLePkcs1Verify_entry)
REX_EXPORT(__imp__XeCryptRandom, rex::kernel::xboxkrnl::XeCryptRandom_entry)
REX_EXPORT(__imp__XeCryptDesParity, rex::kernel::xboxkrnl::XeCryptDesParity_entry)
REX_EXPORT(__imp__XeCryptDes3Key, rex::kernel::xboxkrnl::XeCryptDes3Key_entry)
REX_EXPORT(__imp__XeCryptDes3Ecb, rex::kernel::xboxkrnl::XeCryptDes3Ecb_entry)
REX_EXPORT(__imp__XeCryptDes3Cbc, rex::kernel::xboxkrnl::XeCryptDes3Cbc_entry)
REX_EXPORT(__imp__XeCryptDesCbc, rex::kernel::xboxkrnl::XeCryptDesCbc_entry)
REX_EXPORT(__imp__XeCryptAesKey, rex::kernel::xboxkrnl::XeCryptAesKey_entry)
REX_EXPORT(__imp__XeCryptAesEcb, rex::kernel::xboxkrnl::XeCryptAesEcb_entry)
REX_EXPORT(__imp__XeCryptAesCbc, rex::kernel::xboxkrnl::XeCryptAesCbc_entry)
REX_EXPORT(__imp__XeCryptHmacSha, rex::kernel::xboxkrnl::XeCryptHmacSha_entry)
REX_EXPORT(__imp__XeCryptHmacShaVerify, rex::kernel::xboxkrnl::XeCryptHmacShaVerify_entry)
REX_EXPORT(__imp__XeCryptHmacMd5, rex::kernel::xboxkrnl::XeCryptHmacMd5_entry)
REX_EXPORT(__imp__XeCryptHmacShaInit, rex::kernel::xboxkrnl::XeCryptHmacShaInit_entry)
REX_EXPORT(__imp__XeCryptHmacShaUpdate, rex::kernel::xboxkrnl::XeCryptHmacShaUpdate_entry)
REX_EXPORT(__imp__XeCryptHmacShaFinal, rex::kernel::xboxkrnl::XeCryptHmacShaFinal_entry)
REX_EXPORT(__imp__XeCryptMd5Init, rex::kernel::xboxkrnl::XeCryptMd5Init_entry)
REX_EXPORT(__imp__XeCryptMd5Update, rex::kernel::xboxkrnl::XeCryptMd5Update_entry)
REX_EXPORT(__imp__XeCryptMd5Final, rex::kernel::xboxkrnl::XeCryptMd5Final_entry)
REX_EXPORT(__imp__XeCryptMd5, rex::kernel::xboxkrnl::XeCryptMd5_entry)
REX_EXPORT(__imp__XeCryptSha512Init, rex::kernel::xboxkrnl::XeCryptSha512Init_entry)
REX_EXPORT(__imp__XeCryptSha512Update, rex::kernel::xboxkrnl::XeCryptSha512Update_entry)
REX_EXPORT(__imp__XeCryptSha512Final, rex::kernel::xboxkrnl::XeCryptSha512Final_entry)
REX_EXPORT(__imp__XeCryptDesKey, rex::kernel::xboxkrnl::XeCryptDesKey_entry)
REX_EXPORT(__imp__XeCryptDesEcb, rex::kernel::xboxkrnl::XeCryptDesEcb_entry)
REX_EXPORT(__imp__XeKeysGetConsoleID, rex::kernel::xboxkrnl::XeKeysGetConsoleID_entry)
REX_EXPORT(__imp__XeKeysGetConsoleType, rex::kernel::xboxkrnl::XeKeysGetConsoleType_entry)
REX_EXPORT(__imp__XeKeysGetKeyProperties, rex::kernel::xboxkrnl::XeKeysGetKeyProperties_entry)
REX_EXPORT(__imp__XeKeysGetKey, rex::kernel::xboxkrnl::XeKeysGetKey_entry)
REX_EXPORT(__imp__XeCryptBnQwBeSigVerify, rex::kernel::xboxkrnl::XeCryptBnQwBeSigVerify_entry)
REX_EXPORT(__imp__XeCryptRotSumSha, rex::kernel::xboxkrnl::XeCryptRotSumSha_entry)
REX_EXPORT(__imp__XeKeysHmacSha, rex::kernel::xboxkrnl::XeKeysHmacSha_entry)
REX_EXPORT(__imp__XeKeysAesCbcUsingKey, rex::kernel::xboxkrnl::XeKeysAesCbcUsingKey_entry)
REX_EXPORT(__imp__XeKeysObscureKey, rex::kernel::xboxkrnl::XeKeysObscureKey_entry)
REX_EXPORT(__imp__XeKeysHmacShaUsingKey, rex::kernel::xboxkrnl::XeKeysHmacShaUsingKey_entry)
REX_EXPORT(__imp__XeKeysConsolePrivateKeySign,
           rex::kernel::xboxkrnl::XeKeysConsolePrivateKeySign_entry)
REX_EXPORT(__imp__XeKeysConsoleSignatureVerification,
           rex::kernel::xboxkrnl::XeKeysConsoleSignatureVerification_entry)

REX_EXPORT_STUB(__imp__DevAuthGetStatistics);
REX_EXPORT_STUB(__imp__DevAuthShouldAlwaysEnforce);
