/**
 * @file        rex/graphics/register_groups.h
 * @brief       Xenos registers in groups of render state, and the census of
 *              which groups each draw changed since the draw before
 *              (gpu_register_group_survey)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * The group map and the fetch constant split are ReIGlue's (ForzaHorizon1-
 * ReImagined SDK, 0670 "the pass ledger, the submitted draws and the
 * change-mask counter", include/rex/graphics/register_groups.h, BSD 3-Clause).
 */

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

namespace rex::graphics::register_groups {

enum Group : uint8_t {
  kSystem,         // outside the render state: control, MMIO, the CP's own registers
  kRenderTargets,  // surface, color and depth info, the mode, masks, MSAA
  kResolve,        // the copy (resolve) and clear registers
  kDynamicState,   // viewport, scissor, window offset, depth bias, blend color, stencil refs
  kPipelineState,  // depth, stencil, blend, raster, primitive and shader program state
  kDrawSetup,      // index buffer, initiators, bins: change with nearly every draw
  kOtherContext,   // the rest of the context registers (shader addresses among them)
  kVertexConstants,
  kPixelConstants,
  kFetchConstants,  // compared per slot: textures, samplers or vertex buffers (below)
  kBoolLoopConstants,
  kRangeGroupCount,
  // Groups the census reports from the fetch constants.
  kTextures = kRangeGroupCount,
  kSamplers,
  kVertexFetch,
  kGroupCount,
};

inline const char* GroupName(uint32_t group) {
  constexpr const char* kNames[kGroupCount] = {
      "system",     "render_targets", "resolve",          "dynamic_state",   "pipeline_state",
      "draw_setup", "other_context",  "vertex_constants", "pixel_constants", "fetch_constants",
      "bool_loop",  "textures",       "samplers",         "vertex_fetch"};
  return group < kGroupCount ? kNames[group] : "?";
}

struct Range {
  uint32_t first, last;  // inclusive
  Group group;
};

// In order, without gaps, from 0 to kRegisterEnd - 1 (RegisterFile::kRegisterCount).
constexpr uint32_t kRegisterEnd = 0x5003;
constexpr Range kRanges[] = {
    {0x0000, 0x1FFF, kSystem},
    {0x2000, 0x2005, kRenderTargets},  // RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, COLOR1-3
    {0x2006, 0x200D, kOtherContext},   // COHER_DEST_BASE_0-7
    {0x200E, 0x200F, kDynamicState},   // PA_SC_SCREEN_SCISSOR_TL/BR
    {0x2010, 0x207F, kOtherContext},
    {0x2080, 0x208B, kDynamicState},  // PA_SC_WINDOW_OFFSET, WINDOW_SCISSOR, CLIPRECT
    {0x208C, 0x20FF, kOtherContext},
    {0x2100, 0x2103, kDrawSetup},      // VGT_MAX/MIN_VTX_INDX, INDX_OFFSET, the reset index
    {0x2104, 0x2104, kRenderTargets},  // RB_COLOR_MASK
    {0x2105, 0x2108, kDynamicState},   // RB_BLEND_RED..ALPHA (blend constants)
    {0x2109, 0x210B, kOtherContext},   // RB_FOG_COLOR
    {0x210C, 0x210D, kDynamicState},   // RB_STENCILREFMASK_BF, RB_STENCILREFMASK
    {0x210E, 0x210E, kPipelineState},  // RB_ALPHA_REF
    {0x210F, 0x2114, kDynamicState},   // PA_CL_VPORT_*
    {0x2115, 0x217F, kOtherContext},
    {0x2180, 0x2184, kPipelineState},  // SQ_PROGRAM_CNTL, CONTEXT_MISC, INTERPOLATOR, WRAPPING
    {0x2185, 0x21F8, kOtherContext},   // SQ_PS/VS_PROGRAM, GFX_COPY_STATE among them
    {0x21F9, 0x21FD, kDrawSetup},      // VGT_EVENT_INITIATOR, DMA_BASE/SIZE, DRAW_INITIATOR, IMMED
    {0x21FE, 0x21FF, kOtherContext},
    {0x2200, 0x2202, kPipelineState},  // RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL
    {0x2203, 0x2203, kRenderTargets},  // RB_HIZCONTROL
    {0x2204, 0x2205, kPipelineState},  // PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL
    {0x2206, 0x2206, kDynamicState},   // PA_CL_VTE_CNTL
    {0x2207, 0x2207, kDrawSetup},      // VGT_CURRENT_BIN_ID_MIN
    {0x2208, 0x2208, kRenderTargets},  // RB_MODECONTROL
    {0x2209, 0x220B, kPipelineState},  // RB_BLENDCONTROL1-3
    {0x220C, 0x227F, kOtherContext},
    {0x2280, 0x2294, kPipelineState},  // points, lines, VGT output path, HOS, groups, VIZ query
    {0x2295, 0x22FF, kOtherContext},
    {0x2300, 0x2300, kPipelineState},  // PA_SC_LINE_CNTL
    {0x2301, 0x2301, kRenderTargets},  // PA_SC_AA_CONFIG
    {0x2302, 0x2302, kPipelineState},  // PA_SU_VTX_CNTL
    {0x2303, 0x2306, kDynamicState},   // PA_CL_GB_* clip and discard adjust
    {0x2307, 0x2311, kOtherContext},   // SQ_VS/PS_CONST, debug, unknown
    {0x2312, 0x2312, kRenderTargets},  // PA_SC_AA_MASK
    {0x2313, 0x2317, kOtherContext},
    {0x2318, 0x2325, kResolve},        // RB_COPY_*, the clears, the sample count
    {0x2326, 0x2326, kRenderTargets},  // RB_COLOR_DEST_MASK
    {0x2327, 0x237F, kOtherContext},
    {0x2380, 0x2383, kDynamicState},   // PA_SU_POLY_OFFSET_* (depth bias)
    {0x2384, 0x239F, kPipelineState},  // point sizes, the user clip planes
    {0x23A0, 0x3FFF, kOtherContext},
    {0x4000, 0x43FF, kVertexConstants},
    {0x4400, 0x47FF, kPixelConstants},
    {0x4800, 0x48BF, kFetchConstants},  // 32 slots of 6 words
    {0x48C0, 0x48FF, kSystem},
    {0x4900, 0x4927, kBoolLoopConstants},
    {0x4928, 0x5002, kSystem},
};

constexpr Group GroupOf(uint32_t index) {
  for (const Range& r : kRanges) {
    if (index >= r.first && index <= r.last) {
      return r.group;
    }
  }
  return kSystem;
}

// GroupOf for every register, built once (a lookup a changed register).
inline const std::array<uint8_t, kRegisterEnd>& GroupTable() {
  static const std::array<uint8_t, kRegisterEnd> table = [] {
    std::array<uint8_t, kRegisterEnd> groups{};
    for (const Range& r : kRanges) {
      for (uint32_t i = r.first; i <= r.last && i < kRegisterEnd; ++i) {
        groups[i] = r.group;
      }
    }
    return groups;
  }();
  return table;
}

// The ranges the census compares draw by draw: the context registers the game
// uses, and the constants.
constexpr uint32_t kContextFirst = 0x2000, kContextEnd = 0x2400;
constexpr uint32_t kConstantsFirst = 0x4000, kConstantsEnd = 0x4928;
constexpr uint32_t kFetchFirst = 0x4800, kFetchSlots = 32, kFetchSlotWords = 6;

// A texture fetch constant's sampler bits per word (xenos::xe_gpu_texture_fetch_t): the clamp
// modes; the filters, anisotropy and border size; all of word 4 (volume filters, mip levels,
// aniso walk, LOD bias, gradient bias); the border color, its W, tri clamp and aniso bias. The
// other bits are the texture's (format, address, size, swizzle, dimension, mips).
constexpr uint32_t kSamplerMask[kFetchSlotWords] = {0x0007FC00u, 0,           0,
                                                    0xFFF80000u, 0xFFFFFFFFu, 0x000001FFu};
// A slot's type (word 0, bits 0-1): 2 a texture, otherwise vertex fetch (three of two words).
constexpr uint32_t kFetchTypeTexture = 2;

// Which groups a fetch slot's change touches: `old_words` and `new_words` are its 6 words.
// Returns a mask of 1 << kTextures, kSamplers, kVertexFetch.
constexpr uint32_t FetchSlotChange(const uint32_t* old_words, const uint32_t* new_words) {
  uint32_t mask = 0;
  const bool texture = (new_words[0] & 3) == kFetchTypeTexture;
  for (uint32_t i = 0; i < kFetchSlotWords; ++i) {
    const uint32_t diff = old_words[i] ^ new_words[i];
    if (!diff) {
      continue;
    }
    if (!texture) {
      mask |= 1u << kVertexFetch;
    } else {
      if (diff & kSamplerMask[i]) {
        mask |= 1u << kSamplers;
      }
      if (diff & ~kSamplerMask[i]) {
        mask |= 1u << kTextures;
      }
    }
  }
  return mask;
}

// gpu_register_group_survey: per draw, which groups changed a value since the
// previous draw. What a renderer that skips its per-draw work for unchanged
// state (pipeline lookups, descriptor sets, constant uploads) could save at
// most is the unchanged share of each group (doc 21 Y14, V2). Compares about
// 3,400 registers a draw, so it costs CPU time.
class RegisterGroupSurvey {
 public:
  // The groups the census line reports, in order.
  static constexpr Group kReported[] = {kRenderTargets,     kPipelineState,  kDynamicState,
                                        kVertexConstants,   kPixelConstants, kTextures,
                                        kSamplers,          kVertexFetch,    kBoolLoopConstants,
                                        kOtherContext,      kResolve};
  // A draw with none of these changed could reuse everything bound for the
  // draw before (render targets, pipeline, descriptors, constants).
  static constexpr uint32_t kBindingGroups =
      (1u << kRenderTargets) | (1u << kPipelineState) | (1u << kVertexConstants) |
      (1u << kPixelConstants) | (1u << kTextures) | (1u << kSamplers) | (1u << kVertexFetch) |
      (1u << kBoolLoopConstants) | (1u << kOtherContext);

