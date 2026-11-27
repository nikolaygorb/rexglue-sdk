#pragma once
/**
 * @file        rex/graphics/vulkan/render_area.h
 * @brief       The render area a Vulkan render pass needs: the union of the
 *              scissors in effect at its draws and its clear rectangles
 *
 * @copyright   Copyright (c) 2026, Santiago Quiroz Upegui and the gow2-recomp
 *              contributors. All rights reserved.
 *              Ported for the ReXGlue fork from gears-of-war-2-pc-port
 *              sdk/patches/0089 (src/graphics/vulkan/render_area.h).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdint>

#include <rex/ui/vulkan/api.h>

namespace rex::graphics::vulkan::render_area {

// Smallest rect holding both; an empty rect adds nothing.
inline VkRect2D UnionRect(const VkRect2D& a, const VkRect2D& b) {
  int64_t ax = static_cast<int64_t>(a.offset.x);
  int64_t ay = static_cast<int64_t>(a.offset.y);
  int64_t aw = static_cast<int64_t>(a.extent.width);
  int64_t ah = static_cast<int64_t>(a.extent.height);
  int64_t bx = static_cast<int64_t>(b.offset.x);
  int64_t by = static_cast<int64_t>(b.offset.y);
  int64_t bw = static_cast<int64_t>(b.extent.width);
  int64_t bh = static_cast<int64_t>(b.extent.height);

  if (aw == 0 || ah == 0) {
    return b;
  }
  if (bw == 0 || bh == 0) {
    return a;
  }

  int64_t x = std::min(ax, bx);
  int64_t y = std::min(ay, by);
  int64_t right = std::max(ax + aw, bx + bw);
  int64_t bottom = std::max(ay + ah, by + bh);

  VkRect2D result = {};
  result.offset.x = static_cast<int32_t>(x);
  result.offset.y = static_cast<int32_t>(y);
  result.extent.width = static_cast<uint32_t>(right - x);
  result.extent.height = static_cast<uint32_t>(bottom - y);
  return result;
}

// What one render pass can write, gathered while walking its commands.
struct PassArea {
  VkRect2D covered = {};   // Union so far; empty = nothing yet.
  bool unbounded = false;  // A draw ran with no known scissor, or the pass
                           // clears or resolves its attachments.
};

inline void AddDraw(PassArea& area, const VkRect2D* scissor) {
  if (!scissor) {
    area.unbounded = true;
    return;
  }
  area.covered = UnionRect(area.covered, *scissor);
}

inline void AddClearRect(PassArea& area, const VkRect2D& rect) {
  area.covered = UnionRect(area.covered, rect);
}

inline VkRect2D OnePixelAt(const VkOffset2D& offset) {
  VkRect2D r = {};
  r.offset = offset;
  r.extent.width = 1;
  r.extent.height = 1;
  return r;
}

// The render area to begin the pass with, inside `full` (the area the pass
// was recorded with): unbounded -> full; nothing covered -> 1x1 at
// full.offset; otherwise covered intersected with full, and 1x1 at full.offset
// when that intersection is empty.
inline VkRect2D TightRenderArea(const PassArea& area, const VkRect2D& full) {
  if (area.unbounded) {
    return full;
  }

  VkRect2D covered = area.covered;
  int64_t cw = static_cast<int64_t>(covered.extent.width);
  int64_t ch = static_cast<int64_t>(covered.extent.height);

  if (cw == 0 || ch == 0) {
    return OnePixelAt(full.offset);
  }

  int64_t fx = static_cast<int64_t>(full.offset.x);
  int64_t fy = static_cast<int64_t>(full.offset.y);
  int64_t fw = static_cast<int64_t>(full.extent.width);
  int64_t fh = static_cast<int64_t>(full.extent.height);

  int64_t x = std::max(static_cast<int64_t>(covered.offset.x), fx);
  int64_t y = std::max(static_cast<int64_t>(covered.offset.y), fy);
  int64_t right = std::min(static_cast<int64_t>(covered.offset.x) + cw, fx + fw);
  int64_t bottom = std::min(static_cast<int64_t>(covered.offset.y) + ch, fy + fh);

  if (right <= x || bottom <= y) {
    return OnePixelAt(full.offset);
  }

  VkRect2D result = {};
  result.offset.x = static_cast<int32_t>(x);
  result.offset.y = static_cast<int32_t>(y);
  result.extent.width = static_cast<uint32_t>(right - x);
  result.extent.height = static_cast<uint32_t>(bottom - y);
  return result;
}

}  // namespace rex::graphics::vulkan::render_area
