/**
 * @file        graphics/native/edram_ownership.h
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

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <rex/graphics/native/edram_tiles.h>
#include <rex/graphics/native/scale.h>

namespace rex::graphics::native {

// What a host render target is to the EDRAM bookkeeping (RenderTarget derives
// from it): its surface, kind and format, which of its tiles hold a known
// uniform value, and its last claim.
struct EdramTarget {
  EdramSurface surface;
  bool depth = false;
  // Colour: the key xenos::ColorRenderTargetFormat; depth: the
  // xenos::DepthRenderTargetFormat.
  uint32_t format = 0;
  // A draw has tested or written its stencil: claims bring the stencil in too.
  bool stencil_used = false;
  // Component 0: the colour cell, or the depth bits (the stencil byte zero);
  // 1: the stencil byte. Reset to the EDRAM's tile count by the creator.
  UniformTiles uniform;
  // Changes to this target's tiles by others (EdramOwnership): one is
  // counted when another target takes tiles it owned or held a copy of, or a
  // write makes its copy of a tile invalid. The fast paths below hold while
  // it (and the ownership's generation) stay as they were: claims of other
  // tiles don't end them.
  uint64_t epoch = 0;
  // The last claim (EdramOwnership::Claim's fast path).
  uint64_t claim_generation = UINT64_MAX;
  uint64_t claim_epoch = UINT64_MAX;
  Rect claimed;
  // The last Written with copies to update (EdramOwnership::Written's fast
  // path).
  uint64_t written_generation = UINT64_MAX;
  uint64_t written_epoch = UINT64_MAX;
  Rect written;
  uint32_t written_parts = 0;
};

// Parts of a cell a claim brings in or a draw overwrites.
enum EdramPart : uint32_t {
  // Colour, or depth.
  kEdramPartMain = 1u << 0,
  kEdramPartStencil = 1u << 1,
};

// What the renderer does for a claim, in order, outside its draws.
struct EdramOp {
  enum class Kind {
    // `dst`'s rectangles set to `raw` in `parts` (a source tile known to hold
    // one value: no transfer needed).
    kClear,
    // `src`'s contents copied into `dst` in `rects` (each tile of them owned
    // by `src` until the claim), reinterpreted as the EDRAM would: the raw
    // 32-bit cells; 64bpp only between surfaces of the same format.
    kTransfer,
  };
  Kind kind = Kind::kTransfer;
  EdramTarget* dst = nullptr;
  const EdramTarget* src = nullptr;
  uint32_t parts = 0;
  // kClear: the raw cell, the source's (colour: its 32-bit cell; depth:
  // depth24 << 8 | stencil), to be read in `dst`'s format.
  uint32_t raw = 0;
  // Guest pixels of `dst`.
  std::vector<Rect> rects;
};

// The owner of each EDRAM tile. A target claiming tiles gets what their
// previous owners left there (ops), except what it overwrites anyway; 64bpp
// surfaces exchange contents only with 64bpp surfaces of the same format, the
// rest only change hands. Command processor thread only; the targets outlive
// their ownership (Remove).
//
// Valid copies: per tile, the targets whose image still holds what the EDRAM
// holds there, by part of the cell (depth bits, stencil byte; a colour target
// covers the whole cell, valid whole or not at all). A transfer leaves its
// source valid, and the destination valid in the parts it brought; a write
// (Written, Cleared) leaves only the writer valid in the parts written; parts
// no target holds count as valid in any (no transfer makes them right). With SetUseCopies, a claim of a
// tile the target is still valid in needs no transfer: a surface aliasing
// another one's tiles that is only read (resolved, depth tested without
// writes) gives them back for free (RDR: its shadow maps drawn at 400x400
// 4x MSAA and resolved through 800x800 1x, its 1280x720 2x depth used as
// 640x720 4x for half-resolution passes). Parts a claim needs that the owner
// doesn't hold validly come from another valid copy where there is one.
class EdramOwnership {
 public:
  void Reset(uint32_t edram_tile_count);
  uint32_t tile_count() const { return uint32_t(owners_.size()); }

  // Whether claims skip tiles the target holds a valid copy of (the copies
  // are kept either way).
  void SetUseCopies(bool use) { use_copies_ = use; }
  // The parts of `tile` the target's image holds validly (EdramPart; both
  // for a valid colour target).
  uint32_t ValidParts(uint32_t tile, const EdramTarget* target) const;
  // Tiles claims found valid in their target so far (no transfer).
  uint64_t copy_kept_tiles() const { return copy_kept_tiles_; }
  // Claims and writes that took the slow path (walked their tiles), and the
  // tiles they walked, so far (frame_stats_interval with
  // native_profile_transfers).
  struct WalkCounts {
    uint64_t claims = 0, claim_tiles = 0, writes = 0, write_tiles = 0;
  };
  const WalkCounts& walk_counts() const { return walk_counts_; }

  // Makes `target` the owner of the tiles `rect` (guest pixels, clipped to its
  // surface) touches, first appending the ops bringing in their previous
  // owners' contents. `overwrite` (EdramPart): what the caller writes over
  // all of `rect` (tiles it covers completely skip those parts; tiles it
  // covers partly come over whole, as they change hands whole). A depth
  // target brings its stencil in only once stencil_used.
  void Claim(EdramTarget& target, const Rect& rect, uint32_t overwrite,
             std::vector<EdramOp>& ops_out);
  // Whether a claim of `rect` would do nothing: no change of hands since the
  // target claimed a rectangle containing it (clipped to its surface) or the
  // rectangle is empty there. Claim's own fast path, for callers to skip
  // their work around it.
  bool ClaimIsKept(const EdramTarget& target, const Rect& rect) const;

  // A draw wrote `parts` of `rect` (the target owns those tiles): its uniform
  // tiles there are forgotten, and other targets' copies of those parts are
  // no longer valid.
  void Written(EdramTarget& target, const Rect& rect, uint32_t parts);
  // A clear of `parts` of `rect` to `raw` (as EdramOp::raw): the tiles it
  // covers completely become uniform (not for 64bpp surfaces or colour
  // formats without raw clears), the rest forgotten.
  void Cleared(EdramTarget& target, const Rect& rect, uint32_t parts, uint32_t raw);

  // The target is going away: its tiles have no owner.
  void Remove(const EdramTarget& target);

  const EdramTarget* Owner(uint32_t tile) const {
    return tile < owners_.size() ? owners_[tile] : nullptr;
  }
  // Resets and removals so far (with each target's epoch, the fast paths
  // compare it).
  uint64_t generation() const { return generation_; }

 private:
  void TransferOrClear(EdramTarget& dst, const EdramTarget& src,
                       const std::vector<uint32_t>& tiles, const Rect& clip, uint32_t needed,
                       std::vector<EdramOp>& ops_out);

  struct Copy {
    EdramTarget* target = nullptr;
    uint32_t parts = 0;
  };
  // RDR aliases ~10 surfaces over EDRAM tile 0 in a frame.
  static constexpr uint32_t kCopiesPerTile = 8;
  using TileCopies = std::array<Copy, kCopiesPerTile>;
  // Sets the target's valid parts of the tile (0 removes it); when the tile
  // has no room, a copy of a target other than the owner and this one goes.
  void SetCopy(uint32_t tile, EdramTarget* target, uint32_t parts);
  // The parts of the tile some target holds validly.
  uint32_t HeldParts(uint32_t tile) const;
  // Another target than `exclude` valid in all of `parts` that `dst` can
  // take contents from, or null.
  EdramTarget* CopyWith(uint32_t tile, uint32_t parts, const EdramTarget* exclude,
                        const EdramTarget& dst) const;

  std::vector<EdramTarget*> owners_;
  std::vector<TileCopies> copies_;
  bool use_copies_ = false;
  uint64_t copy_kept_tiles_ = 0;
  WalkCounts walk_counts_;
  uint64_t generation_ = 0;
  // Claim scratch: the tiles changing hands and the target each takes its
  // contents from (null: none needed).
  std::vector<uint32_t> claim_tiles_;
  std::vector<EdramTarget*> claim_sources_;
  // Scratch.
  std::vector<uint32_t> inside_;
  std::vector<uint8_t> inside_mask_;
  std::vector<uint32_t> interior_tiles_;
  std::vector<uint32_t> edge_tiles_;
  std::vector<uint32_t> clear_tiles_;
  std::vector<uint32_t> pass_tiles_;
};

}  // namespace rex::graphics::native
