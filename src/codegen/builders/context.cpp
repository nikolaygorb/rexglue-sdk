/**
 * @file        rexcodegen/builders/context.cpp
 * @brief       PPC context instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

#include <algorithm>

#include <rex/codegen/binary_view.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include "../codegen_logging.h"
#include <rex/system/export_resolver.h>

namespace rex::codegen {

/// eieio instruction encoding (big-endian). Used for MMIO detection:
/// if the next instruction after a load/store is eieio, the access is MMIO.
static constexpr uint32_t kEieioEncoding = 0xAC06007C;

//=============================================================================
// Convenience Accessors
//=============================================================================

const RecompilerConfig& BuilderContext::config() const {
  return emitCtx.config;
}

const FunctionGraph& BuilderContext::graph() const {
  return emitCtx.graph;
}

//=============================================================================
// Register Accessors
//=============================================================================

// Localizing a non-volatile assumes the function owns it across its whole body.
// An SEH funclet does not: it inherits its owner's live registers through ctx.
// A function calling setjmp and the save / restore helpers keep theirs in ctx
// too (FunctionGraph::markContextNonVolatiles).
bool BuilderContext::localizeNonVolatiles() const {
  return config().nonVolatileRegistersAsLocalVariables && !fn.keepsNonVolatilesInContext();
}

bool BuilderContext::localizeNonVolatileGprs() const {
  const auto& cfg = config();
  return (cfg.nonVolatileRegistersAsLocalVariables || cfg.nonVolatileGprsAsLocalVariables) &&
         !fn.keepsNonVolatilesInContext();
}

// r0, r2, r11 and r12 carry values into a few callees outside the ABI: the
// frame pointer of an SEH funclet's owner (r12), the save area of a save /
// restore helper (r12, r11), the frame size of __chkstk (r12), a tail
// fragment's running value (r11). Those callees keep them in ctx like their
// non-volatiles, and a localizing caller syncs them across the call.
bool BuilderContext::localizeNonArgumentGprs() const {
  const auto& cfg = config();
  return (cfg.nonArgumentRegistersAsLocalVariables || cfg.nonArgumentGprsAsLocalVariables) &&
         !fn.keepsNonVolatilesInContext();
}

//=============================================================================
// Dead saves and restores of localized non-volatile GPRs
//=============================================================================
// In a function holding r14-r31 in locals, the prologue's stores of them hold
// the caller's values, which the function has no copy of (its locals start at
// zero) and no one reads back for the caller; the epilogue's loads of them
// feed nothing before the return. The guest accesses are volatile, so the
// host compiler keeps both. They are recognized from the instruction words:
// a store of rN (N >= 14) through r1 in the entry run made of mflr into a
// volatile GPR, stores through r1 and calls to the save helpers (the frame is
// not allocated yet, nothing has written a non-volatile); a load of rN through
// r1 followed only by loads through r1, mtlr from another register and
// addi r1,r1,d up to a blr or a branch to a restore helper.

namespace {

constexpr size_t kMaxSaveRestoreScan = 48;

uint32_t PrimaryOpcode(uint32_t word) {
  return word >> 26;
}
uint32_t FieldD(uint32_t word) {  // rD / rS
  return (word >> 21) & 31;
}
uint32_t FieldA(uint32_t word) {
  return (word >> 16) & 31;
}
bool IsStoreThroughR1(uint32_t word) {  // std, stw, stfd
  const uint32_t op = PrimaryOpcode(word);
  return FieldA(word) == 1 && ((op == 62 && (word & 3) == 0) || op == 36 || op == 54);
}
bool IsLoadThroughR1(uint32_t word) {  // ld, lwz, lfd
  const uint32_t op = PrimaryOpcode(word);
  return FieldA(word) == 1 && ((op == 58 && (word & 3) == 0) || op == 32 || op == 50);
}
bool IsMflr(uint32_t word) {
  return (word & 0xFC1FFFFF) == 0x7C0802A6;
}
bool IsMtlr(uint32_t word) {
  return (word & 0xFC1FFFFF) == 0x7C0803A6;
}
// b / bl with AA = 0 to a function whose name starts with prefix.
bool BranchesToHelper(const FunctionGraph& graph, uint32_t word, uint32_t address, bool link,
                      std::string_view prefix) {
  if (PrimaryOpcode(word) != 18 || (word & 3) != (link ? 1u : 0u))
    return false;
  int32_t offset = static_cast<int32_t>(word & 0x03FFFFFC);
  if (offset & 0x02000000)
    offset -= 0x04000000;
  const auto* target = graph.getFunction(address + static_cast<uint32_t>(offset));
  return target && std::string_view(target->name()).substr(0, prefix.size()) == prefix;
}

}  // namespace

bool BuilderContext::is_dead_nonvolatile_save() const {
  const uint32_t word = rex::memory::load_and_swap<uint32_t>(data);
  if (!localizeNonVolatileGprs() || FieldD(word) < 14 || PrimaryOpcode(word) == 54 ||
      !IsStoreThroughR1(word) || base < fn.base() || (base - fn.base()) / 4 > kMaxSaveRestoreScan)
    return false;
  const auto* entry = emitCtx.binary.translate(fn.base());
  if (!entry)
    return false;
  for (uint32_t address = fn.base(); address < base; address += 4) {
    if (config().midAsmHooks.count(address))
      return false;
    const uint32_t prior = rex::memory::load_and_swap<uint32_t>(entry + (address - fn.base()));
    const bool allowed = (IsMflr(prior) && FieldD(prior) < 14) || IsStoreThroughR1(prior) ||
                         BranchesToHelper(graph(), prior, address, true, "__savegprlr_") ||
                         BranchesToHelper(graph(), prior, address, true, "__savefpr_");
    if (!allowed)
      return false;
  }
  return !config().midAsmHooks.count(base);
}

bool BuilderContext::is_dead_nonvolatile_restore() const {
  const uint32_t word = rex::memory::load_and_swap<uint32_t>(data);
  const uint32_t reg = FieldD(word);
  if (!localizeNonVolatileGprs() || reg < 14 || PrimaryOpcode(word) == 50 || !IsLoadThroughR1(word))
    return false;
  for (size_t i = 0; i <= kMaxSaveRestoreScan; ++i) {
    const uint32_t address = base + static_cast<uint32_t>(i * 4);
    if (address >= fn.end() || config().midAsmHooks.count(address))
      return false;
    if (i == 0)
      continue;
    const auto* bytes = emitCtx.binary.translate(address);
    if (!bytes)
      return false;
    const uint32_t next = rex::memory::load_and_swap<uint32_t>(bytes);
    if (next == 0x4E800020 ||  // blr
        BranchesToHelper(graph(), next, address, false, "__restgprlr_"))
      return true;
    const bool allowed = IsLoadThroughR1(next) || (IsMtlr(next) && FieldD(next) != reg) ||
                         (PrimaryOpcode(next) == 14 && FieldD(next) == 1 && FieldA(next) == 1);
    if (!allowed)
      return false;
  }
  return false;
}

std::string BuilderContext::r(size_t index) {
  const auto& cfg = config();
  if ((localizeNonArgumentGprs() && (index == 0 || index == 2 || index == 11 || index == 12)) ||
      (localizeNonVolatileGprs() && index >= 14)) {
    locals.r[index] = true;
    return fmt::format("r{}", index);
  }
  return fmt::format("ctx.r{}", index);
}

std::string BuilderContext::f(size_t index) {
  const auto& cfg = config();
  if ((cfg.nonArgumentRegistersAsLocalVariables && index == 0) ||
      (localizeNonVolatiles() && index >= 14)) {
    locals.f[index] = true;
    return fmt::format("f{}", index);
  }
  return fmt::format("ctx.f{}", index);
}

std::string BuilderContext::v(size_t index) {
  const auto& cfg = config();
  if (locals.vmx_leaf || (cfg.nonArgumentRegistersAsLocalVariables && (index >= 32 && index <= 63)) ||
      (localizeNonVolatiles() && ((index >= 14 && index <= 31) || (index >= 64 && index <= 127)))) {
    locals.v[index] = true;
    return fmt::format("v{}", index);
  }
  return fmt::format("ctx.v{}", index);
}

std::string BuilderContext::cr(size_t index) {
  if (config().crRegistersAsLocalVariables) {
    locals.cr[index] = true;
    return fmt::format("cr{}", index);
  }
  return fmt::format("ctx.cr{}", index);
}

const char* BuilderContext::ctr() {
  if (config().ctrAsLocalVariable) {
    locals.ctr = true;
    return "ctr";
  }
  return "ctx.ctr";
}

const char* BuilderContext::xer() {
  if (config().xerAsLocalVariable) {
    locals.xer = true;
    return "xer";
  }
  return "ctx.xer";
}

const char* BuilderContext::reserved() {
  if (config().reservedRegisterAsLocalVariable) {
    locals.reserved = true;
    return "reserved";
  }
  return "ctx.reserved";
}

const char* BuilderContext::temp() {
  locals.temp = true;
  return "temp";
}

const char* BuilderContext::v_temp() {
  locals.v_temp = true;
  return "vTemp";
}

const char* BuilderContext::env() {
  locals.env = true;
  return "env";
}

const char* BuilderContext::ea() {
  locals.ea = true;
  return "ea";
}

//=============================================================================
// Output Helpers
//=============================================================================

// Template implementations in header, but we need explicit instantiations
// for common format strings to avoid link errors in some cases

//=============================================================================
// Code Generation Helpers
//=============================================================================

bool BuilderContext::mmio_check_d_form() {
  if (base + 4 < fn.end() && *(data + 1) == kEieioEncoding)
    return true;
  return locals.is_mmio_base(insn.operands[2]);
}

bool BuilderContext::mmio_check_x_form() {
  if (base + 4 < fn.end() && *(data + 1) == kEieioEncoding)
    return true;
  return locals.is_mmio_base(insn.operands[1]) || locals.is_mmio_base(insn.operands[2]);
}

const CallTarget* BuilderContext::findCallTarget(uint32_t site) const {
  // Search in calls (bl instructions)
  for (const auto& edge : fn.calls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  // Search in tail calls (b instructions to other functions)
  for (const auto& edge : fn.tailCalls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  return nullptr;
}

void BuilderContext::emit_function_call(uint32_t address) {
  const auto& cfg = config();

  if (cfg.isLongJmp(address)) {
    // Use custom ppc_longjmp that uses guest address as key (not for storage)
    println("\tppc_longjmp({}.u32, {}.s32);", r(3), r(4));
    return;
  }

  if (cfg.isSetJmp(address)) {
    // Save PPCContext for restoration after longjmp
    println("\t{} = ctx;", env());
    // Use custom ppc_setjmp that uses guest address as key
    println("\t{}.s64 = ppc_setjmp({}.u32);", temp(), r(3));
    // Restore PPCContext if returning from longjmp
    println("\tif ({}.s64 != 0) ctx = {};", temp(), env());
    println("\t{} = {};", r(3), temp());
    return;
  }

  // Try to use pre-resolved call target from FunctionGraph
  if (const auto* target = findCallTarget(base)) {
    if (target->isFunction()) {
      auto* targetFn = target->asFunction();
      const auto& name = targetFn->name();

      // Save/restore helpers. The helper bodies keep their registers in ctx
      // (FunctionGraph::markContextNonVolatiles), so they save and restore for
      // the functions that keep theirs there (funclets, setjmp callers); a
      // function holding the registers in locals has nothing in ctx to save.
      if (IsSaveRestoreHelperName(name)) {
        const bool gpr = name.find("gprlr_") != std::string::npos;
        if (gpr ? localizeNonVolatileGprs() : localizeNonVolatiles())
          return;  // print nothing - handled by local variable tracking
      }

      // An SEH funclet runs on its owner's frame and reads whatever non-volatiles
      // the owner left live, so hand it the localized copies through ctx and take
      // them back afterwards. Only registers already localized here can be live at
      // this point, so that set is the whole live-in the funclet can see.
      // What ctx held there belongs to a caller that keeps its own non-volatiles
      // in ctx (a setjmp caller, a funclet), so it is put back afterwards.
      // r0, r2, r11 and r12 go the same way into a funclet or a helper that is
      // still called; they are volatile, so the callee's values are what the
      // guest sees after the call and nothing is put back.
      constexpr size_t kNonArgument[] = {0, 2, 11, 12};
      const bool syncNonVolatiles = targetFn->sharesRegisters() && localizeNonVolatileGprs();
      const bool syncNonArguments =
          (targetFn->sharesRegisters() || IsSaveRestoreHelperName(name)) &&
          localizeNonArgumentGprs();
      if (syncNonVolatiles || syncNonArguments) {
        println("\t{{");
        for (size_t i = 14; syncNonVolatiles && i < 32; ++i) {
          if (locals.r[i])
            println("\tconst PPCRegister caller_r{0} = ctx.r{0}; ctx.r{0} = r{0};", i);
        }
        for (size_t i : kNonArgument) {
          if (syncNonArguments && locals.r[i])
            println("\tctx.r{0} = r{0};", i);
        }
        emitCtx.reference(name);
        println("\t{}(ctx, base);", name);
        for (size_t i : kNonArgument) {
          if (syncNonArguments && locals.r[i])
            println("\tr{0} = ctx.r{0};", i);
        }
        for (size_t i = 14; syncNonVolatiles && i < 32; ++i) {
          if (locals.r[i])
            println("\tr{0} = ctx.r{0}; ctx.r{0} = caller_r{0};", i);
        }
        println("\t}}");
        return;
      }

      emitCtx.reference(name);
      println("\t{}(ctx, base);", name);
      return;
    }

    if (target->isImport()) {
      const auto& importTarget = std::get<CallTarget::ToImport>(target->value);
      std::string func_name;

      // Try to resolve ordinal to actual function name
      auto at_pos = importTarget.name.find('@');
      if (at_pos != std::string::npos && emitCtx.resolver) {
        auto lib_name = importTarget.name.substr(0, at_pos);
        auto ordinal_str = importTarget.name.substr(at_pos + 1);
        uint16_t ordinal = static_cast<uint16_t>(std::stoul(ordinal_str));

        auto* exp = emitCtx.resolver->GetExportByOrdinal(lib_name + ".xex", ordinal);
        if (!exp)
          exp = emitCtx.resolver->GetExportByOrdinal(lib_name, ordinal);

        if (exp) {
          func_name = "__imp__" + std::string(exp->name);
        }
      }

      if (func_name.empty()) {
        func_name = "__imp__" + importTarget.name;
        std::replace(func_name.begin(), func_name.end(), '@', '_');
        std::replace(func_name.begin(), func_name.end(), '.', '_');
      }

      emitCtx.reference(func_name);
      println("\t{}(ctx, base);", func_name);
      return;
    }

    // Unresolved target from graph
    REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X}", address, base);
    println("\t// FATAL: unresolved function 0x{:08X}", address);
    println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
    return;
  }

  // Same fallback as emit_conditional_branch: the per-site CallTarget table can
  // be missing an entry for a target the graph knows perfectly well. Ask the
  // graph before giving up, so a resolvable call does not become a runtime abort.
  if (auto* fn = graph().getFunction(address)) {
    emitCtx.reference(fn->name());
    println("\t{}(ctx, base);", fn->name());
    return;
  }

  // No pre-resolved target found - this is an error
  REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X} (no CallTarget in FunctionNode)",
                   address, base);
  println("\t// FATAL: unresolved function 0x{:08X} (no CallTarget in FunctionNode)", address);
  println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
}

void BuilderContext::emit_conditional_branch(bool not_, std::string_view cond) {
  uint32_t target = insn.operands[1];

  // Use classifyTarget for consistent branch classification
  // false = branch instruction (not a call), so own-base means loop back
  auto kind = graph().classifyTarget(target, base, false, &fn);

  switch (kind) {
    case TargetKind::InternalLabel:
      // Target is within this function - local goto
      println("\tif ({}{}.{}) goto loc_{:08X};", not_ ? "!" : "", cr(insn.operands[0]), cond,
              target);
      break;

    case TargetKind::Function:
    case TargetKind::Import:
      // Conditional tail call to another function: the same call as an unconditional `b`
      // (emit_function_call), so a share_registers target gets the localized non-volatiles
      // through ctx, save/restore helpers stay elided under non_volatile_as_local and imports
      // resolve by ordinal. A target missing from the per-site CallTarget table resolves
      // through the graph there too (on Gears of War Judgment, 28 conditional back-edges such
      // as `blt cr6,0x8270d2a8` at 0x8270D36C would otherwise be a guaranteed runtime abort).
      if (findCallTarget(base) || graph().getFunction(target)) {
        println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
        emit_function_call(target);
        println("\t\treturn;");
        println("\t}}");
      } else {
        REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X} (no CallTarget)",
                         target, base);
        println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
                not_ ? "!" : "", cr(insn.operands[0]), cond, base, target);
      }
      break;

    case TargetKind::Unknown:
      REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X}", target, base);
      println("\t// ERROR: conditional branch to unknown address 0x{:08X}", target);
      println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
              not_ ? "!" : "", cr(insn.operands[0]), cond, base, target);
      break;
  }
}

void BuilderContext::emit_set_flush_mode(bool enable) {
  // fpu_flush_denormals: the FPU shares the VMX mode, so code only switches
  // into it (once per function / label, a check that is usually a no-op).
  if (config().fpuFlushDenormals)
    enable = true;
  auto newState = enable ? CSRState::VMX : CSRState::FPU;
  if (csrState != newState) {
    auto prefix = enable ? "enable" : "disable";
    auto suffix = csrState != CSRState::Unknown ? "Unconditional" : "";
    println("\tctx.fpscr.{}FlushMode{}();", prefix, suffix);

    csrState = newState;
  }
}

bool BuilderContext::has_mid_asm_hook() const {
  return config().midAsmHooks.find(base) != config().midAsmHooks.end();
}

void BuilderContext::emit_mid_asm_hook() {
  auto midAsmHook = config().midAsmHooks.find(base);
  if (midAsmHook == config().midAsmHooks.end()) {
    return;
  }

  bool returnsBool = midAsmHook->second.returnOnFalse || midAsmHook->second.returnOnTrue ||
                     midAsmHook->second.jumpAddressOnFalse != 0 ||
                     midAsmHook->second.jumpAddressOnTrue != 0;

  print("\t");
  if (returnsBool)
    print("if (");

  // Build call -- no ctx/base prefix, just register arguments resolved through accessors
  print("{}(", midAsmHook->second.name);
  for (auto& reg : midAsmHook->second.registers) {
    if (out.back() != '(')
      out += ", ";

    switch (reg[0]) {
      case 'c':
        if (reg == "ctr")
          out += ctr();
        else
          out += cr(std::atoi(reg.c_str() + 2));
        break;
      case 'x':
        out += xer();
        break;
      case 'r':
        if (reg == "reserved")
          out += reserved();
        else
          out += r(std::atoi(reg.c_str() + 1));
        break;
      case 'f':
        if (reg == "fpscr")
          out += "ctx.fpscr";
        else
          out += f(std::atoi(reg.c_str() + 1));
        break;
      case 'v':
        out += v(std::atoi(reg.c_str() + 1));
        break;
    }
  }

  if (returnsBool) {
    println(")) {{");

    if (midAsmHook->second.returnOnTrue)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnTrue != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnTrue);

    println("\t}}");

    println("\telse {{");

    if (midAsmHook->second.returnOnFalse)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnFalse != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnFalse);

    println("\t}}");
  } else {
    println(");");

    if (midAsmHook->second.ret)
      println("\treturn;");
    else if (midAsmHook->second.jumpAddress != 0)
      println("\tgoto loc_{:X};", midAsmHook->second.jumpAddress);
  }
}

//=============================================================================
// Vector (SIMD) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_vec_fp_binary(const char* simd_op) {
  println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_{}_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)));",
      v(insn.operands[0]), simd_op, v(insn.operands[1]), v(insn.operands[2]));
}

void BuilderContext::emit_vec_fp_unary_expr(std::string_view simd_expr) {
  auto vD = v(insn.operands[0]);
  auto vA = v(insn.operands[1]);

  // Replace {vA} placeholder in expression with actual register
  std::string expr(simd_expr);
  size_t pos;
  while ((pos = expr.find("{vA}")) != std::string::npos) {
    expr.replace(pos, 4, vA);
  }

  println("\tsimde_mm_store_ps({}.f32, {});", vD, expr);
}

void BuilderContext::emit_vec_int_binary(const char* simd_op, const char* element_type) {
  println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      v(insn.operands[0]), element_type, simd_op, v(insn.operands[1]), element_type,
      v(insn.operands[2]), element_type);
}

void BuilderContext::emit_vec_int_binary_swapped(const char* simd_op, const char* element_type) {
  // Swapped: op(vB, vA) instead of op(vA, vB) - useful for andnot which has reversed operand
  // semantics
  println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      v(insn.operands[0]), element_type, simd_op, v(insn.operands[2]), element_type,
      v(insn.operands[1]), element_type);
}

void BuilderContext::emit_vec_var_shift(const char* shift_dir, const char* element_type,
                                        uint32_t mask_value) {
  auto vD = v(insn.operands[0]);
  auto vA = v(insn.operands[1]);
  auto vB = v(insn.operands[2]);
  println("\t{{");
  println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", vA);
  println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", vB);
  println("\t\tsimde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_{}(0x{:X}));", element_type,
          mask_value);
  println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_{}_{}"
      "(a, shift));",
      vD, shift_dir, element_type);
  println("\t}}");
}

//=============================================================================
// Memory (Load/Store) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_load_d_form(const char* load_macro, const char* dest_type,
                                      bool check_mmio) {
  // D-form: rD = LOAD(rA + D) where operands[0]=rD, operands[1]=D, operands[2]=rA
  // load_macro should be like "REX_LOAD_U8" - we replace REX_LOAD with REX_MM_LOAD for MMIO
  if (is_dead_nonvolatile_restore())
    return;
  const char* macro = load_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_d_form()) {
    // Replace "REX_LOAD_" with "REX_MM_LOAD_"
    if (strncmp(load_macro, "REX_LOAD_", 9) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_LOAD_%s", load_macro + 9);
      macro = mm_macro;
    }
  }

  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{});", static_cast<int32_t>(insn.operands[1]));
}

void BuilderContext::emit_load_x_form(const char* load_macro, const char* dest_type,
                                      bool check_mmio) {
  // X-form: rD = LOAD(rA + rB) where operands[0]=rD, operands[1]=rA, operands[2]=rB
  // load_macro should be like "REX_LOAD_U8" - we replace REX_LOAD with REX_MM_LOAD for MMIO
  const char* macro = load_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_x_form()) {
    // Replace "REX_LOAD_" with "REX_MM_LOAD_"
    if (strncmp(load_macro, "REX_LOAD_", 9) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_LOAD_%s", load_macro + 9);
      macro = mm_macro;
    }
  }

  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32);", r(insn.operands[2]));
}

void BuilderContext::emit_store_d_form(const char* store_macro, const char* src_type,
                                       bool check_mmio) {
  // D-form: STORE(rA + D, rS) where operands[0]=rS, operands[1]=D, operands[2]=rA
  // store_macro should be like "REX_STORE_U8" - we replace REX_STORE with REX_MM_STORE for MMIO
  if (is_dead_nonvolatile_save())
    return;
  const char* macro = store_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_d_form()) {
    // Replace "REX_STORE_" with "REX_MM_STORE_"
    if (strncmp(store_macro, "REX_STORE_", 10) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_STORE_%s", store_macro + 10);
      macro = mm_macro;
    }
  }

  print("\t{}(", macro);
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{}, {}.{});", static_cast<int32_t>(insn.operands[1]), r(insn.operands[0]), src_type);
}

void BuilderContext::emit_store_x_form(const char* store_macro, const char* src_type,
                                       bool check_mmio) {
  // X-form: STORE(rA + rB, rS) where operands[0]=rS, operands[1]=rA, operands[2]=rB
  // store_macro should be like "REX_STORE_U8" - we replace REX_STORE with REX_MM_STORE for MMIO
  const char* macro = store_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_x_form()) {  // Use X-form specific check (operands[1] is base)
    // Replace "REX_STORE_" with "REX_MM_STORE_"
    if (strncmp(store_macro, "REX_STORE_", 10) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_STORE_%s", store_macro + 10);
      macro = mm_macro;
    }
  }

  print("\t{}(", macro);
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32, {}.{});", r(insn.operands[2]), r(insn.operands[0]), src_type);
}

}  // namespace rex::codegen
