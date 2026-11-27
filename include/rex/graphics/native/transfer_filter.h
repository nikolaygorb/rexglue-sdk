/**
 * @file        rex/graphics/native/transfer_filter.h
 * @brief       native_drop_transfers: which EDRAM ownership transfers a debug
 *              run leaves out, to find the ones a title needs
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace rex::graphics::native {

// A list of rules "source>destination", separated by commas or spaces. A side
// is "*" (anything) or C (colour) / D (depth), optionally followed by the
// target's format number (xenos::ColorRenderTargetFormat or
// DepthRenderTargetFormat) and "@" with its sample count (1, 2 or 4):
// "D>D@4", "C6>D", "D0@1>D0@4", "*>C". A transfer that matches any rule is
// left out, so the destination keeps what it had. The idea is gow2's
// gpu_native_keep_transfers (sdk 0094), used there to find the one transfer a
// title's motion blur reads; here nothing is dropped by default.
struct TransferSide {
  bool any = true;
  bool depth = false;
  // UINT32_MAX: any format.
  uint32_t format = UINT32_MAX;
  // 0: any sample count.
  uint32_t samples = 0;

  bool Matches(bool target_depth, uint32_t target_format, uint32_t target_samples) const {
    if (any) {
      return true;
    }
    return depth == target_depth && (format == UINT32_MAX || format == target_format) &&
           (samples == 0 || samples == target_samples);
  }
};

struct TransferRule {
  TransferSide source;
  TransferSide destination;
};

struct TransferFilter {
  std::vector<TransferRule> rules;

  bool empty() const { return rules.empty(); }

  bool Drops(bool src_depth, uint32_t src_format, uint32_t src_samples, bool dst_depth,
             uint32_t dst_format, uint32_t dst_samples) const {
    for (const TransferRule& rule : rules) {
      if (rule.source.Matches(src_depth, src_format, src_samples) &&
          rule.destination.Matches(dst_depth, dst_format, dst_samples)) {
        return true;
      }
    }
    return false;
  }
};

namespace transfer_filter_detail {

inline bool IsSeparator(char c) {
  return c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline bool ParseNumber(std::string_view text, uint32_t limit, uint32_t& out) {
  if (text.empty()) {
    return false;
  }
  uint32_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + uint32_t(c - '0');
    if (value > limit) {
      return false;
    }
  }
  out = value;
  return true;
}

inline bool ParseSide(std::string_view text, TransferSide& out) {
  if (text == "*") {
    out = TransferSide{};
    return true;
  }
  if (text.empty()) {
    return false;
  }
  TransferSide side;
  side.any = false;
  if (text[0] == 'C' || text[0] == 'c') {
    side.depth = false;
  } else if (text[0] == 'D' || text[0] == 'd') {
    side.depth = true;
  } else {
    return false;
  }
  text.remove_prefix(1);
  const size_t at = text.find('@');
  const std::string_view format_text = text.substr(0, at);
  if (!format_text.empty() && !ParseNumber(format_text, 63, side.format)) {
    return false;
  }
  if (at != std::string_view::npos) {
    uint32_t samples = 0;
    if (!ParseNumber(text.substr(at + 1), 4, samples) ||
        (samples != 1 && samples != 2 && samples != 4)) {
      return false;
    }
    side.samples = samples;
  }
  out = side;
  return true;
}

}  // namespace transfer_filter_detail

// The filter of a native_drop_transfers value; nullopt if any rule is malformed
// (the caller then drops nothing). An empty or blank value: no rules.
inline std::optional<TransferFilter> ParseTransferFilter(std::string_view text) {
  using namespace transfer_filter_detail;
  TransferFilter filter;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && IsSeparator(text[i])) {
      ++i;
    }
    if (i >= text.size()) {
      break;
    }
    const size_t start = i;
    while (i < text.size() && !IsSeparator(text[i])) {
      ++i;
    }
    const std::string_view token = text.substr(start, i - start);
    const size_t arrow = token.find('>');
    if (arrow == std::string_view::npos || token.find('>', arrow + 1) != std::string_view::npos) {
      return std::nullopt;
    }
    TransferRule rule;
    if (!ParseSide(token.substr(0, arrow), rule.source) ||
        !ParseSide(token.substr(arrow + 1), rule.destination)) {
      return std::nullopt;
    }
    filter.rules.push_back(rule);
  }
  return filter;
}

}  // namespace rex::graphics::native
