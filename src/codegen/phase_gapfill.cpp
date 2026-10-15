/**
 * @file        codegen/phase_gapfill.cpp
 * @brief       GapFill phase: find uncovered code regions and register them as functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builders.h"
#include "decoded_binary.h"
#include "ppc/disasm.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <unordered_set>

#include <rex/codegen/phases.h>
#include "phase_helpers.h"

#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>

#include <ppc.h>

using rex::codegen::ppc::Opcode;
using rex::memory::load_and_swap;

namespace rex::codegen {

namespace {

//=============================================================================
// GapFill to register uncovered code regions
//=============================================================================
// The first pass is the original gap fill: it cuts each code region after
// every `blr` and every tail call to a known function, registers the pieces
// nothing claims, discovers them treating other gap fills as unknown (so the
// pieces of one function without PDATA join up again) and drops the pieces
// another function absorbed.
//
// Then, until nothing new is found, each pass only adds:
//  1. every direct branch target that leaves its function and lands where
//     no function starts (a `b` to a function without PDATA that nothing
//     calls with `bl`); a gap fill the target lies inside is discovered
//     again so that it stops there;
//  2. every piece of code nothing covers that reads as complete code, cut
//     after each instruction that ends straight-line flow (a thunk followed
//     by an unrelated function is two pieces).
// These later pieces are discovered with every function known, so they end
// in a tail call where they reach an existing function instead of absorbing
// it, and only pieces of the same pass can be dropped: no entry the first
// pass found is lost.

constexpr size_t kMaxGapFillPasses = 32;

// True if control never falls through to the next instruction.
bool endsFlow(const DecodedInsn& insn) {
  if (insn.is_return())
    return true;
  if (insn.opcode == Opcode::b || insn.opcode == Opcode::ba)
    return true;
  if ((insn.opcode == Opcode::bcctr || insn.opcode == Opcode::bclr) && !insn.is_conditional())
    return true;
  return false;
}

bool inDataRegion(const CodegenContext& ctx, uint32_t start, uint32_t end) {
  for (const auto& [dataStart, dataEnd] : ctx.scan.dataRegions) {
    if (start < dataEnd && dataStart < end)
      return true;
  }
  return false;
}

// Split [start, end) after every instruction that ends straight-line flow.
std::vector<CodeRegion> splitOnFlowEnds(const DecodedBinary& decoded, uint32_t start,
                                        uint32_t end) {
  std::vector<CodeRegion> segments;
  uint32_t segmentStart = start;
  for (uint32_t addr = start; addr < end; addr += 4) {
    const auto* insn = decoded.get(addr);
    if (insn && endsFlow(*insn)) {
      segments.push_back({segmentStart, addr + 4});
      segmentStart = addr + 4;
    }
  }
  if (segmentStart < end)
    segments.push_back({segmentStart, end});
  return segments;
}

// The analysis decoder (ppc/opcode.h) knows fewer instructions than the
// emitter, binutils plus the builder table: `subfe` is one it lacks. Code is
// what the emitter can translate, so ask the emitter when the analysis decoder
// gives up. RDR 0x8278B4E0 (`subfe`) and 0x82AFFEF0 were skipped as "not code"
// that way.
bool isEmittable(const DecodedInsn& insn) {
  const uint32_t raw = static_cast<uint32_t>(insn.code);
  if (raw == 0x00000000 || raw == 0xFFFFFFFF)
    return false;
  if (insn.opcode != Opcode::kUnknown)
    return true;
  const uint8_t bytes[4] = {static_cast<uint8_t>(raw >> 24), static_cast<uint8_t>(raw >> 16),
                            static_cast<uint8_t>(raw >> 8), static_cast<uint8_t>(raw)};
  ppc_insn emitted;
  ppc::Disassemble(bytes, 4, insn.address, emitted);
  return emitted.opcode && CanDispatchInstruction(emitted.opcode->id);
}

// A segment found after discovery has already run is only accepted if it
// reads as a complete piece of code: every word is an instruction the emitter
// translates and the last one ends the flow. That keeps tables and constants
// between functions out.
bool looksLikeCode(const DecodedBinary& decoded, const CodeRegion& segment) {
  for (uint32_t addr = segment.start; addr < segment.end; addr += 4) {
    const auto* insn = decoded.get(addr);
    if (!insn || !isEmittable(*insn))
      return false;
    if (addr + 4 == segment.end)
      return endsFlow(*insn);
  }
  return false;
}

// Check if address looks like exception handler data (handler ptr + rdata ptr)
bool looksLikeExceptionData(const BinaryView& binary, const FunctionGraph& graph, uint32_t addr) {
  const uint8_t* data = binary.translate(addr);
  if (!data)
    return false;

  // Exception handler data pattern:
  // [addr+0]: pointer to __C_specific_handler (entry point)
  // [addr+4]: pointer to scope table in .rdata
  uint32_t firstDword = load_and_swap<uint32_t>(data);
  uint32_t secondDword = load_and_swap<uint32_t>(data + 4);

  // Check if first dword is a known entry point (like __C_specific_handler)
  if (!graph.isEntryPoint(firstDword)) {
    return false;
  }

  // Check if second dword points to .rdata section
  auto* rdataSection = binary.findSectionByName(".rdata");
  if (!rdataSection)
    return false;

  uint32_t rdataStart = rdataSection->baseAddress;
  uint32_t rdataEnd = rdataStart + rdataSection->size;

  if (secondDword >= rdataStart && secondDword < rdataEnd) {
    REXCODEGEN_TRACE(
        "GapFill: 0x{:08X} looks like exception data (handler=0x{:08X}, scope=0x{:08X}), skipping",
        addr, firstDword, secondDword);
    return true;
  }

  return false;
}

// First pass: cut a code region after every `blr` and every tail call to a
// known function.
std::vector<CodeRegion> splitRegionOnTerminators(const CodeRegion& region,
                                                 const DecodedBinary& decoded,
                                                 const std::unordered_set<uint32_t>& knownCallables) {
  std::vector<CodeRegion> segments;
  uint32_t segmentStart = region.start;
  for (uint32_t addr = region.start; addr < region.end; addr += 4) {
    const auto* insn = decoded.get(addr);
    if (!insn)
      break;
    bool split = insn->is_return();
    if (!split && insn->opcode == Opcode::b && insn->branch_target.has_value()) {
      uint32_t target = *insn->branch_target;
      // Not on tail recursion (a branch to the segment's own start)
      split = target != segmentStart && knownCallables.contains(target);
    }
    if (split) {
      segments.push_back({segmentStart, addr + 4});
      segmentStart = addr + 4;
    }
  }
  if (segmentStart < region.end)
    segments.push_back({segmentStart, region.end});
  return segments;
}

void gapFillFirstPass(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  auto& binary = ctx.binary();

  std::unordered_set<uint32_t> knownCallables;
  for (const auto& [addr, node] : graph.functions())
    knownCallables.insert(addr);

  size_t segmentsCreated = 0;
  for (const auto& region : ctx.scan.codeRegions) {
    for (const auto& segment : splitRegionOnTerminators(region, ctx.decoded(), knownCallables)) {
      if (graph.isEntryPoint(segment.start) || graph.getFunctionContaining(segment.start))
        continue;
      if (looksLikeExceptionData(binary, graph, segment.start))
        continue;
      graph.addFunction(segment.start, segment.size(), FunctionAuthority::GAP_FILL, false);
      REXCODEGEN_TRACE("GapFill: registered sub_{:08X} (0x{:08X}-0x{:08X}, {} bytes)",
                       segment.start, segment.start, segment.end, segment.size());
      segmentsCreated++;
    }
  }
  REXCODEGEN_TRACE("Analyze: registered {} gap functions", segmentsCreated);
}

// Every address range some function already claims, sorted and merged.
// PDATA and CONFIG functions claim their declared extent. With `spans`, the
// rest claim the span from their entry to their last block: unreached code
// between two blocks (cases of a switch nothing decoded, say) belongs to that
// function, so no gap piece starts there. Without it they claim their blocks
// only, which is what a branch from another function is checked against. A
// gap fill's registered size is only the segment it was cut from and never
// counts.
std::vector<CodeRegion> buildCoverage(const FunctionGraph& graph, bool spans = true) {
  std::vector<CodeRegion> ranges;
  for (const auto& [addr, node] : graph.functions()) {
    auto authority = node->authority();
    if (node->blocks().empty() || authority == FunctionAuthority::PDATA ||
        authority == FunctionAuthority::CONFIG) {
      ranges.push_back({node->base(), std::max(node->end(), node->base() + 4)});
    }
    if (spans && !node->blocks().empty()) {
      uint32_t start = node->base();
      uint32_t end = start + 4;
      for (const auto& block : node->blocks()) {
        start = std::min(start, block.base);
        end = std::max(end, block.base + block.size);
      }
      ranges.push_back({start, end});
    } else {
      for (const auto& block : node->blocks())
        ranges.push_back({block.base, block.base + block.size});
    }
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const CodeRegion& a, const CodeRegion& b) { return a.start < b.start; });
  std::vector<CodeRegion> merged;
  for (const auto& range : ranges) {
    if (!merged.empty() && range.start <= merged.back().end)
      merged.back().end = std::max(merged.back().end, range.end);
    else
      merged.push_back(range);
  }
  return merged;
}

bool isCovered(const std::vector<CodeRegion>& coverage, uint32_t addr) {
  auto it = std::upper_bound(coverage.begin(), coverage.end(), addr,
                             [](uint32_t a, const CodeRegion& r) { return a < r.start; });
  return it != coverage.begin() && std::prev(it)->contains(addr);
}

// Code regions sorted by start, for address lookups.
class RegionIndex {
 public:
  explicit RegionIndex(const std::vector<CodeRegion>& regions) : regions_(regions) {
    std::sort(regions_.begin(), regions_.end(),
              [](const CodeRegion& a, const CodeRegion& b) { return a.start < b.start; });
  }
  const CodeRegion* find(uint32_t addr) const {
    auto it = std::upper_bound(regions_.begin(), regions_.end(), addr,
                               [](uint32_t a, const CodeRegion& r) { return a < r.start; });
    if (it == regions_.begin() || !std::prev(it)->contains(addr))
      return nullptr;
    return &*std::prev(it);
  }

 private:
  std::vector<CodeRegion> regions_;
};

// A plausible branch or pointer target: aligned, real code, a real instruction.
bool isCodeTarget(const CodegenContext& ctx, const RegionIndex& regions, uint32_t addr) {
  if ((addr & 3) || ctx.binary().isInImportExportRange(addr) || !regions.find(addr))
    return false;
  const auto* insn = ctx.decoded().get(addr);
  return insn && isEmittable(*insn);
}

// The gap fill whose blocks contain `addr` without starting there, if any.
FunctionNode* gapFillContaining(FunctionGraph& graph, uint32_t addr) {
  auto* node = graph.getFunctionContaining(addr);
  if (node && node->authority() == FunctionAuthority::GAP_FILL && node->base() != addr)
    return node;
  return nullptr;
}

// Direct branch targets are function entries when they leave the branching
// function: a `b` to a function without PDATA that nothing reaches with `bl`.
// A target no function's blocks cover is registered, also where it lies
// between the blocks of another function (a thunk jumping over it, say). A
// target inside a gap fill's blocks is registered too, and the gap fill is
// discovered again so that it stops there: gap fill only guessed where that
// code starts. `coverage` holds blocks and PDATA/config extents only. Every
// function registered here, the rediscovered gap fills included, goes to
// `added`.
size_t registerBranchTargets(CodegenContext& ctx, const RegionIndex& regions,
                             const std::vector<CodeRegion>& coverage,
                             std::vector<uint32_t>& added) {
  auto& graph = ctx.graph;

  std::set<uint32_t> targets;
  std::set<uint32_t> splitGapFills;
  for (const auto& [addr, node] : graph.functions()) {
    for (const auto& jump : node->unresolvedJumps()) {
      uint32_t target = jump.target;
      if (graph.isEntryPoint(target) || node->containsAddress(target) ||
          !isCodeTarget(ctx, regions, target))
        continue;
      if (!isCovered(coverage, target)) {
        targets.insert(target);
      } else if (auto* gapFill = gapFillContaining(graph, target); gapFill && gapFill != node.get()) {
        targets.insert(target);
        splitGapFills.insert(gapFill->base());
      }
    }
  }
  for (uint32_t base : splitGapFills) {
    graph.removeFunction(base);
    graph.addFunction(base, 4, FunctionAuthority::GAP_FILL, false);
    REXCODEGEN_TRACE("GapFill: rediscovering sub_{:08X}, a branch target lies inside it", base);
    added.push_back(base);
  }
  for (uint32_t target : targets) {
    graph.addFunction(target, 4, FunctionAuthority::DISCOVERED, true);
    REXCODEGEN_TRACE("GapFill: registered branch target sub_{:08X}", target);
    added.push_back(target);
  }
  return targets.size();
}

std::vector<uint32_t> gapFillCodeRegions(CodegenContext& ctx, const std::vector<CodeRegion>& coverage,
                                         const std::unordered_set<uint32_t>& rejected) {
  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& decoded = ctx.decoded();

  std::vector<uint32_t> created;
  for (const auto& region : ctx.scan.codeRegions) {
    uint32_t addr = region.start;
    while (addr < region.end) {
      if (isCovered(coverage, addr) || binary.isInImportExportRange(addr)) {
        addr += 4;
        continue;
      }
      uint32_t runStart = addr;
      while (addr < region.end && !isCovered(coverage, addr) &&
             !binary.isInImportExportRange(addr)) {
        addr += 4;
      }

      for (const auto& segment : splitOnFlowEnds(decoded, runStart, addr)) {
        if (graph.isEntryPoint(segment.start) || rejected.contains(segment.start))
          continue;
        if (looksLikeExceptionData(binary, graph, segment.start))
          continue;
        if (!looksLikeCode(decoded, segment) || inDataRegion(ctx, segment.start, segment.end)) {
          REXCODEGEN_TRACE("GapFill: 0x{:08X}-0x{:08X} does not read as code, skipping",
                           segment.start, segment.end);
          continue;
        }
        graph.addFunction(segment.start, segment.size(), FunctionAuthority::GAP_FILL, false);
        REXCODEGEN_TRACE("GapFill: registered sub_{:08X} (0x{:08X}-0x{:08X}, {} bytes)",
                         segment.start, segment.start, segment.end, segment.size());
        created.push_back(segment.start);
      }
    }
  }
  return created;
}

// Drops the candidates of this pass that turned out not to be functions of
// their own and remembers them so later passes leave them alone:
//  - a gap piece another function's blocks already contain;
//  - any candidate that branches into the middle of another function: it is
//    part of that function (unreached code after a `b`, for example), and
//    compiled on its own it would branch to a label it cannot reach.
// Returns the dropped entries.
std::vector<uint32_t> dropFalseEntries(CodegenContext& ctx, const std::vector<uint32_t>& candidates,
                                       std::unordered_set<uint32_t>& rejected) {
  auto& graph = ctx.graph;
  auto coverage = buildCoverage(graph);
  std::vector<uint32_t> toRemove;
  for (uint32_t addr : candidates) {
    auto* node = graph.getFunction(addr);
    if (!node || node->blocks().empty())
      continue;
    bool falseEntry = false;
    if (node->authority() == FunctionAuthority::GAP_FILL) {
      for (const auto& [otherAddr, other] : graph.functions()) {
        if (otherAddr != addr && other->containsAddress(addr) && !other->blocks().empty()) {
          falseEntry = true;
          break;
        }
      }
    }
    for (const auto& jump : node->unresolvedJumps()) {
      if (falseEntry)
        break;
      if (jump.isCall || graph.isEntryPoint(jump.target) || node->containsAddress(jump.target))
        continue;
      falseEntry = isCovered(coverage, jump.target);
    }
    if (falseEntry)
      toRemove.push_back(addr);
  }
  for (uint32_t addr : toRemove) {
    REXCODEGEN_TRACE("GapFill: dropping sub_{:08X}, it is part of another function", addr);
    graph.removeFunction(addr);
    rejected.insert(addr);
  }
  return toRemove;
}

// A function discovered in the same pass as a dropped entry saw that entry as
// a function: its branch there became a tail call and its blocks stop short of
// that code. Fuse 0x82EF5158 (a binary search) lost the `addi r6,r3,1` arm at
// 0x82EF51B4 to a gap piece this way, and the `bge` to it became unresolved.
// Discover such functions again now that the entry is gone, so the code joins
// them as a label. All of them were registered in this pass, with a size of 4
// or their gap segment, so registering them again loses nothing.
size_t rediscoverTailCallers(CodegenContext& ctx, const std::vector<uint32_t>& pass,
                             const std::vector<uint32_t>& dropped) {
  auto& graph = ctx.graph;
  const std::unordered_set<uint32_t> droppedSet(dropped.begin(), dropped.end());
  std::vector<std::pair<uint32_t, FunctionAuthority>> stale;
  for (uint32_t addr : pass) {
    const auto* node = graph.getFunction(addr);
    if (!node)
      continue;
    for (const auto& edge : node->tailCalls()) {
      const auto* target = std::get_if<CallTarget::Unresolved>(&edge.target.value);
      if (target && droppedSet.contains(target->address)) {
        stale.emplace_back(addr, node->authority());
        break;
      }
    }
  }
  for (const auto& [addr, authority] : stale) {
    REXCODEGEN_TRACE("GapFill: rediscovering sub_{:08X}, an entry it branched to was dropped",
                     addr);
    graph.removeFunction(addr);
    graph.addFunction(addr, 4, authority, authority != FunctionAuthority::GAP_FILL);
  }
  return stale.size();
}

// Code addresses the program takes: `lis` + `addi`/`ori` constants in code and
// words in data sections (vtables without RTTI, callback tables, member
// function pointers). Only plausible code targets are kept.
std::vector<uint32_t> collectPointerTargets(const CodegenContext& ctx, const RegionIndex& regions) {
  auto& decoded = ctx.decoded();
  std::set<uint32_t> found;
  auto consider = [&](uint32_t value) {
    if (isCodeTarget(ctx, regions, value))
      found.insert(value);
  };

  for (const auto& region : ctx.scan.codeRegions) {
    std::array<std::optional<uint32_t>, 32> high{};
    for (uint32_t addr = region.start; addr < region.end; addr += 4) {
      const auto* insn = decoded.get(addr);
      if (!insn)
        continue;
      if (isLis(*insn)) {
        high[insn->D.RT] = static_cast<uint32_t>(insn->D.SIMM()) << 16;
        continue;
      }
      if (insn->opcode == Opcode::addi && insn->D.RA != 0 && high[insn->D.RA])
        consider(*high[insn->D.RA] + static_cast<uint32_t>(insn->D.SIMM()));
      else if (insn->opcode == Opcode::ori && high[insn->D.RT])
        consider(*high[insn->D.RT] | insn->D.d);
      for (uint8_t reg : insn->get_register_writes()) {
        if (reg < high.size())
          high[reg].reset();
      }
      if (endsFlow(*insn))
        high.fill(std::nullopt);
    }
  }

  for (const auto& section : ctx.binary().sections()) {
    if (section.executable || !section.data)
      continue;
    for (uint32_t offset = 0; offset + 4 <= section.size; offset += 4)
      consider(load_and_swap<uint32_t>(section.data + offset));
  }
  return {found.begin(), found.end()};
}

// A taken code address is a function entry where no function covers it, or
// where a function without PDATA only reaches it by falling through (a
// this-adjusting thunk that falls into the plain one, for example). A label
// of the function around it (a branch or jump table target) is not an entry,
// and the extent of a PDATA or config function is left alone.
size_t registerPointerTargets(CodegenContext& ctx, const std::vector<uint32_t>& candidates,
                              const std::vector<CodeRegion>& coverage,
                              const std::unordered_set<uint32_t>& rejected,
                              std::vector<uint32_t>& added) {
  auto& graph = ctx.graph;
  auto& decoded = ctx.decoded();
  size_t count = 0;
  for (uint32_t target : candidates) {
    if (graph.isEntryPoint(target) || rejected.contains(target))
      continue;
    if (!isCovered(coverage, target)) {
      // Must read as code up to the end of its first straight-line piece.
      uint32_t end = target;
      while (decoded.get(end) && !endsFlow(*decoded.get(end)) && end - target < 0x1000)
        end += 4;
      if (!looksLikeCode(decoded, {target, end + 4}))
        continue;
    } else {
      auto* owner = graph.getFunctionContaining(target);
      if (!owner || owner->isLabel(target))
        continue;
      auto authority = owner->authority();
      if (authority != FunctionAuthority::GAP_FILL && authority != FunctionAuthority::DISCOVERED &&
          authority != FunctionAuthority::VTABLE)
        continue;
    }
    graph.addFunction(target, 4, FunctionAuthority::DISCOVERED, true);
    REXCODEGEN_TRACE("GapFill: registered pointer target sub_{:08X}", target);
    added.push_back(target);
    count++;
  }
  return count;
}

//=============================================================================
// Cleanup absorbed GAP_FILL functions
//=============================================================================

void cleanupAbsorbedGapFills(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  std::vector<uint32_t> toRemove;

  for (const auto& [addr, node] : graph.functions()) {
    if (node->authority() != FunctionAuthority::GAP_FILL)
      continue;

    for (const auto& [otherAddr, otherNode] : graph.functions()) {
      if (otherAddr == addr)
        continue;
      if (!otherNode->containsAddress(addr))
        continue;

      // This GAP_FILL is inside another function's blocks
      if (otherNode->authority() != FunctionAuthority::GAP_FILL) {
        // Absorbed by higher authority - remove
        toRemove.push_back(addr);
        break;
      } else if (otherAddr < addr) {
        // Both GAP_FILL, other has lower address - it survives
        toRemove.push_back(addr);
        break;
      }
    }
  }

  for (uint32_t addr : toRemove) {
    graph.removeFunction(addr);
  }

  if (!toRemove.empty()) {
    REXCODEGEN_TRACE("Analyze: removed {} absorbed GAP_FILL functions", toRemove.size());
  }
}

}  // anonymous namespace

namespace phases {

VoidResult GapFill(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  std::unordered_set<uint32_t> rejected;
  size_t totalTargets = 0;
  size_t totalSegments = 0;
  size_t totalRejected = 0;

  gapFillFirstPass(ctx);
  discoverPendingFunctions(ctx, buildKnownFunctions(ctx.graph, /*excludeGapFill=*/true));
  cleanupAbsorbedGapFills(ctx);

  RegionIndex regions(ctx.scan.codeRegions);
  const auto pointers = collectPointerTargets(ctx, regions);
  size_t totalPointers = 0;

  size_t pass = 0;
  for (; pass < kMaxGapFillPasses; ++pass) {
    auto coverage = buildCoverage(ctx.graph);
    std::vector<uint32_t> registered;
    size_t targets =
        registerBranchTargets(ctx, regions, buildCoverage(ctx.graph, false), registered);
    std::vector<uint32_t> candidates;
    size_t pointerTargets = registerPointerTargets(ctx, pointers, coverage, rejected, candidates);
    if (targets > 0 || pointerTargets > 0)
      coverage = buildCoverage(ctx.graph);
    auto segments = gapFillCodeRegions(ctx, coverage, rejected);
    if (targets == 0 && pointerTargets == 0 && segments.empty())
      break;
    totalTargets += targets;
    totalPointers += pointerTargets;
    totalSegments += segments.size();
    candidates.insert(candidates.end(), segments.begin(), segments.end());
    registered.insert(registered.end(), candidates.begin(), candidates.end());

    discoverPendingFunctions(ctx, buildKnownFunctions(ctx.graph));
    // Every round drops at least one candidate for good, so this ends.
    for (;;) {
      auto dropped = dropFalseEntries(ctx, candidates, rejected);
      totalRejected += dropped.size();
      if (dropped.empty() || rediscoverTailCallers(ctx, registered, dropped) == 0)
        break;
      discoverPendingFunctions(ctx, buildKnownFunctions(ctx.graph));
    }
  }
  if (pass == kMaxGapFillPasses) {
    REXCODEGEN_WARN("GapFill: still finding code after {} passes, stopped; some code may be "
                    "left without a function",
                    kMaxGapFillPasses);
  }

  REXCODEGEN_DEBUG(
      "GapFill: {} extra pass(es), {} branch targets, {} of {} code pointers, {} pieces, {} "
      "dropped",
      pass, totalTargets, totalPointers, pointers.size(), totalSegments, totalRejected);
  return Ok();
}

}  // namespace phases

}  // namespace rex::codegen
