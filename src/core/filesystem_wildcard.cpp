/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>

#include <rex/assert.h>
#include <rex/filesystem/wildcard.h>
#include <rex/string.h>

namespace rex::filesystem {

WildcardFlags WildcardFlags::FIRST(true, false, false);
WildcardFlags WildcardFlags::LAST(false, true, false);
WildcardFlags WildcardFlags::ANY(false, false, true);
WildcardFlags WildcardFlags::FIRST_AND_LAST(true, true, false);

WildcardFlags::WildcardFlags() : FromStart(false), ToEnd(false), ExactLength(false) {}

WildcardFlags::WildcardFlags(bool start, bool end, bool exact_length)
    : FromStart(start), ToEnd(end), ExactLength(exact_length) {}

WildcardRule::WildcardRule(const std::string_view match, const WildcardFlags& flags)
    : match_(rex::string::utf8_lower_ascii(match)), rules_(flags) {}

bool WildcardRule::Check(const std::string_view lower, std::string::size_type* offset) const {
  if (match_.empty()) {
    return true;
  }

  if ((lower.size() - *offset) < match_.size()) {
    return false;
  }

  if (rules_.ExactLength) {
    *offset += match_.size();
    return true;
  }

  std::string_view::size_type result(lower.find(match_, *offset));

  if (result != std::string_view::npos) {
    if (rules_.FromStart && result != *offset) {
      return false;
    }

    if (rules_.ToEnd && result != (lower.size() - match_.size())) {
      return false;
    }

    *offset = (result + match_.size());
    return true;
  }

  return false;
}

// Matching model from rexglue/rexglue-sdk 32d2c0c8 (Tom Clay, BSD 3-Clause).
// Copyright (c) 2026 Tom Clay. All rights reserved.
void WildcardEngine::PreparePattern(const std::string_view pattern) {
  pattern_ = rex::string::utf8_lower_ascii(pattern == "*.*" ? "*" : pattern);
}

void WildcardEngine::SetRule(const std::string_view pattern) {
  PreparePattern(pattern);
}

bool WildcardEngine::Match(const std::string_view name) const {
  if (pattern_.empty()) {
    return true;
  }
  const std::string lower = rex::string::utf8_lower_ascii(name);
  const size_t last_dot = lower.rfind('.');
  const size_t count = pattern_.size();
  std::vector<bool> active(count + 1);
  std::vector<bool> next(count + 1);
  active[0] = true;
  for (size_t n = 0;; ++n) {
    const bool at_end = n == lower.size();
    const char c = at_end ? '\0' : lower[n];
    for (size_t p = 0; p < count; ++p) {
      const char token = pattern_[p];
      if (active[p] && (token == '*' || token == '<' || (token == '>' && (at_end || c == '.')) ||
                        (token == '"' && at_end))) {
        active[p + 1] = true;
      }
    }
    if (at_end) {
      return active[count];
    }
    std::ranges::fill(next, false);
    for (size_t p = 0; p < count; ++p) {
      if (!active[p]) {
        continue;
      }
      switch (pattern_[p]) {
        case '*':
          next[p] = true;
          break;
        case '<':
          next[p] = next[p] || c != '.' || n != last_dot;
          break;
        case '?':
          next[p + 1] = true;
          break;
        case '>':
          next[p + 1] = next[p + 1] || c != '.';
          break;
        case '"':
          next[p + 1] = next[p + 1] || c == '.';
          break;
        default:
          next[p + 1] = next[p + 1] || pattern_[p] == c;
          break;
      }
    }
    std::swap(active, next);
  }
}

}  // namespace rex::filesystem
