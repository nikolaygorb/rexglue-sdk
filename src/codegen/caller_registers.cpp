/**
 * @file        codegen/caller_registers.cpp
 * @brief       Find the functions that read a register their caller set
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// With non_argument_gpr_as_local (r0, r2, r11, r12) or non_volatile_gpr_as_local
// (r14-r31) every function holds those registers in C++ locals that start at
// zero. A few functions take one from their caller outside the ABI: an SEH
// funclet the frame of its owner in r12, __chkstk the frame size in r12, a
// tail fragment the running value in r11, a function entered in the middle of
// a body whatever the code before it left. Such a function needs
// share_registers (it works on ctx, and a localizing caller copies its locals
// into ctx around the call).
//
// The analysis follows the code the way FunctionNode::emitCpp emits it (blocks
// in order, the same branch classification, jump tables and mid-asm hooks) and
// asks, per function, which tracked registers some instruction reads where no
// path from the entry may have set them yet ("may", not "must": a register
// set in one branch and read after the join is taken as set, as the code
// never takes the other path that way). A value set later in a loop does not
// count for the loop's head: the first pass comes from before the loop. A
// callee's such registers count at its call site, so a caller that passes one
// on without setting it is flagged as well. memory/tools/nonvolatile_liveins.py
// runs the same check on the generated C++.
//
// Getting a write wrong costs only speed: an instruction not listed below
// reads all its GPR operands and writes none, which can flag a function that
// did not need it, never miss one.

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include "codegen_logging.h"
#include "ppc/disasm.h"

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

namespace {

using rex::memory::load_and_swap;

using Mask = uint32_t;  // one bit per GPR

constexpr Mask Bit(uint32_t reg) {
  return Mask{1} << reg;
}

constexpr Mask kNonArgument = Bit(0) | Bit(2) | Bit(11) | Bit(12);
constexpr Mask kNonVolatile = 0xFFFFC000u;  // r14-r31

// Which GPR operands an instruction writes. Everything else it names is read.
enum class Dest : uint8_t {
  kNone,
  kOp0,            // operand 0 only written
  kOp0ReadWrite,   // operand 0 read and written (insert)
  kOp0Update2,     // load with update: operand 0 written, the base (2) too
  kOp0Update1,     // indexed load with update: base at 1
  kUpdate2,        // store / FP load with update: base (2) written
  kUpdate1,        // indexed form: base at 1
  kLoadMultiple,   // lmw: operand 0 to r31
  kStoreMultiple,  // stmw: reads operand 0 to r31
  kLoadString,     // lswi: operand 0 on, one register per 4 bytes of NB (2)
  kStoreString,    // stswi: reads the same
  kStoreStringX,   // stswx: reads any (the count is in XER)
  kLoadQuad,       // lq: operand 0 and the next one
};

Dest DestOf(int id) {
  switch (id) {
    case PPC_INST_ADD:
    case PPC_INST_ADDE:
    case PPC_INST_ADDI:
    case PPC_INST_ADDIC:
    case PPC_INST_ADDIS:
    case PPC_INST_ADDZE:
    case PPC_INST_ADDME:
    case PPC_INST_ADDC:
    case PPC_INST_ADDO:
    case PPC_INST_ADDCO:
    case PPC_INST_ADDEO:
    case PPC_INST_ADDMEO:
    case PPC_INST_ADDZEO:
    case PPC_INST_DIVD:
    case PPC_INST_DIVDU:
    case PPC_INST_DIVW:
    case PPC_INST_DIVWU:
    case PPC_INST_DIVWO:
    case PPC_INST_DIVWUO:
    case PPC_INST_DIVDO:
    case PPC_INST_DIVDUO:
    case PPC_INST_MULHW:
    case PPC_INST_MULHWU:
    case PPC_INST_MULHD:
    case PPC_INST_MULHDU:
    case PPC_INST_MULLD:
    case PPC_INST_MULLI:
    case PPC_INST_MULLW:
    case PPC_INST_MULLWO:
    case PPC_INST_MULLDO:
    case PPC_INST_NEG:
    case PPC_INST_NEGO:
    case PPC_INST_SUBF:
    case PPC_INST_SUBFC:
    case PPC_INST_SUBFE:
    case PPC_INST_SUBFIC:
    case PPC_INST_SUBFZE:
    case PPC_INST_SUBFME:
    case PPC_INST_SUBFO:
    case PPC_INST_SUBFCO:
    case PPC_INST_SUBFEO:
    case PPC_INST_SUBFMEO:
    case PPC_INST_SUBFZEO:
    case PPC_INST_AND:
    case PPC_INST_ANDC:
    case PPC_INST_ANDI:
    case PPC_INST_ANDIS:
    case PPC_INST_NAND:
    case PPC_INST_NOR:
    case PPC_INST_NOT:
    case PPC_INST_OR:
    case PPC_INST_ORC:
    case PPC_INST_ORI:
    case PPC_INST_ORIS:
    case PPC_INST_XOR:
    case PPC_INST_XORI:
    case PPC_INST_XORIS:
    case PPC_INST_EQV:
    case PPC_INST_CNTLZD:
    case PPC_INST_CNTLZW:
    case PPC_INST_EXTSB:
    case PPC_INST_EXTSH:
    case PPC_INST_EXTSW:
    case PPC_INST_CLRLWI:
    case PPC_INST_CLRLDI:
    case PPC_INST_RLDCL:
    case PPC_INST_ROTLD:
    case PPC_INST_RLDCR:
    case PPC_INST_RLDIC:
    case PPC_INST_RLDICL:
    case PPC_INST_RLDICR:
    case PPC_INST_ROTLDI:
    case PPC_INST_RLWINM:
    case PPC_INST_RLWNM:
    case PPC_INST_ROTLW:
    case PPC_INST_ROTLWI:
    case PPC_INST_SLD:
    case PPC_INST_SLW:
    case PPC_INST_SRAD:
    case PPC_INST_SRADI:
    case PPC_INST_SRAW:
    case PPC_INST_SRAWI:
    case PPC_INST_SRD:
    case PPC_INST_SRW:
    case PPC_INST_LI:
    case PPC_INST_LIS:
    case PPC_INST_MR:
    case PPC_INST_LBZ:
    case PPC_INST_LBZX:
    case PPC_INST_LHA:
    case PPC_INST_LHAX:
    case PPC_INST_LHBRX:
    case PPC_INST_LHZ:
    case PPC_INST_LHZX:
    case PPC_INST_LWA:
    case PPC_INST_LWAX:
    case PPC_INST_LWZ:
    case PPC_INST_LWZX:
    case PPC_INST_LWBRX:
    case PPC_INST_LD:
    case PPC_INST_LDX:
    case PPC_INST_LDBRX:
    case PPC_INST_LWARX:
    case PPC_INST_LDARX:
    case PPC_INST_MFXER:
    case PPC_INST_MFCTR:
    case PPC_INST_MFCR:
    case PPC_INST_MFOCRF:
    case PPC_INST_MFLR:
    case PPC_INST_MFMSR:
    case PPC_INST_MFPVR:
    case PPC_INST_MFTB:
    case PPC_INST_MFTBU:
    case PPC_INST_MFVRSAVE:
      return Dest::kOp0;
    case PPC_INST_RLWIMI:
    case PPC_INST_RLDIMI:
      return Dest::kOp0ReadWrite;
    case PPC_INST_LBZU:
    case PPC_INST_LHAU:
    case PPC_INST_LHZU:
    case PPC_INST_LWZU:
    case PPC_INST_LDU:
      return Dest::kOp0Update2;
    case PPC_INST_LBZUX:
    case PPC_INST_LHAUX:
    case PPC_INST_LHZUX:
    case PPC_INST_LWAUX:
    case PPC_INST_LWZUX:
    case PPC_INST_LDUX:
      return Dest::kOp0Update1;
    case PPC_INST_STBU:
    case PPC_INST_STHU:
    case PPC_INST_STWU:
    case PPC_INST_STDU:
    case PPC_INST_LFDU:
    case PPC_INST_LFSU:
    case PPC_INST_STFDU:
    case PPC_INST_STFSU:
      return Dest::kUpdate2;
    case PPC_INST_STBUX:
    case PPC_INST_STHUX:
    case PPC_INST_STWUX:
    case PPC_INST_STDUX:
    case PPC_INST_LFDUX:
    case PPC_INST_LFSUX:
    case PPC_INST_STFDUX:
    case PPC_INST_STFSUX:
      return Dest::kUpdate1;
    case PPC_INST_LMW:
      return Dest::kLoadMultiple;
    case PPC_INST_STMW:
      return Dest::kStoreMultiple;
    case PPC_INST_LSWI:
      return Dest::kLoadString;
    case PPC_INST_STSWI:
      return Dest::kStoreString;
    case PPC_INST_STSWX:
      return Dest::kStoreStringX;
    case PPC_INST_LQ:
      return Dest::kLoadQuad;
    default:
      return Dest::kNone;
  }
}

// GPR stores whose base is the stack spill the non-volatiles (prologues):
// those reads are saves, not uses.
bool IsGprStore(int id, bool& indexed) {
  switch (id) {
    case PPC_INST_STB:
    case PPC_INST_STBU:
    case PPC_INST_STH:
    case PPC_INST_STHU:
    case PPC_INST_STW:
    case PPC_INST_STWU:
    case PPC_INST_STD:
    case PPC_INST_STDU:
    case PPC_INST_STMW:
      indexed = false;
      return true;
    case PPC_INST_STBX:
    case PPC_INST_STBUX:
    case PPC_INST_STHX:
    case PPC_INST_STHUX:
    case PPC_INST_STHBRX:
    case PPC_INST_STWX:
    case PPC_INST_STWUX:
    case PPC_INST_STWBRX:
    case PPC_INST_STDX:
    case PPC_INST_STDUX:
    case PPC_INST_STDBRX:
    case PPC_INST_STWCX:
    case PPC_INST_STDCX:
      indexed = true;
      return true;
    default:
      return false;
  }
}

// The operand that is an address base, where r0 stands for 0 (as for RA0),
// though the binutils table names it a plain RA: the AltiVec / VMX128 loads
// and stores and the cache instructions. -1: none.
int ZeroBaseOperand(int id) {
  switch (id) {
    case PPC_INST_LVX:
    case PPC_INST_LVX128:
    case PPC_INST_LVXL:
    case PPC_INST_LVXL128:
    case PPC_INST_LVLX:
    case PPC_INST_LVLX128:
    case PPC_INST_LVLXL:
    case PPC_INST_LVLXL128:
    case PPC_INST_LVRX:
    case PPC_INST_LVRX128:
    case PPC_INST_LVRXL:
    case PPC_INST_LVRXL128:
    case PPC_INST_LVSL:
    case PPC_INST_LVSL128:
    case PPC_INST_LVSR:
    case PPC_INST_LVSR128:
    case PPC_INST_LVEBX:
    case PPC_INST_LVEHX:
    case PPC_INST_LVEWX:
    case PPC_INST_LVEWX128:
    case PPC_INST_STVEBX:
    case PPC_INST_STVEHX:
    case PPC_INST_STVEWX:
    case PPC_INST_STVEWX128:
    case PPC_INST_STVLX:
    case PPC_INST_STVLX128:
    case PPC_INST_STVLXL:
    case PPC_INST_STVLXL128:
    case PPC_INST_STVRX:
    case PPC_INST_STVRX128:
    case PPC_INST_STVRXL:
    case PPC_INST_STVRXL128:
    case PPC_INST_STVX:
    case PPC_INST_STVX128:
    case PPC_INST_STVXL:
    case PPC_INST_STVXL128:
    case PPC_INST_DCBT:
    case PPC_INST_DCBTST:
      return 1;
    case PPC_INST_DCBF:
    case PPC_INST_DCBFL:
    case PPC_INST_DCBZ:
    case PPC_INST_DCBZL:
    case PPC_INST_DCBST:
    case PPC_INST_ICBI:
    case PPC_INST_DST:
    case PPC_INST_DSTT:
    case PPC_INST_DSTST:
    case PPC_INST_DSTSTT:
      return 0;
    case PPC_INST_STFQ:
      return 2;
    default:
      return -1;
  }
}

struct UseDef {
  Mask reads = 0;
  Mask writes = 0;
};

UseDef GprUseDef(const ppc_insn& insn) {
  UseDef ud;
  const int id = insn.opcode->id;
  const Dest dest = DestOf(id);
  const int zeroBase = ZeroBaseOperand(id);
  auto gpr = [&](int index, uint32_t& reg) {
    int kind = ppc_operand_gpr_kind(insn.opcode, index);
    if (kind == 1 && index == zeroBase)
      kind = 2;
    reg = insn.operands[index];
    return kind != 0 && reg < 32 && !(kind == 2 && reg == 0);
  };

  const bool op0WriteOnly = dest == Dest::kOp0 || dest == Dest::kOp0Update2 ||
                            dest == Dest::kOp0Update1 || dest == Dest::kLoadMultiple ||
                            dest == Dest::kLoadQuad || dest == Dest::kLoadString;
  for (int i = 0; i < 8; ++i) {
    uint32_t reg;
    if (gpr(i, reg) && !(i == 0 && op0WriteOnly))
      ud.reads |= Bit(reg);
  }

  uint32_t reg;
  switch (dest) {
    case Dest::kNone:
      break;
    case Dest::kOp0:
    case Dest::kOp0ReadWrite:
      if (gpr(0, reg))
        ud.writes |= Bit(reg);
      break;
    case Dest::kOp0Update2:
    case Dest::kOp0Update1:
      if (gpr(0, reg))
        ud.writes |= Bit(reg);
      if (gpr(dest == Dest::kOp0Update2 ? 2 : 1, reg))
        ud.writes |= Bit(reg);
      break;
    case Dest::kUpdate2:
    case Dest::kUpdate1:
      if (gpr(dest == Dest::kUpdate2 ? 2 : 1, reg))
        ud.writes |= Bit(reg);
      break;
    case Dest::kLoadMultiple:
      if (gpr(0, reg))
        ud.writes |= ~(Bit(reg) - 1);
      break;
    case Dest::kStoreMultiple:
      if (gpr(0, reg))
        ud.reads |= ~(Bit(reg) - 1);
      break;
    case Dest::kLoadQuad:
      if (gpr(0, reg))
        ud.writes |= Bit(reg) | (reg < 31 ? Bit(reg + 1) : 0);
      break;
    case Dest::kLoadString:
    case Dest::kStoreString:
      if (gpr(0, reg)) {
        const uint32_t bytes = insn.operands[2] ? insn.operands[2] : 32;
        Mask string_regs = 0;
        for (uint32_t k = 0; 4 * k < bytes; ++k) {
          string_regs |= Bit((reg + k) % 32);
        }
        (dest == Dest::kLoadString ? ud.writes : ud.reads) |= string_regs;
      }
      break;
    case Dest::kStoreStringX:
      ud.reads = ~Mask{0};
      break;
  }

  // subf / subfc / subfe / xor / andc rD,rA,rA (0) and eqv / orc rD,rA,rA
  // (-1): rA's value cancels out. Compilers clear a register this way
  // (upstream rexglue PR #609; its vector forms don't apply, only GPRs are
  // tracked here).
  switch (id) {
    case PPC_INST_SUBF:
    case PPC_INST_SUBFO:
    case PPC_INST_SUBFC:
    case PPC_INST_SUBFCO:
    case PPC_INST_SUBFE:
    case PPC_INST_SUBFEO:
    case PPC_INST_XOR:
    case PPC_INST_ANDC:
    case PPC_INST_EQV:
    case PPC_INST_ORC:
      if (insn.operands[1] == insn.operands[2] && insn.operands[1] < 32)
        ud.reads &= ~Bit(insn.operands[1]);
      break;
    default:
      break;
  }

  // tw / twi / td / tdi with TO 31 trap whatever the operands hold
  // (ppc_trap; twi 31,r0,N is the assert / __debugbreak of every title)
  // and with TO 0 never: emitTrap reads neither.
  const uint32_t word = insn.instruction;
  const uint32_t primary = word >> 26, xo = (word >> 1) & 0x3FF, to = (word >> 21) & 0x1F;
  if ((primary == 2 || primary == 3 || (primary == 31 && (xo == 4 || xo == 68))) &&
      (to == 0 || to == 0x1F))
    ud.reads = 0;
  return ud;
}

// One statement of the emitted function: an instruction or a mid-asm hook.
struct Step {
  Mask reads = 0;
  Mask writes = 0;
  bool fallthrough = true;
};

struct Summary {
  Mask direct = 0;  // read where nothing may have set them
  // Per callee: the registers that may be set before every call to it.
  std::vector<std::pair<const FunctionNode*, Mask>> calls;
};

class Walker {
 public:
  Walker(const FunctionGraph& graph, const BinaryView& binary, const RecompilerConfig& config,
         Mask tracked)
      : graph_(graph), binary_(binary), config_(config), tracked_(tracked) {}

  Summary summarize(const FunctionNode& fn) {
    steps_.clear();
    edges_.clear();
    callSites_.clear();
    labels_.clear();
    build(fn);
    return flow();
  }

 private:
  void addHook(uint32_t address, const MidAsmHook& hook) {
    Step step;
    for (const auto& reg : hook.registers) {
      // The hook gets the register by reference and its own code sets it
      // (RDR's ammo hooks hand r0 back): a write, not a read.
      if (reg.size() > 1 && reg[0] == 'r' && reg != "reserved") {
        const int n = std::atoi(reg.c_str() + 1);
        if (n >= 0 && n < 32)
          step.writes |= Bit(n);
      }
    }
    const bool returnsBool = hook.returnOnFalse || hook.returnOnTrue ||
                             hook.jumpAddressOnFalse != 0 || hook.jumpAddressOnTrue != 0;
    const size_t index = steps_.size();
    if (returnsBool) {
      const bool trueLeaves = hook.returnOnTrue || hook.jumpAddressOnTrue != 0;
      const bool falseLeaves = hook.returnOnFalse || hook.jumpAddressOnFalse != 0;
      if (!hook.returnOnTrue && hook.jumpAddressOnTrue != 0)
        edges_.emplace_back(index, hook.jumpAddressOnTrue);
      if (!hook.returnOnFalse && hook.jumpAddressOnFalse != 0)
        edges_.emplace_back(index, hook.jumpAddressOnFalse);
      step.fallthrough = !(trueLeaves && falseLeaves);
    } else if (hook.ret) {
      step.fallthrough = false;
    } else if (hook.jumpAddress != 0) {
      edges_.emplace_back(index, hook.jumpAddress);
      step.fallthrough = false;
    }
    labels_.try_emplace(address, index);
    steps_.push_back(step);
  }

  void call(size_t index, uint32_t target) {
    if (const auto* callee = graph_.getFunction(target))
      callSites_.emplace_back(index, callee);
  }

  void branch(const FunctionNode& fn, size_t index, Step& step, uint32_t address, uint32_t word) {
    const uint32_t op = word >> 26;
    const bool link = (word & 1) != 0;
    if (op == 18) {  // b, bl
      const int32_t li = static_cast<int32_t>((word & 0x03FFFFFC) ^ 0x02000000) - 0x02000000;
      const uint32_t target = (word & 2) ? static_cast<uint32_t>(li) : address + li;
      const auto kind = graph_.classifyTarget(target, address, link, &fn);
      if (link) {
        if (kind == TargetKind::InternalLabel) {
          edges_.emplace_back(index, target);
          step.fallthrough = false;
        } else if (kind == TargetKind::Unknown) {
          step.fallthrough = false;  // REX_FATAL
        } else {
          call(index, target);
        }
        return;
      }
      step.fallthrough = false;
      if (kind == TargetKind::InternalLabel ||
          (kind == TargetKind::Unknown && target >= fn.base() && target < fn.end())) {
        edges_.emplace_back(index, target);
      } else {
        call(index, target);
      }
      return;
    }
    if (op == 16) {  // bc
      const int32_t bd = static_cast<int32_t>((word & 0xFFFC) ^ 0x8000) - 0x8000;
      const uint32_t target = (word & 2) ? static_cast<uint32_t>(bd) : address + bd;
      const auto kind = graph_.classifyTarget(target, address, false, &fn);
      if (kind == TargetKind::InternalLabel && !link)
        edges_.emplace_back(index, target);
      else if (kind == TargetKind::Function || kind == TargetKind::Import)
        call(index, target);
      return;
    }
    if (op != 19)
      return;
    const uint32_t xo = (word >> 1) & 0x3FF;
    const bool always = ((word >> 21) & 0x14) == 0x14;
    if (xo == 16) {  // bclr: a return, or an indirect call with LK
      if (!link && always)
        step.fallthrough = false;
      return;
    }
    if (xo != 528 || link)  // bcctrl: an indirect call
      return;

    // bcctr: a switch over a jump table, or an indirect tail call.
    const JumpTable* table = nullptr;
    if (auto it = config_.switchTables.find(address); it != config_.switchTables.end())
      table = &it->second;
    for (const auto& jt : fn.jumpTables()) {
      if (!table && jt.bctrAddress == address)
        table = &jt;
    }
    std::optional<JumpTable> late;
    if (!table) {
      if (const auto* data = reinterpret_cast<const uint32_t*>(binary_.translate(address))) {
        late = DetectLateJumpTable(binary_, address, data);
        if (late)
          table = &*late;
      }
    }
    if (always)
      step.fallthrough = false;
    if (!table)
      return;
    if (table->indexRegister < 32)
      step.reads |= Bit(table->indexRegister);
    for (uint32_t label : table->targets) {
      if (label == 0)
        continue;
      const auto kind = graph_.classifyTarget(label, address, false, &fn);
      if (kind == TargetKind::Function || kind == TargetKind::Import)
        call(index, label);
      else if (kind == TargetKind::InternalLabel || (label >= fn.base() && label < fn.end()))
        edges_.emplace_back(index, label);
    }
  }

  void build(const FunctionNode& fn) {
    Mask r1Derived = Bit(1);  // registers holding r1 + offset
    ppc_insn insn;
    for (const auto& block : fn.blocks()) {
      const auto* data = reinterpret_cast<const uint32_t*>(binary_.translate(block.base));
      if (!data)
        continue;
      for (uint32_t address = block.base; address < block.end(); address += 4, ++data) {
        auto hook = config_.midAsmHooks.find(address);
        const bool hasHook = hook != config_.midAsmHooks.end();
        if (hasHook && !hook->second.afterInstruction)
          addHook(address, hook->second);

        const uint32_t word = load_and_swap<uint32_t>(reinterpret_cast<const uint8_t*>(data));
        ppc::Disassemble(data, 4, address, insn);
        const size_t index = steps_.size();
        labels_.try_emplace(address, index);
        Step step;
        if (insn.opcode != nullptr) {
          const UseDef ud = GprUseDef(insn);
          step.reads = ud.reads;
          step.writes = ud.writes;
          bool indexed = false;
          if (IsGprStore(insn.opcode->id, indexed)) {
            uint32_t base = insn.operands[indexed ? 1 : 2];
            bool stack = base < 32 && (r1Derived & Bit(base)) != 0;
            if (indexed && insn.operands[2] < 32 && (r1Derived & Bit(insn.operands[2])))
              stack = true;
            if (stack)
              step.reads &= ~kNonVolatile;
          }
          // addi rD,r1,imm: the stack again; anything else written is not.
          if (insn.opcode->id == PPC_INST_ADDI && insn.operands[1] == 1 && insn.operands[0] != 1)
            r1Derived |= Bit(insn.operands[0]);
          else
            r1Derived &= ~(step.writes & ~Bit(1));
          branch(fn, index, step, address, word);
        }
        steps_.push_back(step);

        if (hasHook && hook->second.afterInstruction)
          addHook(address, hook->second);
      }
    }
  }

  Summary flow() {
    const size_t n = steps_.size();
    Summary summary;
    if (n == 0)
      return summary;

    std::vector<std::vector<size_t>> succ(n);
    for (size_t i = 0; i + 1 < n; ++i) {
      if (steps_[i].fallthrough)
        succ[i].push_back(i + 1);
    }
    for (const auto& [from, address] : edges_) {
      if (auto it = labels_.find(address); it != labels_.end())
        succ[from].push_back(it->second);
    }

    // A loop is first entered from the code before it, so a register its body
    // sets is not set yet the first time its head runs. The edges that close
    // loops (those to a step still on the path of a depth-first walk from the
    // entry) carry no definitions. Without this, a loop head that is the
    // function's entry (a loop entered by a branch from another function,
    // with the running value in r11) took the r11 its own body writes for set.
    std::vector<std::vector<size_t>> forward(n);
    {
      std::vector<uint8_t> state(n, 0);  // 0 not seen, 1 on the path, 2 done
      std::vector<std::pair<size_t, size_t>> path;  // step, index of its next successor
      path.emplace_back(size_t{0}, size_t{0});
      state[0] = 1;
      while (!path.empty()) {
        const size_t i = path.back().first;
        const size_t next = path.back().second;
        if (next == succ[i].size()) {
          state[i] = 2;
          path.pop_back();
          continue;
        }
        ++path.back().second;
        const size_t j = succ[i][next];
        if (state[j] == 1)
          continue;  // closes a loop
        forward[i].push_back(j);
        if (state[j] == 0) {
          state[j] = 1;
          path.emplace_back(j, size_t{0});
        }
      }
    }

    std::vector<Mask> defined(n, 0);
    std::vector<bool> reached(n, false);
    std::vector<size_t> work{0};
    reached[0] = true;
    while (!work.empty()) {
      const size_t i = work.back();
      work.pop_back();
      const Mask out = defined[i] | steps_[i].writes;
      for (size_t j : forward[i]) {
        if (!reached[j] || (defined[j] | out) != defined[j]) {
          reached[j] = true;
          defined[j] |= out;
          work.push_back(j);
        }
      }
    }

    for (size_t i = 0; i < n; ++i) {
      if (reached[i])
        summary.direct |= steps_[i].reads & ~defined[i] & tracked_;
    }
    std::unordered_map<const FunctionNode*, Mask> calls;
    for (const auto& [index, callee] : callSites_) {
      if (!reached[index])
        continue;
      auto [it, inserted] = calls.try_emplace(callee, defined[index]);
      if (!inserted)
        it->second &= defined[index];
    }
    summary.calls.assign(calls.begin(), calls.end());
    return summary;
  }

  const FunctionGraph& graph_;
  const BinaryView& binary_;
  const RecompilerConfig& config_;
  const Mask tracked_;
  std::vector<Step> steps_;
  std::vector<std::pair<size_t, uint32_t>> edges_;  // step -> label address
  std::vector<std::pair<size_t, const FunctionNode*>> callSites_;
  std::unordered_map<uint32_t, size_t> labels_;  // address -> its first step
};

std::string RegisterList(Mask mask) {
  std::string out;
  for (uint32_t reg = 0; reg < 32; ++reg) {
    if (mask & Bit(reg))
      out += fmt::format("{}r{}", out.empty() ? "" : " ", reg);
  }
  return out;
}

}  // namespace

size_t MarkCallerRegisterReaders(FunctionGraph& graph, const BinaryView& binary,
                                 const RecompilerConfig& config) {
  Mask tracked = 0;
  if (config.nonArgumentGprsAsLocalVariables || config.nonArgumentRegistersAsLocalVariables)
    tracked |= kNonArgument;
  if (config.nonVolatileGprsAsLocalVariables || config.nonVolatileRegistersAsLocalVariables)
    tracked |= kNonVolatile;
  if (tracked == 0 || !config.autoShareRegisters)
    return 0;

  auto isJmp = [&](uint32_t base) {
    return config.isSetJmp(base) || config.isLongJmp(base);
  };

  // setjmp and longjmp store and load every non-volatile of their caller (and
  // direct calls to them become ppc_setjmp / ppc_longjmp): no live-ins.
  Walker walker(graph, binary, config, tracked);
  std::unordered_map<const FunctionNode*, Summary> summaries;
  summaries.reserve(graph.functions().size());
  for (const auto& [base, node] : graph.functions()) {
    if (node->isImport() || node->blocks().empty() || isJmp(base))
      continue;
    summaries.emplace(node.get(), walker.summarize(*node));
  }

  std::unordered_map<const FunctionNode*, Mask> liveIns;
  liveIns.reserve(summaries.size());
  for (const auto& [node, summary] : summaries)
    liveIns[node] = summary.direct;
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto& [node, summary] : summaries) {
      Mask mask = summary.direct;
      for (const auto& [callee, defined] : summary.calls) {
        if (auto it = liveIns.find(callee); it != liveIns.end())
          mask |= it->second & ~defined;
      }
      Mask& current = liveIns[node];
      if ((mask | current) != current) {
        current |= mask;
        changed = true;
      }
    }
  }

  size_t marked = 0, already = 0;
  Mask seen = 0;
  for (const auto& [base, node] : graph.functions()) {
    auto it = liveIns.find(node.get());
    if (it == liveIns.end() || it->second == 0 || IsSaveRestoreHelperName(node->name()))
      continue;
    // Already sharing (funclets, chunks, the manifest): listed too, so a
    // manifest entry can be checked against the analysis.
    const bool shared = node->sharesRegisters();
    REXCODEGEN_DEBUG("Analyze: 0x{:08X} reads its caller's {}: share_registers{}", base,
                     RegisterList(it->second), shared ? " (already)" : "");
    if (shared) {
      ++already;
      continue;
    }
    node->setSharesRegisters(true);
    ++marked;
    seen |= it->second;
  }
  if (marked > 0 || already > 0) {
    REXCODEGEN_INFO(
        "Analyze: {} functions read registers their caller set ({}): share_registers; {} more "
        "already had it",
        marked, RegisterList(seen), already);
  }
  return marked;
}

}  // namespace rex::codegen
