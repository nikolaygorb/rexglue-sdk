/**
 * @file        kernel/crt/native_memory.h
 * @brief       Hooks that run a title's own memcpy, memset and memmove as host
 *              code, with the title's recompiled routine kept for the calls the
 *              host version would not reproduce exactly.
 *
 * The routines are linked into each title, so a title names them, once:
 *
 *   #include <rex/kernel/crt/native_memory.h>
 *   REX_NATIVE_MEMORY_HOOKS(sub_82BD95D0, sub_82BD7E30, sub_82BD8540)
 *
 * This suits the XDK routines: memcpy starts with `std r3,-8(r1)` and leaves r3
 * as the destination, memset builds its fill word from r4 with two rlwimi, and
 * memmove compares r3 and r4 first. The [rexcrt] table replaces a routine
 * outright, so it cannot fall back like this.
 *
 * The pattern is from Fable-2-Recomp 020607d (MIT, Copyright 2026 Himdo).
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>

namespace rex::kernel::crt {

// One flat host block: not MMIO, and below the physical memory at 0xE0000000,
// whose host offset differs by platform.
inline bool IsFlatGuestRange(uint32_t address, uint32_t size) {
  const uint64_t end = uint64_t(address) + size;
  if (end > 0xE0000000ull) {
    return false;
  }
  return address >= 0x80000000u || end <= 0x7F000000ull;
}

inline bool GuestRangesOverlap(uint32_t a, uint32_t b, uint32_t size) {
  return a < b ? uint64_t(a) + size > b : uint64_t(b) + size > a;
}

inline bool CanCopyNatively(uint32_t dst, uint32_t src, uint32_t size) {
  return size && IsFlatGuestRange(dst, size) && IsFlatGuestRange(src, size);
}

// The guest memcpy copies forward, so an overlapping one has a defined result.
inline bool CanMemcpyNatively(uint32_t dst, uint32_t src, uint32_t size) {
  return CanCopyNatively(dst, src, size) && !GuestRangesOverlap(dst, src, size);
}

// Bits above the low byte of r4 end up in the guest's fill word.
inline bool CanMemsetNatively(uint32_t dst, uint32_t value, uint32_t size) {
  return size && value <= 0xFF && IsFlatGuestRange(dst, size);
}

}  // namespace rex::kernel::crt

// The cvar is defined here, in the title, so the hot path does not cross a DLL.
#define REX_NATIVE_MEMORY_HOOKS(memcpy_sub, memset_sub, memmove_sub)                           \
  REXCVAR_DEFINE_BOOL(native_memory, true, "Performance",                                      \
                      "Run the game's memcpy, memset and memmove as host code");               \
  REX_EXTERN(__imp__##memcpy_sub);                                                             \
  REX_EXTERN(__imp__##memset_sub);                                                             \
  REX_EXTERN(__imp__##memmove_sub);                                                            \
  REX_HOOK_RAW(memcpy_sub) {                                                                   \
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, size = ctx.r5.u32;                      \
    if (REXCVAR_GET(native_memory) && rex::kernel::crt::CanMemcpyNatively(dst, src, size)) {   \
      std::memcpy(base + dst, base + src, size);                                               \
    } else {                                                                                   \
      __imp__##memcpy_sub(ctx, base);                                                          \
    }                                                                                          \
  }                                                                                            \
  REX_HOOK_RAW(memset_sub) {                                                                   \
    const uint32_t dst = ctx.r3.u32, value = ctx.r4.u32, size = ctx.r5.u32;                    \
    if (REXCVAR_GET(native_memory) && rex::kernel::crt::CanMemsetNatively(dst, value, size)) { \
      std::memset(base + dst, int(value), size);                                               \
    } else {                                                                                   \
      __imp__##memset_sub(ctx, base);                                                          \
    }                                                                                          \
  }                                                                                            \
  REX_HOOK_RAW(memmove_sub) {                                                                  \
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, size = ctx.r5.u32;                      \
    if (REXCVAR_GET(native_memory) && rex::kernel::crt::CanCopyNatively(dst, src, size)) {     \
      std::memmove(base + dst, base + src, size);                                              \
    } else {                                                                                   \
      __imp__##memmove_sub(ctx, base);                                                         \
    }                                                                                          \
  }
