/**
 * @file        graphics/native/edram_ownership.cpp
 * @brief       Native renderer: which host render target holds each EDRAM tile
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              ClaimEdram, TransferOrClear, SetUniform, ForgetUniform), Copyright (c) 2026
 *              OpenPortroid, MIT License (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/edram_ownership.h>

#include <algorithm>
#include <utility>

#include <rex/graphics/xenos.h>

namespace rex::graphics::native {

namespace {

Rect SurfaceRect(const EdramTarget& target) {
  return {0, 0, int32_t(target.surface.width), int32_t(target.surface.height)};
}

// Whether the target's cells can be told by one raw value (clears instead of
// transfers).
bool HasRawCells(const EdramTarget& target) {
  return !target.surface.is_64bpp &&
         (target.depth || IsUniformColorFormat(xenos::ColorRenderTargetFormat(target.format)));
}

// Whether `dst` can take contents from `src` (64bpp cells aren't re-packed:
// same-format 64bpp colour only).
bool CanTransfer(const EdramTarget& src, const EdramTarget& dst) {
  return src.surface.is_64bpp == dst.surface.is_64bpp &&
         (!dst.surface.is_64bpp || (!src.depth && !dst.depth && src.format == dst.format));
}

constexpr uint32_t kAllParts = kEdramPartMain | kEdramPartStencil;

// The rectangle a fast path may remember after `latest` was processed while
// `earlier` still held: their union where it is a rectangle (one contains the
// other, or they share two opposite edges and touch or overlap), else
// `latest`. Draws' claim rectangles end at the rows their vertices reach, so
// they differ from draw to draw in their bottom only: remembering just the
// latest made the next taller draw walk its tiles again.
Rect RememberedRect(const Rect& earlier, const Rect& latest) {
  auto contains = [](const Rect& a, const Rect& b) {
    return b.left >= a.left && b.top >= a.top && b.right <= a.right && b.bottom <= a.bottom;
  };
  if (contains(earlier, latest)) {
    return earlier;
  }
  if (contains(latest, earlier)) {
    return latest;
  }
  if (earlier.left == latest.left && earlier.right == latest.right &&
      latest.top <= earlier.bottom && earlier.top <= latest.bottom) {
    return {earlier.left, std::min(earlier.top, latest.top), earlier.right,
            std::max(earlier.bottom, latest.bottom)};
  }
  if (earlier.top == latest.top && earlier.bottom == latest.bottom &&
      latest.left <= earlier.right && earlier.left <= latest.right) {
    return {std::min(earlier.left, latest.left), earlier.top,
            std::max(earlier.right, latest.right), earlier.bottom};
  }
  return latest;
}

// Copies are kept by the parts of the EDRAM cell (depth bits, stencil byte):
// a colour target's cell is all of them, written, brought in and valid whole.
uint32_t CellParts(const EdramTarget& target, uint32_t parts) {
  return target.depth ? parts : (parts ? kAllParts : 0);
}

}  // namespace

void EdramOwnership::Reset(uint32_t edram_tile_count) {
  owners_.assign(edram_tile_count, nullptr);
  copies_.assign(edram_tile_count, TileCopies{});
  inside_mask_.assign(edram_tile_count, 0);
  ++generation_;
}

void EdramOwnership::Remove(const EdramTarget& target) {
  std::replace(owners_.begin(), owners_.end(), const_cast<EdramTarget*>(&target),
               static_cast<EdramTarget*>(nullptr));
  for (TileCopies& copies : copies_) {
    for (Copy& copy : copies) {
      if (copy.target == &target) {
        copy = Copy();
      }
    }
  }
  ++generation_;
}

uint32_t EdramOwnership::ValidParts(uint32_t tile, const EdramTarget* target) const {
  if (tile >= copies_.size()) {
    return 0;
  }
  for (const Copy& copy : copies_[tile]) {
    if (copy.target == target) {
      return copy.parts;
    }
  }
  return 0;
}

void EdramOwnership::SetCopy(uint32_t tile, EdramTarget* target, uint32_t parts) {
  TileCopies& copies = copies_[tile];
  Copy* free_copy = nullptr;
  for (Copy& copy : copies) {
    if (copy.target == target) {
      copy.parts = parts;
      if (!parts) {
        copy = Copy();
      }
      return;
    }
    if (!copy.target && !free_copy) {
      free_copy = &copy;
    }
  }
  if (!parts) {
    return;
  }
  if (!free_copy) {
    // Full: forget a copy that isn't the owner's (only a transfer saved less).
    for (Copy& copy : copies) {
      if (copy.target != owners_[tile]) {
        free_copy = &copy;
        break;
      }
    }
  }
  if (free_copy) {
    if (free_copy->target && free_copy->target != target) {
      // Its copy is forgotten.
      ++free_copy->target->epoch;
    }
    free_copy->target = target;
    free_copy->parts = parts;
  }
}

uint32_t EdramOwnership::HeldParts(uint32_t tile) const {
  uint32_t parts = 0;
  for (const Copy& copy : copies_[tile]) {
    parts |= copy.parts;
  }
  return parts;
}

EdramTarget* EdramOwnership::CopyWith(uint32_t tile, uint32_t parts, const EdramTarget* exclude,
                                      const EdramTarget& dst) const {
  for (const Copy& copy : copies_[tile]) {
    if (copy.target && copy.target != exclude && (copy.parts & parts) == parts &&
        CanTransfer(*copy.target, dst)) {
      return copy.target;
    }
  }
  return nullptr;
}

void EdramOwnership::Written(EdramTarget& target, const Rect& rect, uint32_t parts) {
  const uint32_t edram_tile_count = tile_count();
  const uint32_t cell_parts = CellParts(target, parts);
  // Nothing changed hands since a write of these parts over a rectangle
  // containing this one: the copies are as that write left them, and its
  // uniform tiles there are already forgotten (Cleared, the only other
  // setter outside claims, starts the next write over).
  const bool written_current =
      target.written_generation == generation_ && target.written_epoch == target.epoch;
  if (cell_parts && edram_tile_count && written_current &&
      !(cell_parts & ~target.written_parts) && rect.left >= target.written.left &&
      rect.top >= target.written.top && rect.right <= target.written.right &&
      rect.bottom <= target.written.bottom) {
    return;
  }
  if (parts & kEdramPartMain) {
    target.uniform.Forget(0, target.surface, rect, edram_tile_count);
  }
  if (target.depth && (parts & kEdramPartStencil)) {
    target.uniform.Forget(1, target.surface, rect, edram_tile_count);
  }
  if (!cell_parts || !edram_tile_count) {
    return;
  }
  ++walk_counts_.writes;
  ForEachTile(target.surface, rect, edram_tile_count, [&](uint32_t tile) {
    ++walk_counts_.write_tiles;
    uint32_t own_parts = cell_parts;
    for (Copy& copy : copies_[tile]) {
      if (!copy.target) {
        continue;
      }
      if (copy.target == &target) {
        own_parts |= copy.parts;
        continue;
      }
      // A colour copy is valid whole or not at all.
      const uint32_t parts_left = copy.target->depth
                                      ? copy.parts & ~cell_parts
                                      : ((copy.parts & cell_parts) ? 0 : copy.parts);
      if (parts_left != copy.parts) {
        ++copy.target->epoch;
        copy.parts = parts_left;
      }
      if (!copy.parts) {
        copy = Copy();
      }
    }
    SetCopy(tile, &target, own_parts);
  });
  // Nothing changed hands since an earlier write of the same parts: both
  // rectangles are as the writes left them.
  target.written = written_current && target.written_parts == cell_parts
                       ? RememberedRect(target.written, rect)
                       : rect;
  target.written_generation = generation_;
  target.written_epoch = target.epoch;
  target.written_parts = cell_parts;
}

void EdramOwnership::Cleared(EdramTarget& target, const Rect& rect, uint32_t parts,
                             uint32_t raw) {
  // Partly covered tiles hold mixed values: only whole ones become uniform.
  Written(target, rect, parts);
  // The uniform tiles set below: the next write must forget them, not take
  // Written's fast path.
  target.written_generation = UINT64_MAX;
  if (!HasRawCells(target)) {
    return;
  }
  TilesInside(target.surface, rect, tile_count(), inside_);
  if (inside_.empty()) {
    return;
  }
  if (!target.depth) {
    if (parts & kEdramPartMain) {
      target.uniform.Set(0, inside_, raw);
    }
    return;
  }
  if (parts & kEdramPartMain) {
    target.uniform.Set(0, inside_, raw & ~uint32_t(0xFF));
  }
  if (parts & kEdramPartStencil) {
    target.uniform.Set(1, inside_, raw & 0xFF);
  }
}

bool EdramOwnership::ClaimIsKept(const EdramTarget& target, const Rect& rect_unclipped) const {
  if (!tile_count()) {
    return true;
  }
  Rect rect = rect_unclipped;
  rect.left = std::max(rect.left, int32_t(0));
  rect.top = std::max(rect.top, int32_t(0));
  rect.right = std::min(rect.right, int32_t(target.surface.width));
  rect.bottom = std::min(rect.bottom, int32_t(target.surface.height));
  return rect.empty() ||
         (target.claim_generation == generation_ && target.claim_epoch == target.epoch &&
          rect.left >= target.claimed.left &&
          rect.top >= target.claimed.top && rect.right <= target.claimed.right &&
          rect.bottom <= target.claimed.bottom);
}

void EdramOwnership::Claim(EdramTarget& target, const Rect& rect_unclipped, uint32_t overwrite,
                           std::vector<EdramOp>& ops_out) {
  const uint32_t edram_tile_count = tile_count();
  if (!edram_tile_count) {
    return;
  }
  Rect rect = rect_unclipped;
  rect.left = std::max(rect.left, int32_t(0));
  rect.top = std::max(rect.top, int32_t(0));
  rect.right = std::min(rect.right, int32_t(target.surface.width));
  rect.bottom = std::min(rect.bottom, int32_t(target.surface.height));
  if (rect.empty()) {
    return;
  }
  // The target still owns what it claimed last (none of its tiles changed
  // hands since).
  const bool claimed_current =
      target.claim_generation == generation_ && target.claim_epoch == target.epoch;
  // Within a rectangle it claimed: nothing to do.
  if (claimed_current && rect.left >= target.claimed.left && rect.top >= target.claimed.top &&
      rect.right <= target.claimed.right && rect.bottom <= target.claimed.bottom) {
    return;
  }
  ++walk_counts_.claims;

  uint32_t needed =
      target.depth ? (target.stencil_used ? kEdramPartMain | kEdramPartStencil : kEdramPartMain)
                   : kEdramPartMain;
  // The tiles the rectangle covers completely.
  if (overwrite) {
    TilesInside(target.surface, rect, edram_tile_count, inside_);
    for (uint32_t tile : inside_) {
      inside_mask_[tile] = 1;
    }
  }
  // The parts a tile needs: an edge tile changes hands whole, so all of it
  // comes over, not only its part inside the rectangle (AoT: a shadow map's
  // border cleared through a 4x MSAA surface, then claimed by a draw over the
  // inset viewport only, kept stale depth along its edges).
  auto tile_needed = [&](uint32_t tile) {
    return CellParts(target, inside_mask_[tile] ? needed & ~overwrite : needed);
  };
  // Parts of a tile no target holds validly (a depth-only surface's stencil
  // nobody wrote since): no transfer makes them right, the target's own are
  // as good as any.
  auto unheld = [&](uint32_t tile) { return kAllParts & ~HeldParts(tile); };

  // The tiles changing hands and where each takes its contents from: the
  // owner, or (copies) nothing where the target is still valid, or another
  // valid copy where the owner isn't valid in what is needed. The sources in
  // the order their tiles come.
  std::vector<EdramTarget*> sources;
  claim_tiles_.clear();
  claim_sources_.clear();
  ForEachTile(target.surface, rect, edram_tile_count, [&](uint32_t tile) {
    ++walk_counts_.claim_tiles;
    EdramTarget* owner = owners_[tile];
    if (owner == &target) {
      return;
    }
    EdramTarget* source = owner;
    if (owner && use_copies_) {
      const uint32_t parts = tile_needed(tile);
      const uint32_t free_parts = unheld(tile);
      const uint32_t missing = parts & ~(ValidParts(tile, &target) | free_parts);
      if (!missing) {
        copy_kept_tiles_ += parts != 0;
        source = nullptr;
      } else if (((ValidParts(tile, owner) | free_parts) & missing) != missing) {
        if (EdramTarget* other = CopyWith(tile, missing, &target, target)) {
          source = other;
        }
      }
    }
    claim_tiles_.push_back(tile);
    claim_sources_.push_back(source);
    if (source && std::find(sources.begin(), sources.end(), source) == sources.end()) {
      sources.push_back(source);
    }
  });

  const Rect surface_rect = SurfaceRect(target);
  for (EdramTarget* source : sources) {
    if (!CanTransfer(*source, target)) {
      continue;
    }
    interior_tiles_.clear();
    edge_tiles_.clear();
    for (size_t i = 0; i < claim_tiles_.size(); ++i) {
      if (claim_sources_[i] == source) {
        (inside_mask_[claim_tiles_[i]] ? interior_tiles_ : edge_tiles_).push_back(claim_tiles_[i]);
      }
    }
    TransferOrClear(target, *source, interior_tiles_, rect, needed & ~overwrite, ops_out);
    TransferOrClear(target, *source, edge_tiles_, surface_rect, needed, ops_out);
  }

  if (!claim_tiles_.empty()) {
    for (size_t i = 0; i < claim_tiles_.size(); ++i) {
      const uint32_t tile = claim_tiles_[i];
      EdramTarget* source = claim_sources_[i];
      // The previous owner and the copy holders of the tile: their fast paths
      // end.
      if (owners_[tile]) {
        ++owners_[tile]->epoch;
      }
      for (const Copy& copy : copies_[tile]) {
        if (copy.target) {
          ++copy.target->epoch;
        }
      }
      if (!owners_[tile]) {
        // Nobody held it: the target's image is what the tile holds.
        copies_[tile] = TileCopies{};
        SetCopy(tile, &target, kAllParts);
      } else {
        // The previous owner and other copies stay valid (their images are
        // unchanged); the target is valid in what it held, what it got from
        // a valid source, and what nobody holds.
        const uint32_t free_parts = unheld(tile);
        uint32_t parts = ValidParts(tile, &target);
        if (!source) {
          parts |= tile_needed(tile) & free_parts;
        } else if (CanTransfer(*source, target)) {
          parts |= tile_needed(tile) & (ValidParts(tile, source) | free_parts);
        }
        if (!target.depth && parts != kAllParts) {
          parts = 0;
        }
        SetCopy(tile, &target, parts);
      }
      owners_[tile] = &target;
    }
    // Its own tiles and copies changed too (its written record no longer
    // holds; its claim stamp is set below).
    ++target.epoch;
  }
  if (overwrite) {
    for (uint32_t tile : inside_) {
      inside_mask_[tile] = 0;
    }
  }
  // No tile changed hands in this claim: the target owns both rectangles.
  target.claimed = claimed_current && claim_tiles_.empty()
                       ? RememberedRect(target.claimed, rect)
                       : rect;
  target.claim_generation = generation_;
  target.claim_epoch = target.epoch;
}

void EdramOwnership::TransferOrClear(EdramTarget& dst, const EdramTarget& src,
                                     const std::vector<uint32_t>& tiles, const Rect& clip,
                                     uint32_t needed, std::vector<EdramOp>& ops_out) {
  if (tiles.empty() || !needed) {
    return;
  }
  const uint32_t edram_tile_count = tile_count();
  const bool can_clear = HasRawCells(src) && HasRawCells(dst);
  // Known bits of the source's raw cells: 8..31 (colour / depth) and 0..7
  // (stencil); a colour cell is known whole or not at all.
  auto high_known = [&](uint32_t tile) { return src.uniform.Has(0, tile); };
  auto low_known = [&](uint32_t tile) { return src.uniform.Has(src.depth ? 1 : 0, tile); };
  // A colour destination takes the whole cell.
  const bool want_high = !dst.depth || (needed & kEdramPartMain);
  const bool want_low = !dst.depth || (needed & kEdramPartStencil);
  clear_tiles_.clear();
  pass_tiles_.clear();
  for (uint32_t tile : tiles) {
    bool known = can_clear && (!want_high || high_known(tile)) && (!want_low || low_known(tile));
    (known ? clear_tiles_ : pass_tiles_).push_back(tile);
  }
  const uint32_t raw = src.depth ? ((src.uniform.value(0) & ~uint32_t(0xFF)) |
                                    (src.uniform.value(1) & 0xFF))
                                 : src.uniform.value(0);

  if (!clear_tiles_.empty()) {
    EdramOp op;
    op.kind = EdramOp::Kind::kClear;
    op.dst = &dst;
    op.src = &src;
    op.parts = dst.depth ? needed : kEdramPartMain;
    op.raw = raw;
    TileRects(dst.surface, clear_tiles_, clip, edram_tile_count, op.rects);
    if (!dst.depth) {
      dst.uniform.Set(0, clear_tiles_, raw);
    } else {
      if (needed & kEdramPartMain) {
        dst.uniform.Set(0, clear_tiles_, raw & ~uint32_t(0xFF));
      }
      if (needed & kEdramPartStencil) {
        dst.uniform.Set(1, clear_tiles_, raw & 0xFF);
      }
    }
    if (!op.rects.empty()) {
      ops_out.push_back(std::move(op));
    }
  }
  if (pass_tiles_.empty()) {
    return;
  }

  // The rest: a transfer over these tiles only.
  EdramOp transfer;
  transfer.kind = EdramOp::Kind::kTransfer;
  transfer.dst = &dst;
  transfer.src = &src;
  TileRects(dst.surface, pass_tiles_, clip, edram_tile_count, transfer.rects);
  if (transfer.rects.empty()) {
    return;
  }
  if (!dst.depth) {
    transfer.parts = kEdramPartMain;
    for (uint32_t tile : pass_tiles_) {
      dst.uniform.Forget(0, tile);
    }
    ops_out.push_back(std::move(transfer));
    return;
  }
  // A depth destination: the parts uniform in all of these tiles are cleared
  // instead.
  bool high_all = can_clear;
  bool low_all = can_clear;
  for (uint32_t tile : pass_tiles_) {
    high_all = high_all && high_known(tile);
    low_all = low_all && low_known(tile);
  }
  uint32_t pass_parts = 0;
  uint32_t clear_parts = 0;
  if (needed & kEdramPartMain) {
    (high_all ? clear_parts : pass_parts) |= kEdramPartMain;
  }
  if (needed & kEdramPartStencil) {
    (low_all ? clear_parts : pass_parts) |= kEdramPartStencil;
  }
  std::vector<Rect> rects = transfer.rects;
  if (pass_parts) {
    transfer.parts = pass_parts;
    ops_out.push_back(std::move(transfer));
  }
  if (clear_parts) {
    EdramOp op;
    op.kind = EdramOp::Kind::kClear;
    op.dst = &dst;
    op.src = &src;
    op.parts = clear_parts;
    op.raw = raw;
    op.rects = std::move(rects);
    ops_out.push_back(std::move(op));
  }
  for (uint32_t component = 0; component < 2; ++component) {
    uint32_t part = component ? kEdramPartStencil : kEdramPartMain;
    if (!(needed & part)) {
      continue;
    }
    if (clear_parts & part) {
      dst.uniform.Set(component, pass_tiles_,
                      component ? (raw & 0xFF) : (raw & ~uint32_t(0xFF)));
    } else {
      for (uint32_t tile : pass_tiles_) {
        dst.uniform.Forget(component, tile);
      }
    }
  }
}

}  // namespace rex::graphics::native
