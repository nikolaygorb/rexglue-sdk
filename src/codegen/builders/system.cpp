/**
 * @file        rexcodegen/builders/system.cpp
 * @brief       PPC system instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

namespace rex::codegen {

//=============================================================================
// No-ops and Sync Operations
//=============================================================================

bool build_nop(BuilderContext& ctx) {
  // Canonical PPC no-op (ori 0,0,0)
  (void)ctx;
  return true;
}

bool build_attn(BuilderContext& ctx) {
  // Xenon-specific debug breakpoint, no effect in recompiled code
  (void)ctx;
  return true;
}

bool build_sync(BuilderContext& ctx) {
  // sync: full barrier. Orders all combinations including StoreLoad,
  // which x86_64 TSO does not. seq_cst is the exact C++ mapping.
  //
  // x86_64: emits mfence.
  // ARM64:  emits dmb ish.
  ctx.println("\tstd::atomic_thread_fence(std::memory_order_seq_cst);");
  return true;
}

bool build_lwsync(BuilderContext& ctx) {
  // lwsync: acquire-release fence. Orders LoadLoad, LoadStore, StoreStore,
  // but NOT StoreLoad. acq_rel matches exactly.
  //
  // x86_64: TSO already orders those three.
  // ARM64:  emits dmb ish.
  ctx.println("\tstd::atomic_thread_fence(std::memory_order_acq_rel);");
  return true;
}

bool build_eieio(BuilderContext& ctx) {
  // eieio: I/O ordering. No separate I/O space on the host, so seq_cst
  // is the safe conservative mapping.
  //
  // x86_64: emits mfence.
  // ARM64:  emits dmb ish.
  ctx.println("\tstd::atomic_thread_fence(std::memory_order_seq_cst);");
  return true;
}

bool build_isync(BuilderContext& ctx) {
  // isync: context synchronization. There is no instruction context to
  // discard here; what remains is its use as an import barrier after a lock
  // acquire (lwarx/stwcx./bne/isync): later loads must not move above it.
  //
  // x86_64: no instruction (compiler barrier only).
  // ARM64:  emits dmb ishld.
  ctx.println("\tstd::atomic_thread_fence(std::memory_order_acquire);");
  return true;
}

bool build_db16cyc(BuilderContext& ctx) {
  // Xenon-specific delay hints (db8cyc, db10cyc, db12cyc, db16cyc), no effect
  // in recompiled code
  (void)ctx;
  return true;
}

bool build_cctpl(BuilderContext& ctx) {
  // Xenon-specific cache control thread priority low, no effect in recompiled code
  (void)ctx;
  return true;
}

bool build_cctpm(BuilderContext& ctx) {
  // Xenon-specific cache control thread priority medium, no effect in recompiled code
  (void)ctx;
  return true;
}

bool build_cctph(BuilderContext& ctx) {
  // Xenon-specific cache control thread priority high, no effect in recompiled code
  (void)ctx;
  return true;
}

//=============================================================================
// Trap Instructions
// PPC trap instructions are assertion/debug checks. The TO field (bits 21-25)
// is a 5-bit mask specifying which conditions trigger: signed lt/gt, eq,
// unsigned lt/gt. We extract TO directly from the instruction word so that
// both generic (tw TO,rA,rB) and simplified (tweq rA,rB) forms work with
// the same builder.
//=============================================================================

bool build_tdi(BuilderContext& ctx) {
  uint32_t to = (ctx.insn.instruction >> 21) & 0x1F;
  uint32_t ra = (ctx.insn.instruction >> 16) & 0x1F;
  int64_t simm = static_cast<int16_t>(ctx.insn.instruction & 0xFFFF);
  emitTrap(ctx, to, fmt::format("{}.s64", ctx.r(ra)), fmt::format("{}.u64", ctx.r(ra)),
           fmt::format("{}ll", simm), fmt::format("{}ull", static_cast<uint64_t>(simm)));
  return true;
}

bool build_twi(BuilderContext& ctx) {
  uint32_t to = (ctx.insn.instruction >> 21) & 0x1F;
  uint32_t ra = (ctx.insn.instruction >> 16) & 0x1F;
  int32_t simm = static_cast<int16_t>(ctx.insn.instruction & 0xFFFF);

  // twi 31, r0, <imm> is an unconditional trap with service code in the immediate
  if (to == 0x1F && ra == 0) {
    uint16_t trap_type = static_cast<uint16_t>(simm);
    ctx.println("\tppc_trap(ctx, base, {});", trap_type);
    return true;
  }

  emitTrap(ctx, to, fmt::format("{}.s32", ctx.r(ra)), fmt::format("{}.u32", ctx.r(ra)),
           fmt::format("{}", simm), fmt::format("{}u", static_cast<uint32_t>(simm)));
  return true;
}

bool build_td(BuilderContext& ctx) {
  uint32_t to = (ctx.insn.instruction >> 21) & 0x1F;
  uint32_t ra = (ctx.insn.instruction >> 16) & 0x1F;
  uint32_t rb = (ctx.insn.instruction >> 11) & 0x1F;
  emitTrap(ctx, to, fmt::format("{}.s64", ctx.r(ra)), fmt::format("{}.u64", ctx.r(ra)),
           fmt::format("{}.s64", ctx.r(rb)), fmt::format("{}.u64", ctx.r(rb)));
  return true;
}

bool build_tw(BuilderContext& ctx) {
  uint32_t to = (ctx.insn.instruction >> 21) & 0x1F;
  uint32_t ra = (ctx.insn.instruction >> 16) & 0x1F;
  uint32_t rb = (ctx.insn.instruction >> 11) & 0x1F;
  emitTrap(ctx, to, fmt::format("{}.s32", ctx.r(ra)), fmt::format("{}.u32", ctx.r(ra)),
           fmt::format("{}.s32", ctx.r(rb)), fmt::format("{}.u32", ctx.r(rb)));
  return true;
}

//=============================================================================
// Cache Operations
//=============================================================================

bool build_dcbf(BuilderContext& ctx) {
  // Hint instruction, access violation callback handlers take care of this on write
  (void)ctx;
  return true;
}

bool build_dcbt(BuilderContext& ctx) {
  // Hint instruction, prefetch has no semantic effect
  (void)ctx;
  return true;
}

bool build_dcbtst(BuilderContext& ctx) {
  // Hint instruction, prefetch-for-store has no semantic effect
  (void)ctx;
  return true;
}

bool build_dcbzl(BuilderContext& ctx) {
  // Compute EA, align to 128-byte cache line, apply physical offset
  ctx.print("\t{} = (", ctx.ea());
  if (ctx.insn.operands[0] != 0)
    ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[0]));
  ctx.println("{}.u32) & ~127;", ctx.r(ctx.insn.operands[1]));
  ctx.println("\tmemset((void*)REX_RAW_ADDR({}), 0, 128);", ctx.ea());
  return true;
}

bool build_dcbz(BuilderContext& ctx) {
  // Xenon has no short cache line: plain dcbz clears 128 bytes too.
  return build_dcbzl(ctx);
}

bool build_dcbst(BuilderContext& ctx) {
  // Hint instruction, access violation callback handlers take care of this on write
  (void)ctx;
  return true;
}

bool build_icbi(BuilderContext& ctx) {
  // Instruction cache block invalidate: guest code is recompiled ahead of
  // time, so there is no translated copy to invalidate
  (void)ctx;
  return true;
}

bool build_dst(BuilderContext& ctx) {
  // Data stream touch (dst, dstt, dstst, dststt): prefetch hint, no semantic effect
  (void)ctx;
  return true;
}

bool build_dss(BuilderContext& ctx) {
  // Data stream stop (dss, dssall): ends a dst prefetch, no semantic effect
  (void)ctx;
  return true;
}

//=============================================================================
// Move Register
//=============================================================================

bool build_mr(BuilderContext& ctx) {
  ctx.println("\t{}.u64 = {}.u64;", ctx.r(ctx.insn.operands[0]), ctx.r(ctx.insn.operands[1]));
  emitRecordFormCompare(ctx);

  // Propagates MMIO base flag from source to destination register
  if (ctx.locals.is_mmio_base(ctx.insn.operands[1]))
    ctx.locals.set_mmio_base(ctx.insn.operands[0]);
  else
    ctx.locals.clear_mmio_base(ctx.insn.operands[0]);

  return true;
}

//=============================================================================
// Move Register Field
//=============================================================================

bool build_mcrf(BuilderContext& ctx) {
  // Trivally copy one Control Register Field to another:
  ctx.println("\t{0} = {1};", ctx.cr(ctx.insn.operands[0]), ctx.cr(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Move From Special Registers
//=============================================================================

bool build_mfxer(BuilderContext& ctx) {
  ctx.println("\t{}.u64 = ({}.so << 31) | ({}.ov << 30) | ({}.ca << 29);",
              ctx.r(ctx.insn.operands[0]), ctx.xer(), ctx.xer(), ctx.xer());
  return true;
}

bool build_mfctr(BuilderContext& ctx) {
  ctx.println("\t{}.u64 = {}.u64;", ctx.r(ctx.insn.operands[0]), ctx.ctr());
  return true;
}

bool build_mfvrsave(BuilderContext& ctx) {
  ctx.println("\t{}.u64 = ctx.vrsave;", ctx.r(ctx.insn.operands[0]));
  return true;
}

bool build_mfcr(BuilderContext& ctx) {
  for (size_t i = 0; i < 32; i++) {
    constexpr std::string_view fields[] = {"lt", "gt", "eq", "so"};
    ctx.println("\t{}.u64 {}= {}.{} ? 0x{:X} : 0;", ctx.r(ctx.insn.operands[0]), i == 0 ? "" : "|",
                ctx.cr(i / 4), fields[i % 4], 1u << (31 - i));
  }
  return true;
}

bool build_mfocrf(BuilderContext& ctx) {
  // FXM is a one-hot mask: bit 7 = CR0, bit 6 = CR1, ..., bit 0 = CR7
  uint32_t fxm = ctx.insn.operands[1];
  uint32_t crField = 0;
  for (uint32_t i = 0; i < 8; i++) {
    if (fxm & (0x80u >> i)) {
      crField = i;
      break;
    }
  }
  uint32_t baseShift = 28 - 4 * crField;
  ctx.println("\t{}.u64 = ({}.lt << {}) | ({}.gt << {}) | ({}.eq << {}) | ({}.so << {});",
              ctx.r(ctx.insn.operands[0]), ctx.cr(crField), baseShift + 3, ctx.cr(crField),
              baseShift + 2, ctx.cr(crField), baseShift + 1, ctx.cr(crField), baseShift);
  return true;
}

bool build_mflr(BuilderContext& ctx) {
  if (!ctx.config().skipLr)
    ctx.println("\t{}.u64 = ctx.lr;", ctx.r(ctx.insn.operands[0]));
  return true;
}

bool build_mfmsr(BuilderContext& ctx) {
  if (!ctx.config().skipMsr) {
    // Check global lock and return appropriate value
    // Returns 0x8000 if unlocked (interrupts enabled), 0 if locked
    ctx.println("\t{}.u64 = REX_CHECK_GLOBAL_LOCK();", ctx.r(ctx.insn.operands[0]));
  }
  return true;
}

bool build_mffs(BuilderContext& ctx) {
  ctx.println("\t{}.u64 = ctx.fpscr.loadFromHost() | ctx.fpscr.loadExceptionsFromHost();",
              ctx.f(ctx.insn.operands[0]));
  return true;
}

bool build_mftb(BuilderContext& ctx) {
  // Xbox 360 timebase runs at 50 MHz (guest tick frequency)
  // Using REX_QUERY_TIMEBASE() macro provides properly scaled timing from the runtime
  ctx.println("\t{}.u64 = REX_QUERY_TIMEBASE();", ctx.r(ctx.insn.operands[0]));
  return true;
}

bool build_mftbu(BuilderContext& ctx) {
  // Upper 32 bits of timebase
  ctx.println("\t{}.u64 = REX_QUERY_TIMEBASE() >> 32;", ctx.r(ctx.insn.operands[0]));
  return true;
}

//=============================================================================
// Move To Special Registers
//=============================================================================

bool build_mtcr(BuilderContext& ctx) {
  for (size_t i = 0; i < 32; i++) {
    constexpr std::string_view fields[] = {"lt", "gt", "eq", "so"};
    ctx.println("\t{}.{} = ({}.u32 & 0x{:X}) != 0;", ctx.cr(i / 4), fields[i % 4],
                ctx.r(ctx.insn.operands[0]), 1u << (31 - i));
  }
  return true;
}

bool build_mtcrf(BuilderContext& ctx) {
  uint32_t fxm = ctx.insn.operands[0];
  constexpr std::string_view names[] = {"lt", "gt", "eq", "so"};
  for (uint32_t field = 0; field < 8; field++) {
    if (fxm & (0x80u >> field)) {
      uint32_t base_bit = 28 - 4 * field;
      for (int b = 0; b < 4; b++) {
        ctx.println("\t{}.{} = ({}.u32 & 0x{:X}) != 0;", ctx.cr(field), names[b],
                    ctx.r(ctx.insn.operands[1]), 1u << (base_bit + 3 - b));
      }
    }
  }
  return true;
}

bool build_mtctr(BuilderContext& ctx) {
  ctx.println("\t{}.u64 = {}.u64;", ctx.ctr(), ctx.r(ctx.insn.operands[0]));
  return true;
}

bool build_mtlr(BuilderContext& ctx) {
  if (!ctx.config().skipLr)
    ctx.println("\tctx.lr = {}.u64;", ctx.r(ctx.insn.operands[0]));
  return true;
}

bool build_mtmsrd(BuilderContext& ctx) {
  if (!ctx.config().skipMsr) {
    // Update MSR bits
    ctx.println("\tctx.msr = ({}.u32 & 0x8020) | (ctx.msr & ~0x8020);",
                ctx.r(ctx.insn.operands[0]));
    // Global lock mechanism:
    // R13 = enter lock (disable interrupts)
    // Other = leave lock (enable interrupts)
    uint32_t src_reg = ctx.insn.operands[0];
    if (src_reg == 13) {
      ctx.println("\tREX_ENTER_GLOBAL_LOCK();");
    } else {
      ctx.println("\tREX_LEAVE_GLOBAL_LOCK();");
    }
  }
  return true;
}

// FPSCR is modelled as its rounding mode (RN, the low two bits of field 7)
// and its exception bits (FX, VX, OX, UX, ZX, XX and the VX causes), which
// live in the host's sticky exception status flags (rex::ppc::FPSCRRegister).
// NI, FR, FI, FPRF and the enables are not modelled and read as zero. CR1 of a
// record form is set by DispatchInstruction (emitFpRecordCR1) for every
// floating-point instruction.

bool build_mtfsf(BuilderContext& ctx) {
  // FM bit 7 (the leftmost) selects field 0, the top nibble of FPSCR; bit 0
  // selects field 7, the low nibble with RN.
  uint32_t fm = ctx.insn.operands[0];
  uint32_t mask = 0;
  for (int j = 0; j < 8; j++) {
    if (fm & (1 << j))
      mask |= 0xF << (4 * j);
  }
  const auto f = ctx.f(ctx.insn.operands[1]);
  if (mask & 0xF) {
    ctx.println("\tctx.fpscr.storeFromGuest({}.u32);", f);
  }
  if (mask & ~0xFu) {
    ctx.println("\tctx.fpscr.storeExceptionsFromGuest({}.u32, 0x{:08X});", f, mask);
  }
  return true;
}

bool build_mtfsfi(BuilderContext& ctx) {
  // FPSCR[BF] <- U: field 7 holds RN, fields 0-3 and 5 exception bits.
  const uint32_t field = ctx.insn.operands[0];
  const uint32_t imm = ctx.insn.operands[1] & 0xF;
  if (field == 7) {
    ctx.println("\tctx.fpscr.storeFromGuest({});", imm);
  } else {
    const uint32_t shift = 4 * (7 - field);
    ctx.println("\tctx.fpscr.storeExceptionsFromGuest(0x{:08X}, 0x{:08X});", imm << shift,
                0xFu << shift);
  }
  return true;
}

static void emit_mtfsb(BuilderContext& ctx, bool set) {
  // FPSCR bit BT (0 = leftmost). Bits 30 and 31 are RN.
  const uint32_t bt = ctx.insn.operands[0];
  const uint32_t bit = 1u << (31 - bt);
  if (bt == 30 || bt == 31) {
    if (set) {
      ctx.println("\tctx.fpscr.storeFromGuest(ctx.fpscr.loadFromHost() | {});", bit);
    } else {
      ctx.println("\tctx.fpscr.storeFromGuest(ctx.fpscr.loadFromHost() & ~{}u);", bit);
    }
  } else {
    ctx.println("\tctx.fpscr.storeExceptionsFromGuest(0x{:08X}, 0x{:08X});", set ? bit : 0u,
                bit);
  }
}

bool build_mtfsb0(BuilderContext& ctx) {
  emit_mtfsb(ctx, false);
  return true;
}

bool build_mtfsb1(BuilderContext& ctx) {
  emit_mtfsb(ctx, true);
  return true;
}

bool build_mcrfs(BuilderContext& ctx) {
  // CR[BF] <- FPSCR[BFA], then the exception bits of that field are cleared
  // (FX and OX in field 0; UX, ZX, XX, VXSNAN in 1; VXISI, VXIDI, VXZDZ, VXIMZ
  // in 2; VXVC in 3; VXSOFT, VXSQRT, VXCVI in 5). Field 7 is (0, NI, RN).
  const auto cr = ctx.cr(ctx.insn.operands[0]);
  const uint32_t field = ctx.insn.operands[1];
  const uint32_t shift = 4 * (7 - field);
  ctx.println("\t{{");
  ctx.println(
      "\t\tconst uint32_t fpscr = ctx.fpscr.loadFromHost() | ctx.fpscr.loadExceptionsFromHost();");
  ctx.println(
      "\t\t{0}.lt = (fpscr >> {1}) & 1; {0}.gt = (fpscr >> {2}) & 1; {0}.eq = (fpscr >> {3}) & 1; "
      "{0}.so = (fpscr >> {4}) & 1;",
      cr, shift + 3, shift + 2, shift + 1, shift);
  if (field != 7) {
    // FX is derived from the others: clearing field 0 clears OX only.
    ctx.println("\t\tctx.fpscr.storeExceptionsFromGuest(0, 0x{:08X});", 0xFu << shift);
  }
  ctx.println("\t}}");
  return true;
}

bool build_mcrxr(BuilderContext& ctx) {
  // CR[BF] <- XER[SO, OV, CA, 0], then XER[SO, OV, CA] <- 0 (ReXGlue fork).
  const auto cr = ctx.cr(ctx.insn.operands[0]);
  ctx.println("\t{0}.lt = {1}.so; {0}.gt = {1}.ov; {0}.eq = {1}.ca; {0}.so = 0;", cr, ctx.xer());
  ctx.println("\t{0}.so = 0; {0}.ov = 0; {0}.ca = 0;", ctx.xer());
  return true;
}

bool build_mtxer(BuilderContext& ctx) {
  ctx.println("\t{}.so = ({}.u64 & 0x80000000) != 0;", ctx.xer(), ctx.r(ctx.insn.operands[0]));
  ctx.println("\t{}.ov = ({}.u64 & 0x40000000) != 0;", ctx.xer(), ctx.r(ctx.insn.operands[0]));
  ctx.println("\t{}.ca = ({}.u64 & 0x20000000) != 0;", ctx.xer(), ctx.r(ctx.insn.operands[0]));
  return true;
}

bool build_mtvrsave(BuilderContext& ctx) {
  ctx.println("\tctx.vrsave = {}.u32;", ctx.r(ctx.insn.operands[0]));
  return true;
}

//=============================================================================
// Clear Left Double Word Immediate
//=============================================================================

bool build_clrldi(BuilderContext& ctx) {
  ctx.println("\t{}.u64 = {}.u64 & 0x{:X};", ctx.r(ctx.insn.operands[0]),
              ctx.r(ctx.insn.operands[1]), (1ull << (64 - ctx.insn.operands[2])) - 1);
  emitRecordFormCompare(ctx);
  return true;
}

}  // namespace rex::codegen