  // `registers`: the whole register file (kRegisterEnd values). Returns the
  // change mask of this draw (0 for the first one, which only takes the copy).
  uint32_t Observe(const uint32_t* registers) {
    uint32_t mask = 0;
    if (has_previous_) {
      const auto& groups = GroupTable();
      for (uint32_t i = kContextFirst; i < kContextEnd; ++i) {
        if (registers[i] != previous_[i]) {
          mask |= 1u << groups[i];
        }
      }
      for (uint32_t i = kConstantsFirst; i < kFetchFirst; ++i) {
        if (registers[i] != previous_[i]) {
          mask |= 1u << groups[i];
        }
      }
      for (uint32_t slot = 0; slot < kFetchSlots; ++slot) {
        const uint32_t first = kFetchFirst + slot * kFetchSlotWords;
        mask |= FetchSlotChange(&previous_[first], &registers[first]);
      }
      for (uint32_t i = kFetchFirst + kFetchSlots * kFetchSlotWords; i < kConstantsEnd; ++i) {
        if (registers[i] != previous_[i]) {
          mask |= 1u << groups[i];
        }
      }
      ++compared_;
      for (uint32_t g = 0; g < kGroupCount; ++g) {
        if (mask & (1u << g)) {
          ++changed_[g];
        }
      }
      if (!(mask & kBindingGroups)) {
        ++binding_unchanged_;
      }
    }
    std::memcpy(&previous_[kContextFirst], &registers[kContextFirst],
                (kContextEnd - kContextFirst) * sizeof(uint32_t));
    std::memcpy(&previous_[kConstantsFirst], &registers[kConstantsFirst],
                (kConstantsEnd - kConstantsFirst) * sizeof(uint32_t));
    has_previous_ = true;
    return mask;
  }

