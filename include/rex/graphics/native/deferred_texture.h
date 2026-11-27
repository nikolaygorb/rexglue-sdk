#pragma once

#include <cstdint>
#include <limits>

#include <rex/graphics/native/scale.h>

namespace rex::graphics::native {

// Find walks the resolver's images. Cache only whether an origin exists, not
// an image pointer or a coverage decision. The query depends only on the
// origin address and resolver generation. Format / sampler / swizzle changes
// still go through eligibility and full preflight, but need no new Find.
class DeferredResolveOriginMemo {
 public:
  struct Result {
    bool found;
    bool reused;
  };

  template <typename Lookup>
  Result Query(uint32_t address, uint64_t resolver_generation, Lookup&& lookup) {
    if (valid_ && address_ == address && resolver_generation_ == resolver_generation) {
      return {found_, true};
    }
    // Publish the key only after the lookup succeeds (it may throw).
    const bool found = lookup();
    address_ = address;
    resolver_generation_ = resolver_generation;
    found_ = found;
    valid_ = true;
    return {found_, false};
  }

 private:
  uint32_t address_ = 0;
  uint64_t resolver_generation_ = 0;
  bool valid_ = false;
  bool found_ = false;
};

// A fetch may have both unsigned and signed SPIR-V descriptors. Dimensions,
// not the presence of the signed descriptor, decide whether all uses are 2D.
struct DeferredTextureDimensions {
  uint32_t sampled_2d = 0;
  uint32_t other = 0;
  constexpr void Add(uint32_t slot, xenos::FetchOpDimension dimension) {
    if (slot >= 32) return;
    const uint32_t bit = uint32_t(1) << slot;
    if (dimension == xenos::FetchOpDimension::k1D || dimension == xenos::FetchOpDimension::k2D) {
      sampled_2d |= bit;
    } else {
      other |= bit;
    }
  }
  constexpr uint32_t candidates() const { return sampled_2d & ~other; }
};

constexpr bool DeferredTextureSignsCompatible(uint8_t signs, bool compatible, bool same_format) {
  for (uint32_t component = 0; component < 4; ++component) {
    if (((signs >> (component * 2)) & 3) == uint32_t(xenos::TextureSign::kSigned)) {
      return compatible && same_format;
    }
  }
  return true;
}

// A full-only preflight may decline a partial view without ever constructing
// it. Its null result must not hide that view from the later eager request.
constexpr bool CanReuseDeferredScaledMemo(bool full_only_request, bool was_full_only, bool has_view) {
  return full_only_request || !was_full_only || has_view;
}

// The first experiment accepts exactly one compatible, wholly backed piece.
// Guest coverage alone is insufficient: fractional scaling or clipped host
// sources may leave pixels whose values still require the ordinary base.
constexpr bool DeferredSingleResolveCovers(const Rect& source, const Rect& destination,
                                          uint32_t width, uint32_t height,
                                          uint32_t source_host_width, uint32_t source_host_height,
                                          uint32_t scale4) {
  if (!width || !height || width > 8192 || height > 8192 ||
      scale4 <= kScale4Min || scale4 > kScale4Max || source.empty() ||
      source.left < 0 || source.top < 0 ||
      source.right > std::numeric_limits<int32_t>::max() / int32_t(scale4) ||
      source.bottom > std::numeric_limits<int32_t>::max() / int32_t(scale4) ||
      source_host_width > uint32_t(std::numeric_limits<int32_t>::max()) ||
      source_host_height > uint32_t(std::numeric_limits<int32_t>::max()) ||
      destination != Rect{0, 0, int32_t(width), int32_t(height)} ||
      source.right - source.left != int32_t(width) || source.bottom - source.top != int32_t(height)) {
    return false;
  }
  const Rect src = ScaleRect(source, scale4);
  const Rect dst = ScaleRect(destination, scale4);
  return dst.right == int32_t(ScaleSize(width, scale4)) &&
         dst.bottom == int32_t(ScaleSize(height, scale4)) &&
         src.right <= int32_t(source_host_width) && src.bottom <= int32_t(source_host_height) &&
         src.right - src.left >= dst.right && src.bottom - src.top >= dst.bottom;
}

}  // namespace rex::graphics::native
