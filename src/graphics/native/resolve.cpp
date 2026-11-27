/**
 * @file        graphics/native/resolve.cpp
 * @brief       Native renderer: resolves into host textures, and their clears
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              RendererResolve, HostTexture), Copyright (c) 2026 OpenPortroid, MIT License
 *              (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/resolve.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/graphics/native/edram_tiles.h>
#include <rex/graphics/native/image_barrier.h>
#include <rex/graphics/native/render_targets.h>
#include <rex/graphics/native/resolve_placement.h>
#include <rex/graphics/native/resolve_rect.h>
#include <rex/graphics/native/shared_memory.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/native/texture_cache.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DECLARE(bool, native_sample_resolved_images);

REXCVAR_DEFINE_BOOL(native_resolve_into_textures, true, "GPU",
                    "Native renderer at resolution scale 1: a resolve is also copied into the "
                    "texture the game samples it as, when that texture exists and the copy covers "
                    "it in its own format, instead of the texture being loaded again from the "
                    "memory the resolve wrote; and a resolve nothing samples that way and the "
                    "present can't show writes no image of its own (the guest memory still gets "
                    "it). Off: as before")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_resolve_into_textures_depth, true, "GPU",
                    "With native_resolve_into_textures: depth resolves too (k_24_8 / "
                    "k_24_8_FLOAT textures get the render target's depth as floats, not rounded "
                    "to the guest's 24 bits); off: depth textures are loaded from memory")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_resolve_unsampled_images, false, "GPU",
                    "With native_resolve_into_textures: resolves write their image even when "
                    "nothing at scale 1 reads it (as before), to compare")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_resolve_into_textures_direct, true, "GPU",
                    "With native_resolve_into_textures: a colour resolve the texture takes as "
                    "it is (same format, no MSAA, no exponent bias) is copied from the render "
                    "target straight into the texture, without the resolve's own image in "
                    "between (as xenos); off: through the image, as before")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::native {

namespace util = ui::vulkan::util;
using xenos::ColorFormat;

static_assert(Resolver::Stats::kResultCount ==
              size_t(NativeTextureCache::ResolveCopy::kCount));

namespace {

// Reasons a resolve is not exact yet, logged once each (Resolver::LogOnce).
enum LogReason : uint32_t {
  kLogDepthCopy = 1u << 0,
  kLogDestFormat = 1u << 1,
  kLogMsaaConvert = 1u << 2,
  kLogConvert = 1u << 3,
  kLogExpBias = 1u << 4,
  kLogFixed16 = 1u << 5,
  kLogConvertTo1111 = 1u << 6,
  kLogNoPass = 1u << 7,
  // Not a defect: the first depth copy into a texture, for the log.
  kLogDepthPlaced = 1u << 8,
};

// One image per destination address and host format: a game resolving
// depth and colour to the same memory (Perfect Dark Zero's shadow map and
// scene, at 1D700000) keeps both instead of recreating one twice a frame.
uint64_t ResolveImageKey(uint32_t address, VkFormat vk_format) {
  return (uint64_t(uint32_t(vk_format)) << 32) | address;
}

// Samples of copy_sample_select (k0..k3, k01, k23, k0123, and the reserved
// value as k0123).
constexpr uint32_t kSampleMasks[8] = {1, 2, 4, 8, 3, 12, 15, 15};

// Guest samples as the host image's: 2x has the top sample (guest 0) at host
// sample 1 with the standard sample locations (as the EDRAM transfers and the
// memory resolves read them); 4x has them in the same order.
uint32_t HostSampleMask(uint32_t guest_mask, xenos::MsaaSamples msaa) {
  if (msaa != xenos::MsaaSamples::k2X) {
    return guest_mask;
  }
  return (guest_mask & ~uint32_t(3)) | ((guest_mask & 1) << 1) | ((guest_mask >> 1) & 1);
}

// Largest destination side (texture limit).
constexpr uint32_t kMaxDestSize = 8192;
// CPU writes queued between two drops; past it, one range covers them.
constexpr size_t kMaxQueuedCpuWrites = 256;

}  // namespace

VkFormat ResolveDestVkFormat(xenos::ColorFormat format, xenos::SurfaceNumberFormat number,
                             const NativeTextureCache* texture_cache) {
  bool snorm = number == xenos::SurfaceNumberFormat::kSignedRepeatingFraction;
  switch (format) {
    case ColorFormat::k_8:
    case ColorFormat::k_8_A:
    case ColorFormat::k_8_B:
      return VK_FORMAT_R8_UNORM;
    case ColorFormat::k_8_8:
      return VK_FORMAT_R8G8_UNORM;
    case ColorFormat::k_8_8_8_8:
    case ColorFormat::k_8_8_8_8_A:
    case ColorFormat::k_8_8_8_8_AS_16_16_16_16:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case ColorFormat::k_2_10_10_10:
    case ColorFormat::k_2_10_10_10_AS_16_16_16_16:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case ColorFormat::k_16:
      return snorm ? VK_FORMAT_R16_SNORM : VK_FORMAT_R16_UNORM;
    case ColorFormat::k_16_16:
      return snorm ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM;
    case ColorFormat::k_16_16_16_16:
      return snorm ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM;
    case ColorFormat::k_16_FLOAT:
      return VK_FORMAT_R16_SFLOAT;
    case ColorFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case ColorFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case ColorFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case ColorFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    case ColorFormat::k_32_32_32_32_FLOAT:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case ColorFormat::k_10_11_11:
    case ColorFormat::k_11_11_10:
    case ColorFormat::k_10_11_11_AS_16_16_16_16:
    case ColorFormat::k_11_11_10_AS_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_UNORM;
    // Packed 16-bit formats: the texture cache's own host format (B4G4R4A4,
    // R5G6B5, A1R5G5B5, or RGBA8 where the device can't filter them). Its
    // loads rearrange the guest's bits so that the host components are the
    // guest's red, green, blue and alpha, which is what a blit or the resolve
    // pass writes component by component: the image and a texture the cache
    // loads from the copy agree, and composites match them by host format.
    // Not k_6_5_5: the cache keeps its green bits in blue (and the other way
    // round) and swaps them back with the swizzle. RDR resolves k_8_8_8_8 into
    // k_4_4_4_4 once early in loading (doc 31).
    case ColorFormat::k_1_5_5_5:
    case ColorFormat::k_5_6_5:
    case ColorFormat::k_4_4_4_4:
      if (number == xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction && texture_cache) {
        return texture_cache->GetUnsignedHostFormat(ColorFormatToTextureFormat(format));
      }
      return VK_FORMAT_UNDEFINED;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

Resolver::~Resolver() {
  Shutdown();
}

bool Resolver::Initialize(Submitter& submitter, RenderTargets& render_targets,
                          NativeSharedMemory* shared_memory) {
  submitter_ = &submitter;
  render_targets_ = &render_targets;
  shared_memory_ = shared_memory;
  if (shared_memory_) {
    memory_pass_ready_ = memory_pass_.Initialize(submitter, render_targets, *shared_memory_);
    if (!memory_pass_ready_) {
      REXGPU_WARN("Native renderer: resolves aren't written into guest memory");
      memory_pass_.Shutdown();
    }
  }
  pass_ready_ = pass_.Initialize(submitter, render_targets);
  if (!pass_ready_) {
    REXGPU_WARN("Native renderer: no resolve pass, depth resolves and resolves that convert are "
                "dropped");
    pass_.Shutdown();
  }
  if (shared_memory_) {
    memory_watch_ = shared_memory_->RegisterGlobalWatch(MemoryWrittenThunk, this);
  }
  return true;
}

void Resolver::Shutdown() {
  if (!submitter_) {
    return;
  }
  if (memory_watch_) {
    shared_memory_->UnregisterGlobalWatch(memory_watch_);
    memory_watch_ = nullptr;
  }
  pass_.Shutdown();
  pass_ready_ = false;
  memory_pass_.Shutdown();
  memory_pass_ready_ = false;
  shared_memory_ = nullptr;
  {
    std::lock_guard<std::mutex> lock(cpu_writes_mutex_);
    cpu_writes_.clear();
    cpu_writes_pending_.store(false, std::memory_order_relaxed);
  }
  // The caller has waited for the GPU.
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (vulkan_device) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    for (auto& [address, texture] : textures_) {
      for (auto& [swizzle, view] : texture->swizzled_views) {
        dfn.vkDestroyImageView(device, view, nullptr);
      }
      util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device, texture->framebuffer);
      util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, texture->array_view);
      util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, texture->view);
      util::DestroyAndNullHandle(dfn.vkDestroyImage, device, texture->image);
      util::DestroyAndNullHandle(dfn.vkFreeMemory, device, texture->memory);
    }
  }
  textures_.clear();
  placeable_index_.clear();
  placeable_max_end_.clear();
  placeable_max_end_dirty_ = false;
  BeginChange();
  NoteChange(0, UINT32_MAX);
  present_address_ = UINT32_MAX;
  logged_ = 0;
  stats_ = {};
  texture_cache_ = nullptr;
  submitter_ = nullptr;
  render_targets_ = nullptr;
}

void Resolver::LogOnce(uint32_t reason_bit, const char* message) {
  if (!(logged_ & reason_bit)) {
    logged_ |= reason_bit;
    REXGPU_WARN("Native renderer: {}", message);
  }
}

void Resolver::MemoryWrittenThunk(const std::unique_lock<std::recursive_mutex>& global_lock,
                                  void* context, uint32_t address_first, uint32_t address_last,
                                  bool invalidated_by_gpu) {
  (void)global_lock;
  // The resolves themselves, and the shared memory's own resets.
  if (invalidated_by_gpu) {
    return;
  }
  auto* resolver = static_cast<Resolver*>(context);
  std::lock_guard<std::mutex> lock(resolver->cpu_writes_mutex_);
  std::vector<std::pair<uint32_t, uint32_t>>& writes = resolver->cpu_writes_;
  if (writes.size() >= kMaxQueuedCpuWrites) {
    uint32_t first = address_first;
    uint32_t last = address_last;
    for (const auto& [write_first, write_last] : writes) {
      first = std::min(first, write_first);
      last = std::max(last, write_last);
    }
    writes.clear();
    writes.emplace_back(first, last);
  } else {
    writes.emplace_back(address_first, address_last);
  }
  resolver->cpu_writes_pending_.store(true, std::memory_order_release);
}

void Resolver::DropWrittenByCpu() {
  if (!cpu_writes_pending_.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  {
    std::lock_guard<std::mutex> lock(cpu_writes_mutex_);
    writes.swap(cpu_writes_);
    cpu_writes_pending_.store(false, std::memory_order_relaxed);
  }
  for (auto it = textures_.begin(); it != textures_.end();) {
    const ResolvedTexture& texture = *it->second;
    bool written = false;
    if (texture.written_end > texture.written_start) {
      for (const auto& [write_first, write_last] : writes) {
        if (write_first < texture.written_end && write_last >= texture.written_start) {
          written = true;
          break;
        }
      }
    }
    if (!written) {
      ++it;
      continue;
    }
    if (present_address_ == it->second->address) {
      present_address_ = UINT32_MAX;
    }
    // Recorded commands (this frame's draws) may still use it.
    BeginChange();
    DestroyLater(std::move(it->second));
    it = textures_.erase(it);
  }
}

void Resolver::BeginChange() {
  ++generation_;
  changes_.push_back({generation_, UINT32_MAX, 0});
  if (changes_.size() > kMaxChanges) {
    changes_.pop_front();
  }
}

void Resolver::NoteChange(uint32_t start, uint32_t end) {
  if (end <= start || changes_.empty()) {
    return;
  }
  Change& change = changes_.back();
  change.start = std::min(change.start, start);
  change.end = std::max(change.end, end);
}

bool Resolver::UnchangedSince(uint64_t generation, uint32_t start, uint32_t end) {
  DropWrittenByCpu();
  if (generation == generation_) {
    return true;
  }
  // Every generation since then must still have its entry.
  if (generation > generation_ || changes_.empty() ||
      changes_.front().generation > generation + 1) {
    return false;
  }
  for (auto it = changes_.rbegin(); it != changes_.rend() && it->generation > generation; ++it) {
    if (it->start < end && it->end > start) {
      return false;
    }
  }
  return true;
}

void Resolver::Unindex(const ResolvedTexture* texture) {
  for (auto it = placeable_index_.begin(); it != placeable_index_.end(); ++it) {
    if (it->texture == texture) {
      placeable_index_.erase(it);
      placeable_max_end_dirty_ = true;
      return;
    }
  }
}

void Resolver::Reindex(ResolvedTexture* texture) {
  Unindex(texture);
  if (!texture->placeable || texture->written_rects.empty()) {
    return;
  }
  PlaceableEntry entry = {texture->written_start, texture->written_end, texture};
  auto position = std::upper_bound(
      placeable_index_.begin(), placeable_index_.end(), entry.start,
      [](uint32_t value, const PlaceableEntry& other) { return value < other.start; });
  placeable_index_.insert(position, entry);
  placeable_max_end_dirty_ = true;
}

ResolvedTexture* Resolver::Find(uint32_t address) {
  DropWrittenByCpu();
  // The last one written there, of any format.
  address &= 0x1FFFFFFF;
  ResolvedTexture* newest = nullptr;
  for (auto& [key, texture] : textures_) {
    if (texture->address == address && (!newest || texture->serial > newest->serial)) {
      newest = texture.get();
    }
  }
  return newest;
}

void Resolver::GetPlaceableOverlapping(uint32_t start, uint32_t end,
                                       std::vector<ResolvedTexture*>& out) {
  out.clear();
  DropWrittenByCpu();
  if (placeable_max_end_dirty_) {
    placeable_max_end_dirty_ = false;
    placeable_max_end_.resize(placeable_index_.size());
    uint32_t max_end = 0;
    for (size_t i = 0; i < placeable_index_.size(); ++i) {
      max_end = std::max(max_end, placeable_index_[i].end);
      placeable_max_end_[i] = max_end;
    }
  }
  // Those starting below `end`, back from the last, while any earlier one
  // still reaches past `start`.
  size_t i = size_t(std::lower_bound(placeable_index_.begin(), placeable_index_.end(), end,
                                     [](const PlaceableEntry& entry, uint32_t value) {
                                       return entry.start < value;
                                     }) -
                    placeable_index_.begin());
  while (i && placeable_max_end_[i - 1] > start) {
    --i;
    if (placeable_index_[i].end > start) {
      out.push_back(placeable_index_[i].texture);
    }
  }
  std::sort(out.begin(), out.end(), [](const ResolvedTexture* a, const ResolvedTexture* b) {
    return a->serial < b->serial;
  });
}

void Resolver::RecordPlacement(const RegisterFile& regs, const Rect& written,
                               ResolvedTexture& texture) {
  auto dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
  auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
  // Depth as the texture the game samples it as (the image holds its float
  // values, as the texture cache holds k_24_8 and k_24_8_FLOAT).
  xenos::TextureFormat format =
      texture.from_depth ? (texture.depth_float24 ? xenos::TextureFormat::k_24_8_FLOAT
                                                  : xenos::TextureFormat::k_24_8)
                         : ColorFormatToTextureFormat(dest_info.copy_dest_format);
  const FormatInfo* format_info = FormatInfo::Get(format);
  uint32_t bytes_per_block = format_info->bytes_per_block();
  bool placeable = !dest_info.copy_dest_array &&
                   format_info->block_width == 1 && format_info->block_height == 1 &&
                   bytes_per_block && !(bytes_per_block & (bytes_per_block - 1)) &&
                   dest_pitch.copy_dest_pitch;
  uint32_t bpp_log2 = 0;
  while (bytes_per_block > (1u << bpp_log2)) {
    ++bpp_log2;
  }
  ResolveDestOrigin origin =
      GetResolveDestOrigin(regs[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF, bpp_log2);
  uint32_t pitch_aligned = (uint32_t(dest_pitch.copy_dest_pitch) + 31) & ~uint32_t(31);
  // A copy into the same image elsewhere in memory, or in another layout:
  // the rectangles written before are not where this one's are.
  if (texture.placeable &&
      (origin.texture_base != texture.dest_texture_base ||
       origin.x_offset != texture.dest_x_offset || pitch_aligned != texture.dest_pitch_aligned ||
       bpp_log2 != texture.dest_bpp_log2 || format != texture.dest_texture_format ||
       dest_info.copy_dest_endian != texture.dest_endian)) {
    texture.written_rects.clear();
  }
  texture.placeable = placeable;
  texture.dest_texture_base = origin.texture_base;
  texture.dest_x_offset = origin.x_offset;
  texture.dest_pitch_aligned = pitch_aligned;
  texture.dest_bpp_log2 = bpp_log2;
  texture.dest_texture_format = format;
  texture.dest_endian = dest_info.copy_dest_endian;
  if (!placeable) {
    texture.written_rects.clear();
    return;
  }
  // The union of what was written: a rectangle inside another adds nothing,
  // one around others replaces them. Many disjoint ones (tiled rendering
  // into one texture) are kept up to a limit, beyond which the image is not
  // placed (1x).
  auto contains = [](const Rect& outer, const Rect& inner) {
    return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right &&
           outer.bottom >= inner.bottom;
  };
  for (const Rect& rect : texture.written_rects) {
    if (contains(rect, written)) {
      return;
    }
  }
  std::erase_if(texture.written_rects, [&](const Rect& rect) { return contains(written, rect); });
  constexpr size_t kMaxWrittenRects = 64;
  if (texture.written_rects.size() >= kMaxWrittenRects) {
    texture.placeable = false;
    texture.written_rects.clear();
    return;
  }
  texture.written_rects.push_back(written);
}

void Resolver::Transition(CommandRecorder* command_buffer, ResolvedTexture& texture,
                          VkImageLayout layout) {
  // A new one is cleared first: only the resolves' rectangles are written.
  TransitionTrackedImage(submitter_->vulkan_device()->functions(), command_buffer, texture.image,
                         VK_IMAGE_ASPECT_COLOR_BIT, texture.layout, layout);
}

VkImageView Resolver::GetSampledView(ResolvedTexture& texture, uint32_t host_swizzle) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  // Without view swizzles the shader swizzles (its red / blue order is not
  // corrected then, milestone N4).
  if (!vulkan_device->properties().imageViewFormatSwizzle) {
    return texture.array_view;
  }
  // Red and blue as the guest's memory would have them after the resolve.
  uint32_t swizzle = (texture.red_blue_swapped ? TextureCache::SwapRedAndBlueSources(host_swizzle)
                                               : host_swizzle) &
                     0xFFF;
  if (swizzle == xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA) {
    return texture.array_view;
  }
  auto it = texture.swizzled_views.find(swizzle);
  if (it != texture.swizzled_views.end()) {
    return it->second;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = texture.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view_info.format = texture.vk_format;
  view_info.components.r = GuestComponentSwizzle(swizzle, 0);
  view_info.components.g = GuestComponentSwizzle(swizzle, 1);
  view_info.components.b = GuestComponentSwizzle(swizzle, 2);
  view_info.components.a = GuestComponentSwizzle(swizzle, 3);
  view_info.subresourceRange = util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageView view;
  if (vulkan_device->functions().vkCreateImageView(vulkan_device->device(), &view_info, nullptr,
                                                   &view) != VK_SUCCESS) {
    return texture.array_view;
  }
  texture.swizzled_views.emplace(swizzle, view);
  return view;
}

void Resolver::DestroyLater(std::unique_ptr<ResolvedTexture> texture) {
  // Lookups in its memory change (the caller began the change).
  NoteChange(texture->written_start, texture->written_end);
  Unindex(texture.get());
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  VkImageView view = texture->view;
  VkImageView array_view = texture->array_view;
  VkFramebuffer framebuffer = texture->framebuffer;
  VkImage image = texture->image;
  VkDeviceMemory memory = texture->memory;
  std::vector<VkImageView> swizzled_views;
  for (auto& [swizzle, swizzled_view] : texture->swizzled_views) {
    swizzled_views.push_back(swizzled_view);
  }
  submitter_->DestroyAfter([vulkan_device, view, array_view, framebuffer, image, memory,
                            swizzled_views = std::move(swizzled_views)]() {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (framebuffer != VK_NULL_HANDLE) {
      dfn.vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    for (VkImageView swizzled_view : swizzled_views) {
      dfn.vkDestroyImageView(device, swizzled_view, nullptr);
    }
    if (array_view != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, array_view, nullptr);
    }
    dfn.vkDestroyImageView(device, view, nullptr);
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
  });
}

ResolvedTexture* Resolver::GetTexture(uint32_t address, xenos::ColorFormat guest_format,
                                      VkFormat vk_format, uint32_t width, uint32_t height,
                                      uint32_t scale4) {
  const uint64_t key = ResolveImageKey(address, vk_format);
  std::unique_ptr<ResolvedTexture>& slot = textures_[key];
  if (slot && slot->vk_format == vk_format && slot->scale4 == scale4 && slot->width >= width &&
      slot->height >= height) {
    slot->guest_format = guest_format;
    return slot.get();
  }
  if (slot) {
    // Recorded commands (this frame's draws, the present) may still use it.
    DestroyLater(std::move(slot));
  }
  auto texture = std::make_unique<ResolvedTexture>();
  texture->id = next_texture_id_++;
  texture->address = address;
  texture->guest_format = guest_format;
  texture->vk_format = vk_format;
  texture->width = width;
  texture->height = height;
  texture->scale4 = scale4;
  texture->host_width = ScaleSize(width, scale4);
  texture->host_height = ScaleSize(height, scale4);

  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = vk_format;
  image_info.extent = {texture->host_width, texture->host_height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  texture->attachment = pass_ready_ && pass_.CanDrawInto(vk_format);
  if (texture->attachment) {
    image_info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool created = util::CreateDedicatedAllocationImage(
      vulkan_device, image_info, util::MemoryPurpose::kDeviceLocal, texture->image,
      texture->memory);
  if (created) {
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = texture->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = vk_format;
    view_info.subresourceRange = util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
    created = dfn.vkCreateImageView(device, &view_info, nullptr, &texture->view) == VK_SUCCESS;
    if (created) {
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      created = dfn.vkCreateImageView(device, &view_info, nullptr, &texture->array_view) ==
                VK_SUCCESS;
    }
  }
  if (!created) {
    REXGPU_ERROR("Native renderer: failed to create a resolve destination ({:08X}, {}x{}, format {})",
                 address, width, height, int32_t(vk_format));
    util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, texture->array_view);
    util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, texture->view);
    util::DestroyAndNullHandle(dfn.vkDestroyImage, device, texture->image);
    util::DestroyAndNullHandle(dfn.vkFreeMemory, device, texture->memory);
    textures_.erase(key);
    return nullptr;
  }
  slot = std::move(texture);
  return slot.get();
}

void Resolver::MarkWrittenByGpu(const RegisterFile& regs, const memory::Memory& memory,
                                ResolvedTexture& texture, uint32_t marked_start,
                                uint32_t marked_length) {
  if (!shared_memory_) {
    return;
  }
  // The range the SDK's resolve would write, from its own derivation.
  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(regs, memory, 1, 1, xenos::kEdramTileCount, false, false,
                                 resolve_info) ||
      !resolve_info.copy_dest_extent_length) {
    return;
  }
  uint32_t start = resolve_info.copy_dest_extent_start;
  uint32_t end = start + resolve_info.copy_dest_extent_length;
  if (texture.written_end > texture.written_start) {
    texture.written_start = std::min(texture.written_start, start);
    texture.written_end = std::max(texture.written_end, end);
  } else {
    texture.written_start = start;
    texture.written_end = end;
  }
  // Valid (not uploaded over the resolve) and watched: the CPU writing to it
  // is seen (MemoryWrittenThunk). Textures loaded from this memory are
  // outdated now. Not again when the memory pass of this resolve did it for
  // the range: its pages would be protected a second time (and a CPU write
  // since then would be taken as the GPU's).
  if (marked_length && marked_start <= start &&
      uint64_t(marked_start) + marked_length >= uint64_t(end)) {
    return;
  }
  shared_memory_->RangeWrittenByGpu(start, resolve_info.copy_dest_extent_length);
}

void Resolver::Resolve(const RegisterFile& regs, const memory::Memory& memory, uint64_t frame,
                       uint32_t& memory_written_start_out, uint32_t& memory_written_length_out) {
  memory_written_start_out = 0;
  memory_written_length_out = 0;
  // Images and their placements may change from here on (in the memory
  // NoteChange records).
  BeginChange();
  auto copy_control = regs.Get<reg::RB_COPY_CONTROL>();
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if (!surface_info.surface_pitch || surface_info.msaa_samples > xenos::MsaaSamples::k4X) {
    return;
  }

  // The rectangle, from the vertices D3D puts in vertex fetch constant 0.
  xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) {
    return;
  }
  if (TraceWriter* trace = TraceWriter::active()) {
    trace->WriteMemoryRead(fetch.address * sizeof(uint32_t), fetch.size * sizeof(uint32_t));
  }
  const float* vertices =
      memory.TranslatePhysical<const float*>(fetch.address * sizeof(uint32_t));
  ResolveRectInput input;
  for (uint32_t i = 0; i < 6; ++i) {
    input.vertices[i] = xenos::GpuSwap(vertices[i], fetch.endian);
  }
  input.half_pixel_offset =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero;
  input.vtx_window_offset_enable = regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable;
  auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  input.window_x_offset = window_offset.window_x_offset;
  input.window_y_offset = window_offset.window_y_offset;
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, false);
  input.scissor = {int32_t(scissor.offset[0]), int32_t(scissor.offset[1]),
                   int32_t(scissor.offset[0] + scissor.extent[0]),
                   int32_t(scissor.offset[1] + scissor.extent[1])};
  input.surface_pitch = surface_info.surface_pitch;
  const Rect rect = ResolveRect(input);
  if (rect.empty()) {
    return;
  }

  const bool is_depth = copy_control.copy_src_select >= xenos::kMaxColorRenderTargets;
  auto color_info = regs.Get<reg::RB_COLOR_INFO>(
      reg::RB_COLOR_INFO::rt_register_indices[copy_control.copy_src_select & 3]);
  auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  RenderTarget* source =
      is_depth ? render_targets_->GetDepth(depth_info.depth_base, depth_info.depth_format,
                                           surface_info.surface_pitch, surface_info.msaa_samples)
               : render_targets_->GetColor(color_info.color_base, color_info.color_format,
                                           surface_info.surface_pitch, surface_info.msaa_samples);
  if (!source) {
    return;
  }
  if (REXCVAR_GET(native_log_edram)) {
    auto log_dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
    auto log_dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
    REXGPU_INFO(
        "EDRAM log: resolve (command {}) of {} base {} format {} pitch {} MSAA {}, rectangle "
        "{},{}-{},{}, samples {}, to {:08X} as colour format {} (pitch {} height {}, endian {}, "
        "exp bias {}), clears: colour {} depth {}",
        uint32_t(copy_control.copy_command), is_depth ? "depth" : "colour",
        source->surface.base_tiles, source->format, source->surface.pitch_pixels,
        uint32_t(source->surface.msaa), rect.left, rect.top, rect.right, rect.bottom,
        uint32_t(copy_control.copy_sample_select), regs[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF,
        uint32_t(log_dest_info.copy_dest_format),
        uint32_t(log_dest_pitch.copy_dest_pitch), uint32_t(log_dest_pitch.copy_dest_height),
        uint32_t(log_dest_info.copy_dest_endian), int32_t(log_dest_info.copy_dest_exp_bias),
        uint32_t(copy_control.color_clear_enable), uint32_t(copy_control.depth_clear_enable));
  }
  // What other surfaces left in the source's tiles there comes in first.
  render_targets_->Claim(*source, rect, 0, RenderTargets::ClaimCaller::kResolve);

  // The guest's bytes into the shared memory (all the copy commands), before
  // the clears below.
  bool written_to_memory = false;
  if (memory_pass_ready_ && (copy_control.copy_command == xenos::CopyCommand::kRaw ||
                             copy_control.copy_command == xenos::CopyCommand::kConvert ||
                             copy_control.copy_command == xenos::CopyCommand::kConvertTo1111)) {
    uint32_t written_start, written_length;
    if (memory_pass_.Resolve(regs, memory, *source, written_start, written_length)) {
      written_to_memory = true;
      // Valid and watched; textures loaded from it outdated.
      shared_memory_->RangeWrittenByGpu(written_start, written_length);
      memory_written_start_out = written_start;
      memory_written_length_out = written_length;
    }
  }

  // The copy into a host texture.
  if (copy_control.copy_command == xenos::CopyCommand::kConvertTo1111) {
    // Sampled from the memory the copy above wrote.
    if (!written_to_memory) {
      LogOnce(kLogConvertTo1111, "resolves to k_DXT3A_AS_1_1_1_1 need guest memory resolves");
    }
  } else if (copy_control.copy_command == xenos::CopyCommand::kRaw ||
             copy_control.copy_command == xenos::CopyCommand::kConvert) {
    auto dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
    auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
    uint32_t dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF;
    // Depth: its float values (as the SDK's texture cache holds k_24_8 and
    // k_24_8_FLOAT textures); colour: the destination format.
    VkFormat dest_format =
        is_depth ? VK_FORMAT_R32_SFLOAT
                 : ResolveDestVkFormat(dest_info.copy_dest_format, dest_info.copy_dest_number,
                                       texture_cache_);
    // Source and destination at the same coordinates (AoT's simplification:
    // a destination point other than the origin moves RB_COPY_DEST_BASE by
    // whole macro tiles, milestone N4).
    Rect source_rect = rect;
    source_rect.right = std::min(source_rect.right, int32_t(source->surface.width));
    source_rect.bottom = std::min(source_rect.bottom, int32_t(source->surface.height));
    if (dest_format == VK_FORMAT_UNDEFINED) {
      // Named, so the next run says which one to add (RDR logs one early in
      // loading, 31 section 2).
      LogOnce(kLogDestFormat,
              fmt::format("a resolve destination format is not handled yet, copy dropped "
                          "(colour format {}, number format {}, {} source format {}, {:08X})",
                          uint32_t(dest_info.copy_dest_format),
                          uint32_t(dest_info.copy_dest_number), is_depth ? "depth" : "colour",
                          source->format, dest_base)
                  .c_str());
    } else if (!source_rect.empty()) {
      uint32_t width = std::min(std::max(uint32_t(dest_pitch.copy_dest_pitch),
                                         uint32_t(source_rect.right)),
                                kMaxDestSize);
      uint32_t height = std::min(std::max(uint32_t(dest_pitch.copy_dest_height),
                                          uint32_t(source_rect.bottom)),
                                 kMaxDestSize);
      // Colour scaled by the exponent bias, and 16-bit fixed values (stored
      // /32) back to their range.
      float scale = 1.0f;
      if (!is_depth) {
        scale = std::ldexp(1.0f, int32_t(dest_info.copy_dest_exp_bias));
        if (source->fixed16) {
          scale *= 32.0f;
        }
      } else if (depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
        // The host holds float24 depth at half its value (the draws' viewport,
        // GetHostViewportInfo full_float24_in_0_to_1), the texture cache's
        // k_24_8_FLOAT the guest's [0, 2).
        scale = 2.0f;
      }
      bool same_format = !is_depth && dest_format == source->vk_format;
      bool multisampled = source->samples != VK_SAMPLE_COUNT_1_BIT;
      bool can_blit =
          !is_depth &&
          (render_targets_->OptimalTilingFeatures(source->vk_format) &
           VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
          (render_targets_->OptimalTilingFeatures(dest_format) & VK_FORMAT_FEATURE_BLIT_DST_BIT);
      // The pass for what the transfer commands can't do (and multisampled
      // sources: copy_sample_select).
      bool use_pass = is_depth || scale != 1.0f || multisampled || (!same_format && !can_blit);
      bool pass_possible = pass_ready_ && pass_.CanDrawInto(dest_format);
      // The image holds what the guest's copy wrote (not a copy without the
      // pass's conversion, below).
      const bool image_exact = !use_pass || pass_possible;
      bool drop = false;
      if (use_pass && !pass_possible) {
        if (is_depth) {
          LogOnce(kLogDepthCopy, "depth resolves need the resolve pass, copy dropped");
          drop = true;
        } else if (multisampled && !same_format) {
          LogOnce(kLogMsaaConvert,
                  "multisampled resolves into another format need the resolve pass, copy dropped");
          drop = true;
        } else if (!same_format && !can_blit) {
          LogOnce(kLogConvert, "a resolve format conversion needs the resolve pass, copy dropped");
          drop = true;
        } else {
          // A copy without the scale.
          if (dest_info.copy_dest_exp_bias) {
            LogOnce(kLogExpBias, "resolve exponent bias needs the resolve pass, not applied");
          }
          if (source->fixed16) {
            LogOnce(kLogFixed16,
                    "resolves from 16-bit fixed render targets need the resolve pass, not "
                    "converted");
          }
          use_pass = false;
        }
        LogOnce(kLogNoPass, "the destination format can't be drawn into for the resolve pass");
      }
      // At 1x nothing samples the image (native_sample_resolved_images aside):
      // the textures of the destination are loaded from the guest memory the
      // memory pass wrote, and only the present may show the image. With
      // native_resolve_into_textures, the texture the game samples the copy
      // as gets the image's texels instead, when it exists and the copy covers
      // it from its origin (a 4 KB aligned destination, so no macro tile
      // phase moves it: image and texture coordinates are the same).
      const bool into_textures = !drop && texture_cache_ && source->scale4 == kScale4Min &&
                                 REXCVAR_GET(native_resolve_into_textures) &&
                                 !REXCVAR_GET(native_sample_resolved_images);
      // The image holds the texels the guest memory got bit for bit (as its
      // texture loads them): not for 16-bit fixed surfaces or destinations
      // (memory: the EDRAM's int16, value * 1024; the image: the value as a
      // normalized one), nor a raw copy into another format (memory: the
      // render target's bits; the image: its values converted).
      const xenos::ColorFormat dest_color_format = dest_info.copy_dest_format;
      const bool image_bits_match =
          is_depth ||
          (!source->fixed16 && dest_color_format != ColorFormat::k_16 &&
           dest_color_format != ColorFormat::k_16_16 &&
           dest_color_format != ColorFormat::k_16_16_16_16 &&
           (copy_control.copy_command != xenos::CopyCommand::kRaw ||
            dest_format == source->vk_format));
      NativeTextureCache::ResolveDestination destination;
      bool destination_sampled = false;
      if (into_textures && image_exact && image_bits_match &&
          (!is_depth || REXCVAR_GET(native_resolve_into_textures_depth)) && !source_rect.left &&
          !source_rect.top && !(dest_base & 0xFFF) && !dest_info.copy_dest_array &&
          uint32_t(dest_info.copy_dest_endian) <= uint32_t(xenos::Endian128::k16in32)) {
        destination.base = dest_base;
        destination.pitch_texels = (uint32_t(dest_pitch.copy_dest_pitch) + 31) & ~uint32_t(31);
        destination.width = uint32_t(source_rect.right);
        destination.height = uint32_t(source_rect.bottom);
        destination.format =
            is_depth ? (depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8
                            ? xenos::TextureFormat::k_24_8_FLOAT
                            : xenos::TextureFormat::k_24_8)
                     : ColorFormatToTextureFormat(dest_info.copy_dest_format);
        destination.endianness = xenos::Endian(uint32_t(dest_info.copy_dest_endian));
        destination_sampled = texture_cache_->HasResolveDestinationTexture(destination);
      }
      // A colour resolve the present may show (PresentFrame: the front buffer's,
      // or the last one of at least 640x360).
      const bool present_candidate = !is_depth && width >= 640 && height >= 360;
      // An image of an earlier resolve there is outdated when this one writes
      // none.
      auto drop_earlier_image = [&]() {
        if (auto it = textures_.find(ResolveImageKey(dest_base, dest_format));
            it != textures_.end()) {
          if (present_address_ == it->second->address) {
            present_address_ = UINT32_MAX;
          }
          DestroyLater(std::move(it->second));
          textures_.erase(it);
        }
      };
      // Only when the guest memory got the copy (its textures are loaded from
      // there).
      if (into_textures && written_to_memory && !destination_sampled && !present_candidate &&
          !REXCVAR_GET(native_resolve_unsampled_images)) {
        drop = true;
        ++stats_.images_skipped;
        drop_earlier_image();
      }
      // native_resolve_into_textures_direct: a copy the texture takes as the
      // render target holds it (no conversion, exponent bias, MSAA or scale:
      // the plain copy below) goes from the render target straight into the
      // texture, as xenos does, without an image in between (at 1x only the
      // present reads one, and the present can't show this one).
      if (!drop && into_textures && written_to_memory && destination_sampled && !use_pass &&
          same_format && !present_candidate &&
          REXCVAR_GET(native_resolve_into_textures_direct)) {
        drop = true;
        ++stats_.images_direct;
        drop_earlier_image();
        if (CommandRecorder* command_buffer = submitter_->OpenOutsideRenderPass(
                RenderPassEnd::kResolve, GpuKind::kResolveTexture)) {
          render_targets_->Transition(command_buffer, *source,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
          NativeTextureCache::ResolveCopy result = texture_cache_->CopyResolveIntoTexture(
              command_buffer, destination, source->image, source->vk_format, false,
              dest_info.copy_dest_swap != 0);
          ++stats_.into_textures[size_t(result)];
        }
      }
      ResolvedTexture* texture =
          drop ? nullptr
               : GetTexture(dest_base, dest_info.copy_dest_format, dest_format, width, height,
                            source->scale4);
      if (texture && use_pass && !texture->attachment) {
        // Created before the pass could draw into it: once more.
        texture = nullptr;
        if (auto it = textures_.find(ResolveImageKey(dest_base, dest_format));
            it != textures_.end()) {
          DestroyLater(std::move(it->second));
          textures_.erase(it);
        }
        texture = GetTexture(dest_base, dest_info.copy_dest_format, dest_format, width, height,
                             source->scale4);
        if (texture && !texture->attachment) {
          texture = nullptr;
        }
      }
      Rect host = ScaleRect(source_rect, source->scale4);
      if (texture) {
        host.right =
            std::min({host.right, int32_t(source->host_width), int32_t(texture->host_width)});
        host.bottom =
            std::min({host.bottom, int32_t(source->host_height), int32_t(texture->host_height)});
      }
      bool copied = false;
      if (texture && !host.empty()) {
        if (use_pass) {
          copied = pass_.Draw(
              *source, *texture, host, scale,
              HostSampleMask(kSampleMasks[uint32_t(copy_control.copy_sample_select) & 7],
                             source->msaa));
        } else if (CommandRecorder* command_buffer =
                       submitter_->OpenOutsideRenderPass(RenderPassEnd::kResolve,
                                                         GpuKind::kResolveImage);
                   command_buffer != nullptr) {
          render_targets_->Transition(command_buffer, *source,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
          Transition(command_buffer, *texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          VkImageSubresourceLayers layers = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          VkOffset3D offset = {host.left, host.top, 0};
          VkExtent3D extent = {uint32_t(host.right - host.left), uint32_t(host.bottom - host.top),
                               1};
          if (multisampled) {
            // All the samples averaged (without the pass).
            VkImageResolve region = {layers, offset, layers, offset, extent};
            command_buffer->CmdVkResolveImage(source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                              texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                              1, &region);
          } else if (same_format) {
            VkImageCopy region = {layers, offset, layers, offset, extent};
            command_buffer->CmdVkCopyImage(source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                           &region);
          } else {
            // Conversion by the blit, same size: unorm / snorm / float only.
            VkImageBlit region = {};
            region.srcSubresource = layers;
            region.srcOffsets[0] = offset;
            region.srcOffsets[1] = {host.right, host.bottom, 1};
            region.dstSubresource = layers;
            region.dstOffsets[0] = offset;
            region.dstOffsets[1] = {host.right, host.bottom, 1};
            command_buffer->CmdVkBlitImage(source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                           &region, VK_FILTER_NEAREST);
          }
          copied = true;
        }
      }
      if (copied) {
        // Lookups in its old and new memory change.
        NoteChange(texture->written_start, texture->written_end);
        texture->red_blue_swapped = !is_depth && dest_info.copy_dest_swap != 0;
        texture->from_depth = is_depth;
        texture->source_format = source->format;
        texture->source_fixed16 = !is_depth && source->fixed16;
        texture->dest_number = uint32_t(dest_info.copy_dest_number);
        texture->depth_float24 =
            is_depth && depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8;
        texture->last_frame = frame;
        texture->serial = next_serial_++;
        RecordPlacement(regs, source_rect, *texture);
        MarkWrittenByGpu(regs, memory, *texture, memory_written_start_out,
                         memory_written_length_out);
        NoteChange(texture->written_start, texture->written_end);
        Reindex(texture);
        // After the memory was marked as written (which outdated the
        // texture): the texture gets the image's texels and is up to date.
        if (destination_sampled) {
          if (CommandRecorder* command_buffer =
                  submitter_->OpenOutsideRenderPass(RenderPassEnd::kResolve,
                                                    GpuKind::kResolveTexture)) {
            Transition(command_buffer, *texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            NativeTextureCache::ResolveCopy result = texture_cache_->CopyResolveIntoTexture(
                command_buffer, destination, texture->image, texture->vk_format, is_depth,
                texture->red_blue_swapped);
            ++stats_.into_textures[size_t(result)];
          }
        }
        if (is_depth && !(logged_ & kLogDepthPlaced)) {
          logged_ |= kLogDepthPlaced;
          REXGPU_INFO(
              "Native renderer: first depth copy into a texture: {:08X} ({:08X} as {}), {}x{} "
              "pitch {} endian {}, {}placed",
              dest_base, texture->dest_texture_base,
              FormatInfo::Get(texture->dest_texture_format)->name, source_rect.right,
              source_rect.bottom, texture->dest_pitch_aligned, uint32_t(texture->dest_endian),
              texture->placeable ? "" : "not ");
        }
        if (!is_depth && width >= 640 && height >= 360) {
          present_address_ = dest_base;
        }
      }
    }
  }

  // The clears that come with the resolve, over the same rectangle.
  if (copy_control.color_clear_enable && !is_depth) {
    float color[4];
    DecodeColorClear(color_info.color_format, regs[XE_GPU_REG_RB_COLOR_CLEAR],
                     regs[XE_GPU_REG_RB_COLOR_CLEAR_LO], color);
    render_targets_->Claim(*source, rect, kEdramPartMain,
                           RenderTargets::ClaimCaller::kResolveClear);
    render_targets_->ClearColor(*source, rect, color);
    // 32bpp formats with raw cells: RB_COLOR_CLEAR is the cell.
    render_targets_->edram().Cleared(*source, rect, kEdramPartMain,
                                     regs[XE_GPU_REG_RB_COLOR_CLEAR]);
  }
  if (copy_control.depth_clear_enable) {
    RenderTarget* depth =
        is_depth ? source
                 : render_targets_->GetDepth(depth_info.depth_base, depth_info.depth_format,
                                             surface_info.surface_pitch,
                                             surface_info.msaa_samples);
    if (depth) {
      uint32_t depth_clear = regs[XE_GPU_REG_RB_DEPTH_CLEAR];
      render_targets_->Claim(*depth, rect, kEdramPartMain | kEdramPartStencil,
                             RenderTargets::ClaimCaller::kResolveClear);
      render_targets_->ClearDepthStencil(
          *depth, rect, DecodeDepth24(depth_clear >> 8, depth_info.depth_format),
          depth_clear & 0xFF);
      // Depth24 << 8 | stencil, as the cell.
      render_targets_->edram().Cleared(*depth, rect, kEdramPartMain | kEdramPartStencil,
                                       depth_clear);
    }
  }
}

}  // namespace rex::graphics::native