  uint64_t compared() const { return compared_; }
  uint64_t changed(uint32_t group) const { return group < kGroupCount ? changed_[group] : 0; }
  uint64_t binding_unchanged() const { return binding_unchanged_; }

  // "<draws> draws a frame; unchanged from the draw before: render_targets 98.1%, ...;
  // all bindings 12.0%", or empty without draws compared.
  std::string Format(uint64_t frames) const {
    if (!compared_) {
      return std::string();
    }
    const double draws = double(compared_);
    std::string line = fmt::format("{:.0f} draws a frame; unchanged from the draw before:",
                                   frames ? draws / double(frames) : draws);
    bool first = true;
    for (Group g : kReported) {
      line += fmt::format("{} {} {:.1f}%", first ? "" : ",", GroupName(g),
                          100.0 * double(compared_ - changed_[g]) / draws);
      first = false;
    }
    line += fmt::format("; all bindings {:.1f}%", 100.0 * double(binding_unchanged_) / draws);
    return line;
  }

  // Counts start over; the copy stays, so the next draw is compared with the
  // one before it.
  void ResetCounts() {
    compared_ = 0;
    binding_unchanged_ = 0;
    changed_.fill(0);
  }

 private:
  std::array<uint32_t, kRegisterEnd> previous_{};
  bool has_previous_ = false;
  uint64_t compared_ = 0;
  uint64_t binding_unchanged_ = 0;
  std::array<uint64_t, kGroupCount> changed_{};
};

}  // namespace rex::graphics::register_groups
