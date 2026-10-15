/**
 * @file        codegen/analyze.cpp
 * @brief       Analysis pipeline orchestrator
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "codegen_flags.h"
#include "decoded_binary.h"

#include <algorithm>
#include <fstream>
#include <map>

#include <rex/codegen/analysis_errors.h>
#include <rex/codegen/analyze.h>
#include <rex/codegen/phases.h>
#include <rex/logging.h>

#include "codegen_logging.h"

namespace rex::codegen {

namespace {

// One line per function: address, size, the source that registered it, then
// one indented line per jump table (bctr, table, entries). The first module a
// run analyzes truncates the file, later ones append.
void writeAnalysisReport(const CodegenContext& ctx) {
  const std::string path = REXCVAR_GET(analysis_report);
  if (path.empty())
    return;

  static bool truncated = false;
  std::ofstream out(path, truncated ? std::ios::app : std::ios::trunc);
  if (!out) {
    REXCODEGEN_WARN("analysis_report: cannot write {}", path);
    return;
  }
  truncated = true;

  std::vector<const FunctionNode*> nodes;
  for (const auto& [addr, node] : ctx.graph.functions())
    nodes.push_back(node.get());
  std::sort(nodes.begin(), nodes.end(),
            [](const FunctionNode* a, const FunctionNode* b) { return a->base() < b->base(); });

  out << "# module " << ctx.Config().filePath << "\n";
  for (const auto* node : nodes) {
    out << fmt::format("{:08X} {:X} {}\n", node->base(), node->size(),
                       AuthorityName(node->authority()));
    for (const auto& jt : node->jumpTables()) {
      out << fmt::format("  jt {:08X} {:08X} {}\n", jt.bctrAddress, jt.tableAddress,
                         jt.targets.size());
    }
  }
}

}  // namespace

Result<void> Analyze(CodegenContext& ctx, ProgressReporter* reporter) {
  REXCODEGEN_TRACE("Analyze: starting analysis...");

  ctx.initDecoded();
  REXCODEGEN_TRACE("Analyze: decoded {} instructions across {} code regions",
                   ctx.decoded().instructionCount(), ctx.decoded().codeRegions().size());

  // 1. Register entry points (imports, helpers, config, pdata)
  if (reporter)
    reporter->phaseChanged("Register");
  auto regResult = phases::Register(ctx, reporter);
  if (!regResult) {
    return regResult;
  }

  // 2. Scan binary into code/data regions
  if (reporter)
    reporter->phaseChanged("Scan");
  auto scanResult = phases::Scan(ctx, reporter);
  if (!scanResult) {
    return scanResult;
  }

  // 3. Discover function blocks iteratively (includes vtable scan)
  if (reporter)
    reporter->phaseChanged("Discover");
  auto discoverResult = phases::Discover(ctx, reporter);
  if (!discoverResult) {
    return discoverResult;
  }

  // 4. Gap fill uncovered regions, branch and pointer targets, then discover them
  if (reporter)
    reporter->phaseChanged("GapFill");
  auto gapFillResult = phases::GapFill(ctx, reporter);
  if (!gapFillResult) {
    return gapFillResult;
  }

  // 5. Merge: resolve jumps and seal functions
  if (reporter)
    reporter->phaseChanged("Merge");
  auto mergeResult = phases::Merge(ctx, reporter);
  if (!mergeResult) {
    return mergeResult;
  }

  // Before Validate, so the report also explains a failed validation.
  writeAnalysisReport(ctx);

  // 6. Validate
  if (reporter)
    reporter->phaseChanged("Validate");
  auto validateResult = phases::Validate(ctx, reporter);
  if (!validateResult) {
    return validateResult;
  }

  REXCODEGEN_TRACE("Analyze: complete - {} functions ready for code generation",
                   ctx.graph.functionCount());

  return Ok();
}

//=============================================================================
// AnalysisErrors implementation
//=============================================================================

const char* AnalysisErrors::CategoryName(Category cat) {
  switch (cat) {
    case Category::UnresolvedCall:
      return "UnresolvedCall";
    case Category::MissingJumpTable:
      return "MissingJumpTable";
    case Category::JumpTargetOutOfBounds:
      return "JumpTargetOutOfBounds";
    case Category::DiscontinuousFunction:
      return "DiscontinuousFunction";
    case Category::UnimplementedInsn:
      return "UnimplementedInsn";
    default:
      return "Unknown";
  }
}

void AnalysisErrors::Add(Category cat, uint32_t addr, const std::string& msg) {
  Add(cat, addr, 0, msg);
}

void AnalysisErrors::Add(Category cat, uint32_t addr, uint32_t secondary, const std::string& msg) {
  entries_.push_back({cat, addr, secondary, msg});
}

size_t AnalysisErrors::Count(Category cat) const {
  return std::count_if(entries_.begin(), entries_.end(),
                       [cat](const Entry& e) { return e.category == cat; });
}

void AnalysisErrors::PrintReport() const {
  if (entries_.empty()) {
    return;
  }

  REXCODEGEN_ERROR("=== ANALYSIS ERRORS ===");

  // Group by category
  std::map<Category, std::vector<const Entry*>> byCategory;
  for (const auto& entry : entries_) {
    byCategory[entry.category].push_back(&entry);
  }

  for (const auto& [cat, entries] : byCategory) {
    REXCODEGEN_ERROR("{} ({}):", CategoryName(cat), entries.size());

    for (const auto* entry : entries) {
      if (entry->secondaryAddress != 0) {
        REXCODEGEN_ERROR("  0x{:08X} from 0x{:08X}: {}", entry->address, entry->secondaryAddress,
                         entry->message);
      } else {
        REXCODEGEN_ERROR("  0x{:08X}: {}", entry->address, entry->message);
      }
    }
  }

  REXCODEGEN_ERROR("Total: {} errors", entries_.size());
}

}  // namespace rex::codegen
