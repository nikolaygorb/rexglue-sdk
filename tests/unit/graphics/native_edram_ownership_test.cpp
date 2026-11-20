/**
 * @file        graphics/native_edram_ownership_test.cpp
 * @brief       Tests for the native renderer's EDRAM tile ownership and the ops of its claims
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/native/edram_ownership.h>
#include <rex/graphics/xenos.h>

using namespace rex::graphics;
using namespace rex::graphics::native;

namespace {

constexpr uint32_t kTiles = xenos::kEdramTileCount;
constexpr uint32_t kColor8888 = uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
constexpr uint32_t kColor16x4 = uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16);
constexpr uint32_t kColor16x4Float =
    uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT);
constexpr uint32_t kDepth24S8 = uint32_t(xenos::DepthRenderTargetFormat::kD24S8);

// 1280 pixels: 16 tiles of 80x16 per row (32 of 40x16 for 64bpp).
EdramTarget Target(uint32_t base, bool depth = false, uint32_t format = kColor8888,
                   bool is_64bpp = false) {
  EdramTarget target;
  target.surface.base_tiles = base;
  target.surface.pitch_pixels = 1280;
  target.surface.width = 1280;
  target.surface.height = 720;
  target.surface.is_64bpp = is_64bpp;
  target.depth = depth;
  target.format = depth && format == kColor8888 ? kDepth24S8 : format;
  target.uniform.Reset(kTiles);
  return target;
}

EdramOwnership Ownership() {
  EdramOwnership ownership;
  ownership.Reset(kTiles);
  return ownership;
}

}  // namespace

TEST_CASE("Native EDRAM claims of free tiles need nothing", "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  CHECK(ops.empty());
  CHECK(ownership.Owner(0) == &a);
  CHECK(ownership.Owner(1) == &a);
  CHECK(ownership.Owner(2) == nullptr);
  CHECK(ownership.Owner(16) == nullptr);
}

TEST_CASE("Native EDRAM claims transfer whole tiles from the previous owner",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  EdramTarget b = Target(0, true);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  // Part of tile 0: all of it comes over.
  ownership.Claim(b, {10, 2, 20, 4}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].kind == EdramOp::Kind::kTransfer);
  CHECK(ops[0].dst == &b);
  CHECK(ops[0].src == &a);
  CHECK(ops[0].parts == kEdramPartMain);
  REQUIRE(ops[0].rects.size() == 1);
  CHECK(ops[0].rects[0] == Rect{0, 0, 80, 16});
  CHECK(ownership.Owner(0) == &b);
  CHECK(ownership.Owner(1) == &a);
}

TEST_CASE("Native EDRAM claims skip what the caller overwrites", "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  EdramTarget c = Target(0);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 160, 32}, 0, ops);

  // All four tiles covered: nothing comes over.
  ownership.Claim(c, {0, 0, 160, 32}, kEdramPartMain, ops);
  CHECK(ops.empty());
  CHECK(ownership.Owner(17) == &c);

  // Columns 0 covered, 1 partly: column 1 comes over whole, rows merged.
  ownership.Claim(a, {0, 0, 160, 32}, 0, ops);
  ops.clear();
  ownership.Claim(c, {0, 0, 120, 32}, kEdramPartMain, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].kind == EdramOp::Kind::kTransfer);
  REQUIRE(ops[0].rects.size() == 1);
  CHECK(ops[0].rects[0] == Rect{80, 0, 160, 32});
}

TEST_CASE("Native EDRAM claims of uniform tiles are clears", "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  EdramTarget d = Target(0);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  ownership.Cleared(a, {0, 0, 160, 16}, kEdramPartMain, 0x11223344);
  ownership.Claim(d, {0, 0, 160, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].kind == EdramOp::Kind::kClear);
  CHECK(ops[0].raw == 0x11223344);
  REQUIRE(ops[0].rects.size() == 1);
  CHECK(ops[0].rects[0] == Rect{0, 0, 160, 16});
  CHECK(d.uniform.Has(0, 0));
  CHECK(d.uniform.Has(0, 1));

  // Tile 0 cleared whole, tile 1 partly: a clear and a transfer.
  ops.clear();
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  ops.clear();
  ownership.Cleared(a, {0, 0, 120, 16}, kEdramPartMain, 0x55);
  CHECK(a.uniform.Has(0, 0));
  CHECK(!a.uniform.Has(0, 1));
  ownership.Claim(d, {0, 0, 160, 16}, 0, ops);
  REQUIRE(ops.size() == 2);
  CHECK(ops[0].kind == EdramOp::Kind::kClear);
  CHECK(ops[0].raw == 0x55);
  CHECK(ops[0].rects[0] == Rect{0, 0, 80, 16});
  CHECK(ops[1].kind == EdramOp::Kind::kTransfer);
  CHECK(ops[1].rects[0] == Rect{80, 0, 160, 16});
  CHECK(d.uniform.Has(0, 0));
  CHECK(!d.uniform.Has(0, 1));

  // A draw over tile 0 forgets it.
  ownership.Written(d, {0, 0, 10, 10}, kEdramPartMain);
  CHECK(!d.uniform.Has(0, 0));
}

TEST_CASE("Native EDRAM writes after a clear forget its uniform tiles",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  // A write first: the next write over the same rectangle takes the fast path.
  ownership.Written(a, {0, 0, 160, 16}, kEdramPartMain);
  ownership.Cleared(a, {0, 0, 160, 16}, kEdramPartMain, 0x11223344);
  CHECK(a.uniform.Has(0, 0));
  // Nothing changed hands since, but the clear set uniform tiles: forgotten.
  ownership.Written(a, {0, 0, 80, 16}, kEdramPartMain);
  CHECK(!a.uniform.Has(0, 0));
  CHECK(a.uniform.Has(0, 1));
}

TEST_CASE("Native EDRAM claims that would do nothing are told apart", "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  EdramTarget b = Target(0);
  std::vector<EdramOp> ops;
  CHECK(!ownership.ClaimIsKept(a, {0, 0, 160, 16}));
  ownership.Claim(a, {0, 720, 160, 8192}, 0, ops);
  ownership.Claim(a, {0, 0, 1280, 720}, 0, ops);
  CHECK(ownership.ClaimIsKept(a, {0, 0, 80, 16}));
  // Beyond the surface (Direct3D 9's 8192-row scissor): clipped as Claim does.
  CHECK(ownership.ClaimIsKept(a, {0, 0, 8192, 8192}));
  CHECK(ownership.ClaimIsKept(a, {0, 0, 0, 0}));
  // Another target took tiles: a's claims are not kept any more, and b's only
  // within the rectangle it claimed.
  ownership.Claim(b, {0, 0, 160, 16}, 0, ops);
  CHECK(!ownership.ClaimIsKept(a, {0, 0, 80, 16}));
  CHECK(ownership.ClaimIsKept(b, {0, 0, 160, 16}));
  CHECK(!ownership.ClaimIsKept(b, {0, 0, 160, 32}));
}

TEST_CASE("Native EDRAM fast paths remember the union of rectangles differing in one edge",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  EdramTarget b = Target(0);
  std::vector<EdramOp> ops;
  // Draws reaching different rows: after the taller one, the shorter one and
  // the first are both inside what is remembered.
  ownership.Claim(a, {0, 0, 1280, 64}, 0, ops);
  ownership.Claim(a, {0, 0, 1280, 160}, 0, ops);
  CHECK(ownership.ClaimIsKept(a, {0, 0, 1280, 64}));
  CHECK(ownership.ClaimIsKept(a, {0, 0, 1280, 160}));
  const uint64_t walks = ownership.walk_counts().claims;
  ownership.Claim(a, {0, 0, 1280, 100}, 0, ops);
  CHECK(ownership.walk_counts().claims == walks);
  ownership.Written(a, {0, 0, 1280, 64}, kEdramPartMain);
  ownership.Written(a, {0, 0, 1280, 160}, kEdramPartMain);
  const uint64_t write_walks = ownership.walk_counts().writes;
  ownership.Written(a, {0, 0, 1280, 64}, kEdramPartMain);
  ownership.Written(a, {0, 0, 1280, 100}, kEdramPartMain);
  CHECK(ownership.walk_counts().writes == write_walks);
  // Rectangles whose union isn't one: only the latest is remembered.
  ownership.Claim(b, {0, 0, 160, 16}, 0, ops);
  ownership.Claim(b, {160, 16, 320, 32}, 0, ops);
  CHECK(!ownership.ClaimIsKept(b, {0, 0, 320, 32}));
  // A claim that took tiles from another target forgets the earlier rectangle
  // (a lost them).
  CHECK(!ownership.ClaimIsKept(a, {0, 0, 160, 16}));
}

TEST_CASE("Native EDRAM claims of other tiles keep a target's fast paths",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  EdramTarget b = Target(600);
  EdramTarget c = Target(600);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 1280, 160}, 0, ops);
  ownership.Written(a, {0, 0, 1280, 160}, kEdramPartMain);
  // Tiles far from a's change hands between b and c (a resolve source and a
  // draw elsewhere in the EDRAM): a's claim and write stay on their fast paths.
  ownership.Claim(b, {0, 0, 1280, 64}, 0, ops);
  ownership.Claim(c, {0, 0, 1280, 64}, 0, ops);
  const EdramOwnership::WalkCounts before = ownership.walk_counts();
  CHECK(ownership.ClaimIsKept(a, {0, 0, 1280, 160}));
  ownership.Claim(a, {0, 0, 1280, 100}, 0, ops);
  ownership.Written(a, {0, 0, 1280, 100}, kEdramPartMain);
  CHECK(ownership.walk_counts().claims == before.claims);
  CHECK(ownership.walk_counts().writes == before.writes);
  // b lost its tiles to c: its fast path ends; c's holds.
  CHECK(!ownership.ClaimIsKept(b, {0, 0, 1280, 64}));
  CHECK(ownership.ClaimIsKept(c, {0, 0, 1280, 64}));
  // Taking one of a's tiles ends a's fast paths.
  EdramTarget d = Target(0, true);
  ownership.Claim(d, {0, 0, 80, 16}, 0, ops);
  CHECK(!ownership.ClaimIsKept(a, {0, 0, 1280, 100}));
}

TEST_CASE("Native EDRAM depth claims bring the stencil once it is used",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget e = Target(0, true);
  EdramTarget f = Target(0, true);
  std::vector<EdramOp> ops;
  ownership.Claim(e, {0, 0, 80, 16}, 0, ops);
  ownership.Cleared(e, {0, 0, 80, 16}, kEdramPartMain | kEdramPartStencil, 0xABCDEF80);

  ownership.Claim(f, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].kind == EdramOp::Kind::kClear);
  CHECK(ops[0].parts == kEdramPartMain);
  CHECK(ops[0].raw == 0xABCDEF80);

  ops.clear();
  ownership.Claim(e, {0, 0, 80, 16}, 0, ops);
  ops.clear();
  f.stencil_used = true;
  ownership.Claim(f, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].kind == EdramOp::Kind::kClear);
  CHECK(ops[0].parts == (kEdramPartMain | kEdramPartStencil));
  CHECK(f.uniform.Has(1, 0));
  CHECK(f.uniform.value(1) == 0x80);

  // Depth known, stencil not: the stencil is transferred, the depth cleared.
  ops.clear();
  ownership.Claim(e, {0, 0, 80, 16}, 0, ops);
  ops.clear();
  ownership.Written(e, {0, 0, 80, 16}, kEdramPartStencil);
  ownership.Claim(f, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 2);
  CHECK(ops[0].kind == EdramOp::Kind::kTransfer);
  CHECK(ops[0].parts == kEdramPartStencil);
  CHECK(ops[1].kind == EdramOp::Kind::kClear);
  CHECK(ops[1].parts == kEdramPartMain);
  CHECK(f.uniform.Has(0, 0));
  CHECK(!f.uniform.Has(1, 0));
}

TEST_CASE("Native EDRAM colour from a uniform colour into depth is a clear",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  EdramTarget b = Target(0, true);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 80, 16}, 0, ops);
  ownership.Cleared(a, {0, 0, 80, 16}, kEdramPartMain, 0xFFFFFF00);
  ownership.Claim(b, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].kind == EdramOp::Kind::kClear);
  CHECK(ops[0].raw == 0xFFFFFF00);
}

TEST_CASE("Native EDRAM claims inside the last one are free until tiles change hands",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  EdramTarget b = Target(0);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  ownership.Claim(a, {0, 0, 80, 16}, 0, ops);
  CHECK(ops.empty());
  uint64_t generation = ownership.generation();
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  CHECK(ownership.generation() == generation);

  ownership.Claim(b, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  ops.clear();
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].src == &b);
  CHECK(ops[0].rects[0] == Rect{0, 0, 80, 16});
}

TEST_CASE("Native EDRAM 64bpp contents stay between 64bpp surfaces of a format",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget g = Target(0, false, kColor16x4, true);
  EdramTarget h = Target(0);
  EdramTarget i = Target(0, false, kColor16x4, true);
  EdramTarget j = Target(0, false, kColor16x4Float, true);
  std::vector<EdramOp> ops;
  // Tile 0: 40x16 pixels of a 64bpp surface.
  ownership.Claim(g, {0, 0, 40, 16}, 0, ops);
  CHECK(ownership.Owner(0) == &g);
  CHECK(ownership.Owner(1) == nullptr);
  ownership.Claim(h, {0, 0, 80, 16}, 0, ops);
  CHECK(ops.empty());
  CHECK(ownership.Owner(0) == &h);

  ownership.Claim(g, {0, 0, 40, 16}, 0, ops);
  CHECK(ops.empty());
  ownership.Claim(j, {0, 0, 40, 16}, 0, ops);
  CHECK(ops.empty());
  ownership.Claim(g, {0, 0, 40, 16}, 0, ops);
  ownership.Claim(i, {0, 0, 40, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].kind == EdramOp::Kind::kTransfer);
  CHECK(ops[0].src == &g);
}

TEST_CASE("Native EDRAM claims transfer from each previous owner", "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  EdramTarget b = Target(0);
  EdramTarget c = Target(0, true);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 80, 16}, 0, ops);
  ownership.Claim(b, {80, 0, 160, 16}, 0, ops);
  ownership.Claim(c, {0, 0, 160, 16}, 0, ops);
  REQUIRE(ops.size() == 2);
  CHECK(ops[0].src == &a);
  CHECK(ops[0].rects[0] == Rect{0, 0, 80, 16});
  CHECK(ops[1].src == &b);
  CHECK(ops[1].rects[0] == Rect{80, 0, 160, 16});
}

TEST_CASE("Native EDRAM tiles of a removed target have no owner", "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  EdramTarget b = Target(0);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  ownership.Remove(a);
  CHECK(ownership.Owner(0) == nullptr);
  ownership.Claim(b, {0, 0, 160, 16}, 0, ops);
  CHECK(ops.empty());
}

TEST_CASE("Native EDRAM surfaces at other bases share tiles by index",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  // b starts one row of a further: its row 0 is a's row 1.
  EdramTarget a = Target(0);
  EdramTarget b = Target(16);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 16, 80, 32}, 0, ops);
  CHECK(ownership.Owner(16) == &a);
  ownership.Claim(b, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].rects[0] == Rect{0, 0, 80, 16});
}

TEST_CASE("Native EDRAM tiles a surface only read go back without a transfer",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  ownership.SetUseCopies(true);
  EdramTarget a = Target(0, true);
  EdramTarget b = Target(0, true);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  ownership.Written(a, {0, 0, 160, 16}, kEdramPartMain);
  ownership.Claim(b, {0, 0, 160, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].kind == EdramOp::Kind::kTransfer);
  // b only tests (Written with no parts): a is still valid.
  ownership.Written(b, {0, 0, 160, 16}, 0);
  ops.clear();
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  CHECK(ops.empty());
  CHECK(ownership.Owner(0) == &a);
  CHECK(ownership.copy_kept_tiles() == 2);
  // And b, unchanged since its transfer, is valid too.
  ownership.Claim(b, {0, 0, 160, 16}, 0, ops);
  CHECK(ops.empty());
  CHECK(ownership.copy_kept_tiles() == 4);
}

TEST_CASE("Native EDRAM tiles written by the other surface come back by transfer",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  ownership.SetUseCopies(true);
  EdramTarget a = Target(0);
  EdramTarget b = Target(0, true);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  ownership.Claim(b, {0, 0, 160, 16}, 0, ops);
  ownership.Written(b, {0, 0, 80, 16}, kEdramPartMain);
  ops.clear();
  ownership.Claim(a, {0, 0, 160, 16}, 0, ops);
  // Tile 0 was written by b, tile 1 wasn't.
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].src == &b);
  REQUIRE(ops[0].rects.size() == 1);
  CHECK(ops[0].rects[0] == Rect{0, 0, 80, 16});
  CHECK(ownership.copy_kept_tiles() == 1);
}

TEST_CASE("Native EDRAM copies are only used when enabled", "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  EdramTarget a = Target(0);
  EdramTarget b = Target(0, true);
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 80, 16}, 0, ops);
  ownership.Claim(b, {0, 0, 80, 16}, 0, ops);
  ops.clear();
  ownership.Claim(a, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].src == &b);
  CHECK(ownership.copy_kept_tiles() == 0);
}

TEST_CASE("Native EDRAM stencil a surface never got comes from a copy holding it",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  ownership.SetUseCopies(true);
  EdramTarget a = Target(0, true);
  EdramTarget b = Target(0, true);
  EdramTarget c = Target(0, true);
  a.stencil_used = true;
  c.stencil_used = true;
  std::vector<EdramOp> ops;
  ownership.Claim(a, {0, 0, 80, 16}, 0, ops);
  ownership.Written(a, {0, 0, 80, 16}, kEdramPartMain | kEdramPartStencil);
  // b takes the depth only (no stencil used): a stays valid in both.
  ownership.Claim(b, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].parts == kEdramPartMain);
  CHECK(ownership.ValidParts(0, &b) == kEdramPartMain);
  CHECK(ownership.ValidParts(0, &a) == (kEdramPartMain | kEdramPartStencil));
  // c needs the stencil, which b doesn't hold: from a.
  ops.clear();
  ownership.Claim(c, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].src == &a);
  CHECK(ops[0].parts == (kEdramPartMain | kEdramPartStencil));
  CHECK(ownership.ValidParts(0, &c) == (kEdramPartMain | kEdramPartStencil));
  // A depth write by c: a stays valid in the stencil, b (depth only) in nothing.
  ownership.Written(c, {0, 0, 80, 16}, kEdramPartMain);
  CHECK(ownership.ValidParts(0, &a) == kEdramPartStencil);
  CHECK(ownership.ValidParts(0, &b) == 0);
  CHECK(ownership.ValidParts(0, &c) == (kEdramPartMain | kEdramPartStencil));
}

TEST_CASE("Native EDRAM depth that came from colour keeps its stencil valid",
          "[graphics][native][edram]") {
  // RDR's shadow maps: a depth surface first filled from colour surfaces
  // (whole cells), then aliased by a depth-only surface that only reads.
  EdramOwnership ownership = Ownership();
  ownership.SetUseCopies(true);
  EdramTarget color = Target(0);
  EdramTarget d = Target(0, true);
  EdramTarget e = Target(0, true);
  d.stencil_used = true;
  std::vector<EdramOp> ops;
  ownership.Claim(color, {0, 0, 80, 16}, 0, ops);
  ownership.Written(color, {0, 0, 80, 16}, kEdramPartMain);
  CHECK(ownership.ValidParts(0, &color) == (kEdramPartMain | kEdramPartStencil));
  ownership.Claim(d, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].parts == (kEdramPartMain | kEdramPartStencil));
  CHECK(ownership.ValidParts(0, &d) == (kEdramPartMain | kEdramPartStencil));
  // A depth write by d: the colour cell changed, colour isn't valid any more.
  ownership.Written(d, {0, 0, 80, 16}, kEdramPartMain);
  CHECK(ownership.ValidParts(0, &color) == 0);
  ops.clear();
  ownership.Claim(e, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  ops.clear();
  ownership.Claim(d, {0, 0, 80, 16}, 0, ops);
  CHECK(ops.empty());
  CHECK(ownership.Owner(0) == &d);
}

TEST_CASE("Native EDRAM colour writes end depth copies' stencil too",
          "[graphics][native][edram]") {
  EdramOwnership ownership = Ownership();
  ownership.SetUseCopies(true);
  EdramTarget d = Target(0, true);
  EdramTarget color = Target(0);
  d.stencil_used = true;
  std::vector<EdramOp> ops;
  ownership.Claim(d, {0, 0, 80, 16}, 0, ops);
  ownership.Claim(color, {0, 0, 80, 16}, 0, ops);
  CHECK(ownership.ValidParts(0, &color) == (kEdramPartMain | kEdramPartStencil));
  ownership.Written(color, {0, 0, 80, 16}, kEdramPartMain);
  CHECK(ownership.ValidParts(0, &d) == 0);
  ops.clear();
  ownership.Claim(d, {0, 0, 80, 16}, 0, ops);
  REQUIRE(ops.size() == 1);
  CHECK(ops[0].src == &color);
  CHECK(ops[0].parts == (kEdramPartMain | kEdramPartStencil));
}
