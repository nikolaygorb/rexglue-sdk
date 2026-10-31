/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Ported for the ReXGlue fork
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <rex/assert.h>
#include <rex/types.h>

namespace rex {
namespace kernel {
namespace xboxkrnl {

// The console's MD5 context: a 32-bit byte count, the state words A to D and
// the block buffer.
struct XECRYPT_MD5_STATE {
  rex::be<uint32_t> count;     // 0x0
  rex::be<uint32_t> state[4];  // 0x4
  uint8_t buffer[64];          // 0x14
};
static_assert_size(XECRYPT_MD5_STATE, 0x54);

namespace md5_detail {

inline uint32_t RotateLeft(uint32_t value, uint32_t count) {
  return (value << count) | (value >> (32 - count));
}

// RFC 1321 compression of one 64-byte block.
inline void Transform(uint32_t abcd[4], const uint8_t block[64]) {
  static constexpr uint32_t kSine[64] = {
      0xD76AA478, 0xE8C7B756, 0x242070DB, 0xC1BDCEEE, 0xF57C0FAF, 0x4787C62A, 0xA8304613,
      0xFD469501, 0x698098D8, 0x8B44F7AF, 0xFFFF5BB1, 0x895CD7BE, 0x6B901122, 0xFD987193,
      0xA679438E, 0x49B40821, 0xF61E2562, 0xC040B340, 0x265E5A51, 0xE9B6C7AA, 0xD62F105D,
      0x02441453, 0xD8A1E681, 0xE7D3FBC8, 0x21E1CDE6, 0xC33707D6, 0xF4D50D87, 0x455A14ED,
      0xA9E3E905, 0xFCEFA3F8, 0x676F02D9, 0x8D2A4C8A, 0xFFFA3942, 0x8771F681, 0x6D9D6122,
      0xFDE5380C, 0xA4BEEA44, 0x4BDECFA9, 0xF6BB4B60, 0xBEBFBC70, 0x289B7EC6, 0xEAA127FA,
      0xD4EF3085, 0x04881D05, 0xD9D4D039, 0xE6DB99E5, 0x1FA27CF8, 0xC4AC5665, 0xF4292244,
      0x432AFF97, 0xAB9423A7, 0xFC93A039, 0x655B59C3, 0x8F0CCC92, 0xFFEFF47D, 0x85845DD1,
      0x6FA87E4F, 0xFE2CE6E0, 0xA3014314, 0x4E0811A1, 0xF7537E82, 0xBD3AF235, 0x2AD7D2BB,
      0xEB86D391};
  static constexpr uint32_t kShift[64] = {
      7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
      14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
      4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

  uint32_t words[16];
  for (uint32_t i = 0; i < 16; i++) {
    words[i] = uint32_t(block[i * 4]) | (uint32_t(block[i * 4 + 1]) << 8) |
               (uint32_t(block[i * 4 + 2]) << 16) | (uint32_t(block[i * 4 + 3]) << 24);
  }
  uint32_t a = abcd[0], b = abcd[1], c = abcd[2], d = abcd[3];
  for (uint32_t i = 0; i < 64; i++) {
    uint32_t f, g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    const uint32_t temp = d;
    d = c;
    c = b;
    b = b + RotateLeft(a + f + kSine[i] + words[g], kShift[i]);
    a = temp;
  }
  abcd[0] += a;
  abcd[1] += b;
  abcd[2] += c;
  abcd[3] += d;
}

}  // namespace md5_detail

inline void XeCryptMd5Init(XECRYPT_MD5_STATE* state) {
  std::memset(state, 0, sizeof(*state));
  state->state[0] = 0x67452301;
  state->state[1] = 0xEFCDAB89;
  state->state[2] = 0x98BADCFE;
  state->state[3] = 0x10325476;
}

// Every byte goes through the buffer, so it holds the last block compressed.
inline void XeCryptMd5Update(XECRYPT_MD5_STATE* state, const uint8_t* input,
                             uint32_t input_size) {
  uint32_t abcd[4];
  std::copy(std::begin(state->state), std::end(state->state), abcd);
  uint32_t used = state->count % 64;
  state->count = state->count + input_size;
  while (input_size) {
    const uint32_t size = std::min(64 - used, input_size);
    std::memcpy(state->buffer + used, input, size);
    used += size;
    input += size;
    input_size -= size;
    if (used == 64) {
      md5_detail::Transform(abcd, state->buffer);
      used = 0;
    }
  }
  std::copy(std::begin(abcd), std::end(abcd), state->state);
}

// Pads in place with a 32-bit bit count and keeps the count, so the last block
// compressed stays in the buffer. The state words are left byte swapped so
// they hold the digest.
inline void XeCryptMd5Final(XECRYPT_MD5_STATE* state, uint8_t* out, uint32_t out_size) {
  const uint32_t count = state->count;
  const uint32_t used = count % 64;
  const uint32_t length_offset = (used < 56 ? 56 : 120) - used;
  uint8_t padding[72] = {0x80};
  const uint32_t bit_count = count << 3;
  for (uint32_t i = 0; i < 4; i++) {
    padding[length_offset + i] = uint8_t(bit_count >> (i * 8));
  }
  XeCryptMd5Update(state, padding, length_offset + 8);
  state->count = count;
  for (auto& word : state->state) {
    word = rex::byte_swap(uint32_t(word));
  }
  if (out && out_size) {
    std::memcpy(out, state->state, std::min<uint32_t>(out_size, 16));
  }
}

inline void XeCryptMd5(const uint8_t* input_1, uint32_t input_1_size, const uint8_t* input_2,
                       uint32_t input_2_size, const uint8_t* input_3, uint32_t input_3_size,
                       uint8_t* output, uint32_t output_size) {
  XECRYPT_MD5_STATE state;
  XeCryptMd5Init(&state);
  if (input_1 && input_1_size) {
    XeCryptMd5Update(&state, input_1, input_1_size);
  }
  if (input_2 && input_2_size) {
    XeCryptMd5Update(&state, input_2, input_2_size);
  }
  if (input_3 && input_3_size) {
    XeCryptMd5Update(&state, input_3, input_3_size);
  }
  XeCryptMd5Final(&state, output, output_size);
}

// Long keys are assumed to truncate, as in XeCryptHmacSha.
inline void XeCryptHmacMd5(const uint8_t* key, uint32_t key_size_in, const uint8_t* input_1,
                           uint32_t input_1_size, const uint8_t* input_2, uint32_t input_2_size,
                           const uint8_t* input_3, uint32_t input_3_size, uint8_t* output,
                           uint32_t output_size) {
  uint8_t inner_pad[0x40];
  uint8_t outer_pad[0x40];
  std::memset(inner_pad, 0x36, sizeof(inner_pad));
  std::memset(outer_pad, 0x5C, sizeof(outer_pad));
  const uint32_t key_size = std::min<uint32_t>(key_size_in, 0x40);
  for (uint32_t i = 0; i < key_size; i++) {
    inner_pad[i] ^= key[i];
    outer_pad[i] ^= key[i];
  }

  XECRYPT_MD5_STATE state;
  XeCryptMd5Init(&state);
  XeCryptMd5Update(&state, inner_pad, sizeof(inner_pad));
  if (input_1 && input_1_size) {
    XeCryptMd5Update(&state, input_1, input_1_size);
  }
  if (input_2 && input_2_size) {
    XeCryptMd5Update(&state, input_2, input_2_size);
  }
  if (input_3 && input_3_size) {
    XeCryptMd5Update(&state, input_3, input_3_size);
  }
  uint8_t digest[16];
  XeCryptMd5Final(&state, digest, sizeof(digest));

  XeCryptMd5Init(&state);
  XeCryptMd5Update(&state, outer_pad, sizeof(outer_pad));
  XeCryptMd5Update(&state, digest, sizeof(digest));
  XeCryptMd5Final(&state, output, output_size);
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace rex
