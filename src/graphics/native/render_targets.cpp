/**
 * @file        graphics/native/render_targets.cpp
 * @brief       Native renderer: host render targets standing for EDRAM surfaces
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              RenderTarget, GetRenderTarget, ColorFormat .. DepthFormat), Copyright (c) 2026
 *              OpenPortroid, MIT License (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/render_targets.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

#include <rex/cvar.h>
#include <rex/graphics/native/edram_transfer.h>
#include <rex/graphics/native/image_barrier.h>
#include <rex/graphics/native/submission.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DECLARE(bool, native_edram_transfers);
REXCVAR_DECLARE(std::string, native_drop_transfers);
REXCVAR_DECLARE(bool, native_profile_transfers);

REXCVAR_DEFINE_BOOL(native_edram_valid_copies, true, "GPU",
                    "Native renderer: a surface taking back EDRAM tiles it still holds unchanged "
                    "(the surface that had them only read them) skips the transfer; off: every "
                    "change of hands transfers")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Also read by edram_transfer.cpp.
REXCVAR_DEFINE_BOOL(native_pass_area_bounds, true, "GPU",
                    "Native renderer: the render passes of EDRAM transfers and clears cover only "
                    "the bounds of their rectangles (render area); off: the whole surface")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::native {

namespace util = ui::vulkan::util;
using xenos::ColorRenderTargetFormat;

xenos::ColorRenderTargetFormat KeyColorFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return ColorRenderTargetFormat::k_8_8_8_8;
    case ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return ColorRenderTargetFormat::k_2_10_10_10;
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return ColorRenderTargetFormat::k_2_10_10_10_FLOAT;
    default:
      return format;
  }
}

bool Is64bppColorFormat(xenos::ColorRenderTargetFormat format) {
  return format == ColorRenderTargetFormat::k_16_16_16_16 ||
         format == ColorRenderTargetFormat::k_16_16_16_16_FLOAT ||
         format == ColorRenderTargetFormat::k_32_32_FLOAT;
}

RenderTargets::RenderTargets() = default;

RenderTargets::~RenderTargets() {
  Shutdown();
}

VkFormatFeatureFlags RenderTargets::OptimalTilingFeatures(VkFormat format) const {
  auto it = format_features_.find(format);
  if (it != format_features_.end()) {
    return it->second;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  VkFormatProperties properties = {};
  vulkan_device->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
      vulkan_device->physical_device(), format, &properties);
  format_features_.emplace(format, properties.optimalTilingFeatures);
  return properties.optimalTilingFeatures;
}

bool RenderTargets::Initialize(Submitter& submitter, uint32_t tile_count, uint32_t scale4) {
  profile_transfers_ = REXCVAR_GET(native_profile_transfers);
  claim_stats_ = {};
  submitter_ = &submitter;
  tile_count_ = tile_count;
  scale4_ = std::clamp(scale4, kScale4Min, kScale4Max);

  // The first format the device can render to, sample and copy.
  constexpr VkFormatFeatureFlags kColorFeatures =
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  constexpr VkFormatFeatureFlags kDepthFeatures =
      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  auto pick = [&](std::initializer_list<VkFormat> candidates, VkFormatFeatureFlags features) {
    for (VkFormat candidate : candidates) {
      if ((OptimalTilingFeatures(candidate) & features) == features) {
        return candidate;
      }
    }
    return VK_FORMAT_UNDEFINED;
  };
  auto set_color = [&](ColorRenderTargetFormat format, std::initializer_list<VkFormat> candidates) {
    color_formats_[uint32_t(format)] = pick(candidates, kColorFeatures);
    if (color_formats_[uint32_t(format)] == VK_FORMAT_UNDEFINED) {
      REXGPU_WARN("Native renderer: no host format for colour render target format {}",
                  uint32_t(format));
    }
  };
  set_color(ColorRenderTargetFormat::k_8_8_8_8, {VK_FORMAT_R8G8B8A8_UNORM});
  set_color(ColorRenderTargetFormat::k_2_10_10_10,
            {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16B16A16_UNORM});
  set_color(ColorRenderTargetFormat::k_2_10_10_10_FLOAT, {VK_FORMAT_R16G16B16A16_SFLOAT});
  set_color(ColorRenderTargetFormat::k_16_16,
            {VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_SFLOAT});
  set_color(ColorRenderTargetFormat::k_16_16_16_16,
            {VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SFLOAT});
  set_color(ColorRenderTargetFormat::k_16_16_FLOAT, {VK_FORMAT_R16G16_SFLOAT});
  set_color(ColorRenderTargetFormat::k_16_16_16_16_FLOAT, {VK_FORMAT_R16G16B16A16_SFLOAT});
  set_color(ColorRenderTargetFormat::k_32_FLOAT, {VK_FORMAT_R32_SFLOAT});
  set_color(ColorRenderTargetFormat::k_32_32_FLOAT, {VK_FORMAT_R32G32_SFLOAT});
  // 20e4 needs more than unorm24; AMD has no D24S8 attachments.
  depth_formats_[uint32_t(xenos::DepthRenderTargetFormat::kD24S8)] = pick(
      {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}, kDepthFeatures);
  depth_formats_[uint32_t(xenos::DepthRenderTargetFormat::kD24FS8)] =
      pick({VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}, kDepthFeatures);
  if (depth_formats_[0] == VK_FORMAT_UNDEFINED || depth_formats_[1] == VK_FORMAT_UNDEFINED) {
    REXGPU_ERROR("Native renderer: no host depth / stencil format");
    return false;
  }
  edram_.Reset(tile_count_);
  transfers_ = std::make_unique<EdramTransfers>();
  if (!transfers_->Initialize(submitter, *this)) {
    REXGPU_WARN("Native renderer: no EDRAM transfers, surfaces sharing EDRAM won't see each "
                "other's contents");
    transfers_->Shutdown();
    transfers_.reset();
  }
  return true;
}

void RenderTargets::Shutdown() {
  if (!submitter_) {
    return;
  }
  // The caller has waited for the GPU. The transfers' views of the targets
  // first.
  if (transfers_) {
    transfers_->Shutdown();
    transfers_.reset();
  }
  edram_.Reset(tile_count_);
  claim_ops_.clear();
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (vulkan_device) {
    for (auto& [hash, framebuffer] : draw_framebuffers_) {
      vulkan_device->functions().vkDestroyFramebuffer(vulkan_device->device(), framebuffer.second,
                                                      nullptr);
    }
  }
  draw_framebuffers_.clear();
  for (auto& [key, target] : targets_) {
    DestroyTarget(*target);
  }
  targets_.clear();
  if (vulkan_device) {
    for (auto& [key, render_pass] : clear_render_passes_) {
      vulkan_device->functions().vkDestroyRenderPass(vulkan_device->device(), render_pass,
                                                     nullptr);
    }
  }
  clear_render_passes_.clear();
  format_features_.clear();
  submitter_ = nullptr;
}

VkFormat RenderTargets::ColorVkFormat(xenos::ColorRenderTargetFormat format) const {
  return color_formats_[uint32_t(KeyColorFormat(format)) & 15];
}

VkFormat RenderTargets::DepthVkFormat(xenos::DepthRenderTargetFormat format) const {
  return depth_formats_[uint32_t(format) & 1];
}

uint64_t RenderTargets::SurfaceKey(uint32_t base_tiles, uint32_t format, bool depth, uint32_t pitch,
                                  xenos::MsaaSamples msaa) {
  // base 13 | format 4 | pitch 14 | msaa 2 | depth 1; image keys add the scale (5 bits) above.
  return uint64_t(base_tiles % xenos::kEdramTileCount) | (uint64_t(format & 15) << 13) | (uint64_t(pitch & 0x3FFF) << 17) |
         (uint64_t(msaa) << 31) | (uint64_t(depth) << 33);
}

RenderTarget* RenderTargets::GetColor(uint32_t base_tiles, xenos::ColorRenderTargetFormat format,
                                      uint32_t pitch, xenos::MsaaSamples msaa, uint32_t scale4) {
  return Get(base_tiles, uint32_t(KeyColorFormat(format)), false, pitch, msaa, scale4);
}

RenderTarget* RenderTargets::GetDepth(uint32_t base_tiles, xenos::DepthRenderTargetFormat format,
                                      uint32_t pitch, xenos::MsaaSamples msaa, uint32_t scale4) {
  return Get(base_tiles, uint32_t(format), true, pitch, msaa, scale4);
}

RenderTarget* RenderTargets::Instance(const RenderTarget& target, uint32_t scale4) {
  return Get(target.base_tiles, target.format, target.depth, target.pitch, target.msaa,
             scale4 ? scale4 : scale4_);
}

RenderTarget* RenderTargets::Get(uint32_t base_tiles, uint32_t format, bool depth, uint32_t pitch,
                                 xenos::MsaaSamples msaa, uint32_t scale4) {
  if (!pitch || msaa > xenos::MsaaSamples::k4X || !tile_count_) {
    return nullptr;
  }
  base_tiles %= tile_count_;
  const uint64_t surface_key = SurfaceKey(base_tiles, format, depth, pitch, msaa);
  if (!scale4) {
    scale4 = ScaleFor(surface_key);
  }
  const uint64_t key = surface_key | (uint64_t(scale4 & 31) << 34);
  auto it = targets_.find(key);
  if (it != targets_.end()) {
    return it->second.get();
  }
  auto target = std::make_unique<RenderTarget>();
  target->base_tiles = base_tiles;
  target->pitch = pitch;
  target->format = format;
  target->msaa = msaa;
  target->depth = depth;
  bool is_64bpp = !depth && Is64bppColorFormat(ColorRenderTargetFormat(format));
  target->surface.base_tiles = base_tiles;
  target->surface.pitch_pixels = pitch;
  target->surface.msaa = msaa;
  target->surface.is_64bpp = is_64bpp;
  target->surface.width = pitch;
  target->surface.height = SurfaceHeightPixels(base_tiles, pitch, msaa, is_64bpp, tile_count_);
  target->uniform.Reset(tile_count_);
  target->scale4 = scale4;
  target->host_width = ScaleSize(target->surface.width, target->scale4);
  target->host_height = ScaleSize(target->surface.height, target->scale4);
  target->fixed16 = !depth && (ColorRenderTargetFormat(format) == ColorRenderTargetFormat::k_16_16 ||
                               ColorRenderTargetFormat(format) == ColorRenderTargetFormat::k_16_16_16_16);
  target->vk_format = depth ? DepthVkFormat(xenos::DepthRenderTargetFormat(format))
                            : ColorVkFormat(ColorRenderTargetFormat(format));
  target->aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                         : VK_IMAGE_ASPECT_COLOR_BIT;
  target->samples = VkSampleCountFlagBits(uint32_t(1) << uint32_t(msaa));
  if (target->vk_format == VK_FORMAT_UNDEFINED || !CreateImage(*target)) {
    REXGPU_ERROR("Native renderer: failed to create a {} render target (base {}, format {}, {}x{}, "
                 "{}x MSAA)",
                 depth ? "depth" : "colour", base_tiles, format, target->surface.width,
                 target->surface.height, uint32_t(target->samples));
    DestroyTarget(*target);
    return nullptr;
  }
  REXGPU_DEBUG("Native renderer: {} render target base {} format {} {}x{} {}x MSAA (host {}x{})",
               depth ? "depth" : "colour", base_tiles, format, target->surface.width,
               target->surface.height, uint32_t(target->samples), target->host_width,
               target->host_height);
  RenderTarget* result = target.get();
  target->id = next_target_id_++;
  targets_.emplace(key, std::move(target));
  return result;
}

RenderTarget* RenderTargets::Find(uint32_t base_tiles, uint32_t format, bool depth, uint32_t pitch,
                                 xenos::MsaaSamples msaa, uint32_t scale4) const {
  if (!tile_count_ || !pitch || msaa > xenos::MsaaSamples::k4X) return nullptr;
  if (!depth) format = uint32_t(KeyColorFormat(xenos::ColorRenderTargetFormat(format)));
  const uint64_t surface_key = SurfaceKey(base_tiles % tile_count_, format, depth, pitch, msaa);
  const uint64_t key =
      surface_key | (uint64_t((scale4 ? scale4 : ScaleFor(surface_key)) & 31) << 34);
  const auto it = targets_.find(key);
  return it == targets_.end() ? nullptr : it->second.get();
}

bool RenderTargets::CreateImage(RenderTarget& target) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = target.vk_format;
  image_info.extent = {target.host_width, target.host_height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = target.samples;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = (target.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                   : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!util::CreateDedicatedAllocationImage(vulkan_device, image_info,
                                            util::MemoryPurpose::kDeviceLocal, target.image,
                                            target.memory)) {
    return false;
  }
  target.layout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = target.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = target.vk_format;
  view_info.subresourceRange = util::InitializeSubresourceRange(target.aspect);
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &target.view) != VK_SUCCESS) {
    return false;
  }

  VkRenderPass render_pass = GetClearRenderPass(target.vk_format, target.samples, target.depth);
  if (render_pass == VK_NULL_HANDLE) {
    return false;
  }
  VkFramebufferCreateInfo framebuffer_info = {};
  framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_info.renderPass = render_pass;
  framebuffer_info.attachmentCount = 1;
  framebuffer_info.pAttachments = &target.view;
  framebuffer_info.width = target.host_width;
  framebuffer_info.height = target.host_height;
  framebuffer_info.layers = 1;
  return dfn.vkCreateFramebuffer(device, &framebuffer_info, nullptr, &target.framebuffer) ==
         VK_SUCCESS;
}

void RenderTargets::DestroyTarget(RenderTarget& target) {
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  if (!vulkan_device) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device, target.framebuffer);
  util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, target.view);
  util::DestroyAndNullHandle(dfn.vkDestroyImage, device, target.image);
  util::DestroyAndNullHandle(dfn.vkFreeMemory, device, target.memory);
}

VkRenderPass RenderTargets::GetClearRenderPass(VkFormat format, VkSampleCountFlagBits samples,
                                               bool depth) {
  uint64_t key = uint64_t(uint32_t(format)) | (uint64_t(samples) << 32) | (uint64_t(depth) << 40);
  auto it = clear_render_passes_.find(key);
  if (it != clear_render_passes_.end()) {
    return it->second;
  }
  // Loads and stores everything: only the clear rectangles change. The layout
  // stays the attachment one; barriers are outside (Transition).
  VkImageLayout layout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                               : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentDescription attachment = {};
  attachment.format = format;
  attachment.samples = samples;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = depth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp =
      depth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = layout;
  attachment.finalLayout = layout;
  VkAttachmentReference reference = {0, layout};
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  if (depth) {
    subpass.pDepthStencilAttachment = &reference;
  } else {
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
  }
  VkRenderPassCreateInfo render_pass_info = {};
  render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_info.attachmentCount = 1;
  render_pass_info.pAttachments = &attachment;
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (vulkan_device->functions().vkCreateRenderPass(vulkan_device->device(), &render_pass_info,
                                                    nullptr, &render_pass) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a clear render pass");
    return VK_NULL_HANDLE;
  }
  clear_render_passes_.emplace(key, render_pass);
  return render_pass;
}

void RenderTargets::Transition(CommandRecorder* command_buffer, RenderTarget& target,
                               VkImageLayout layout) {
  // native_gpu_time_by_kind: a change of layout in an EDRAM transfer or a
  // resolve timed apart from the work around it (a driver may decompress the
  // whole target when it goes to a sampled or copy layout), then that work's
  // kind again.
  GpuKind resume = GpuKind::kCount;
  if (target.layout != layout && submitter_->gpu_time_by_kind()) {
    const GpuKind current = submitter_->gpu_kind();
    const GpuKind layout_kind = GpuLayoutKindOf(current);
    if (layout_kind != GpuKind::kCount) {
      resume = current;
      submitter_->MarkGpuKind(layout_kind);
    }
  }
  // A new one is cleared first (as on any GPU).
  TransitionTrackedImage(submitter_->vulkan_device()->functions(), command_buffer, target.image,
                         target.aspect, target.layout, layout);
  if (resume != GpuKind::kCount) {
    submitter_->MarkGpuKind(resume);
  }
}

VkFramebuffer RenderTargets::GetDrawFramebuffer(
    VkRenderPass render_pass, RenderTarget* const color[xenos::kMaxColorRenderTargets],
    RenderTarget* depth, VkExtent2D extent) {
  static_assert(sizeof(DrawFramebufferKey) ==
                    sizeof(VkRenderPass) + sizeof(VkImageView) * (xenos::kMaxColorRenderTargets + 1) +
                        sizeof(uint32_t) * 2,
                "DrawFramebufferKey is compared and hashed as memory");
  DrawFramebufferKey key;
  std::memset(&key, 0, sizeof(key));
  key.render_pass = render_pass;
  uint32_t attachment_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (color[i]) {
      key.views[attachment_count++] = color[i]->view;
    }
  }
  if (depth) {
    key.views[attachment_count++] = depth->view;
  }
  // None: draws only exporting to memory, rasterized over the extent.
  if (!extent.width || !extent.height) {
    return VK_NULL_HANDLE;
  }
  key.width = extent.width;
  key.height = extent.height;
  uint64_t hash = XXH3_64bits(&key, sizeof(key));
  auto range = draw_framebuffers_.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (!std::memcmp(&it->second.first, &key, sizeof(key))) {
      return it->second.second;
    }
  }
  VkFramebufferCreateInfo framebuffer_info = {};
  framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_info.renderPass = render_pass;
  framebuffer_info.attachmentCount = attachment_count;
  framebuffer_info.pAttachments = attachment_count ? key.views : nullptr;
  framebuffer_info.width = extent.width;
  framebuffer_info.height = extent.height;
  framebuffer_info.layers = 1;
  const ui::vulkan::VulkanDevice* vulkan_device = submitter_->vulkan_device();
  VkFramebuffer framebuffer;
  if (vulkan_device->functions().vkCreateFramebuffer(vulkan_device->device(), &framebuffer_info,
                                                     nullptr, &framebuffer) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a draw framebuffer ({}x{}, {} attachments)",
                 extent.width, extent.height, attachment_count);
    return VK_NULL_HANDLE;
  }
  draw_framebuffers_.emplace(hash, std::make_pair(key, framebuffer));
  return framebuffer;
}

bool RenderTargets::BeginClear(RenderTarget& target, CommandRecorder*& command_buffer_out,
                               const std::vector<VkClearRect>& clear_rects) {
  CommandRecorder* command_buffer = submitter_->OpenOutsideRenderPass(RenderPassEnd::kEdram);
  if (command_buffer == nullptr) {
    return false;
  }
  Transition(command_buffer, target,
             target.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                          : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderPassBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin_info.renderPass = GetClearRenderPass(target.vk_format, target.samples, target.depth);
  begin_info.framebuffer = target.framebuffer;
  begin_info.renderArea.extent = {target.host_width, target.host_height};
  if (REXCVAR_GET(native_pass_area_bounds) && !clear_rects.empty()) {
    // Loads, stores (and on tiling GPUs the tiles) only where it clears.
    int32_t left = INT32_MAX, top = INT32_MAX, right = 0, bottom = 0;
    for (const VkClearRect& clear_rect : clear_rects) {
      left = std::min(left, clear_rect.rect.offset.x);
      top = std::min(top, clear_rect.rect.offset.y);
      right = std::max(right, clear_rect.rect.offset.x + int32_t(clear_rect.rect.extent.width));
      bottom = std::max(bottom, clear_rect.rect.offset.y + int32_t(clear_rect.rect.extent.height));
    }
    begin_info.renderArea.offset = {left, top};
    begin_info.renderArea.extent = {uint32_t(right - left), uint32_t(bottom - top)};
  }
  command_buffer->CmdVkBeginRenderPass(&begin_info, VK_SUBPASS_CONTENTS_INLINE);
  command_buffer_out = command_buffer;
  return true;
}

namespace {

// Guest rectangle -> host clear rectangle of the target; false when empty.
bool HostClearRect(const RenderTarget& target, const Rect& rect, VkClearRect& out) {
  Rect guest = rect;
  guest.left = std::max(guest.left, int32_t(0));
  guest.top = std::max(guest.top, int32_t(0));
  guest.right = std::min(guest.right, int32_t(target.surface.width));
  guest.bottom = std::min(guest.bottom, int32_t(target.surface.height));
  if (guest.empty()) {
    return false;
  }
  Rect host = ScaleRect(guest, target.scale4);
  host.right = std::min(host.right, int32_t(target.host_width));
  host.bottom = std::min(host.bottom, int32_t(target.host_height));
  if (host.empty()) {
    return false;
  }
  out.rect.offset = {host.left, host.top};
  out.rect.extent = {uint32_t(host.right - host.left), uint32_t(host.bottom - host.top)};
  out.baseArrayLayer = 0;
  out.layerCount = 1;
  return true;
}

}  // namespace

void RenderTargets::ClearColor(RenderTarget& target, const Rect& rect, const float color[4]) {
  ClearColor(target, std::vector<Rect>{rect}, color);
}

void RenderTargets::ClearDepthStencil(RenderTarget& target, const Rect& rect, float depth,
                                      uint32_t stencil) {
  ClearDepthStencil(target, std::vector<Rect>{rect}, depth, stencil,
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
}

void RenderTargets::ClearColor(RenderTarget& target, const std::vector<Rect>& rects,
                               const float color[4]) {
  if (target.depth) {
    return;
  }
  std::vector<VkClearRect> clear_rects;
  for (const Rect& rect : rects) {
    VkClearRect clear_rect;
    if (HostClearRect(target, rect, clear_rect)) {
      clear_rects.push_back(clear_rect);
    }
  }
  if (clear_rects.empty()) {
    return;
  }
  CommandRecorder* command_buffer;
  if (!BeginClear(target, command_buffer, clear_rects)) {
    return;
  }
  VkClearAttachment attachment = {};
  attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  attachment.colorAttachment = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    attachment.clearValue.color.float32[i] = std::isnan(color[i]) ? 0.0f : color[i];
  }
  command_buffer->CmdVkClearAttachments(1, &attachment, uint32_t(clear_rects.size()),
                                        clear_rects.data());
  command_buffer->CmdVkEndRenderPass();
}

void RenderTargets::ClearDepthStencil(RenderTarget& target, const std::vector<Rect>& rects,
                                      float depth, uint32_t stencil, VkImageAspectFlags aspects) {
  aspects &= VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  if (!target.depth || !aspects) {
    return;
  }
  std::vector<VkClearRect> clear_rects;
  for (const Rect& rect : rects) {
    VkClearRect clear_rect;
    if (HostClearRect(target, rect, clear_rect)) {
      clear_rects.push_back(clear_rect);
    }
  }
  if (clear_rects.empty()) {
    return;
  }
  CommandRecorder* command_buffer;
  if (!BeginClear(target, command_buffer, clear_rects)) {
    return;
  }
  VkClearAttachment attachment = {};
  attachment.aspectMask = aspects;
  // 20e4 reaches 2; host depth stays 0..1 (VK_EXT_depth_range_unrestricted is
  // not required).
  attachment.clearValue.depthStencil.depth = std::isnan(depth) ? 0.0f : std::clamp(depth, 0.0f, 1.0f);
  attachment.clearValue.depthStencil.stencil = stencil & 0xFF;
  command_buffer->CmdVkClearAttachments(1, &attachment, uint32_t(clear_rects.size()),
                                        clear_rects.data());
  command_buffer->CmdVkEndRenderPass();
}

const char* RenderTargets::ClaimCallerName(ClaimCaller caller) {
  switch (caller) {
    case ClaimCaller::kDraw:
      return "draw";
    case ClaimCaller::kDrawClearRect:
      return "clear rectangle draw";
    case ClaimCaller::kResolve:
      return "resolve";
    case ClaimCaller::kResolveClear:
      return "resolve clear";
    default:
      return "?";
  }
}

void RenderTargets::Claim(RenderTarget& target, const Rect& rect, uint32_t overwrite,
                          ClaimCaller caller) {
  // Most claims (99.5 % of ~12,000 a frame in RDR) find the target still
  // owning the rectangle: nothing to set up, record or log.
  if (edram_.ClaimIsKept(target, rect)) {
    if (profile_transfers_) {
      ++claim_stats_.claims;
      ++claim_stats_.no_ops;
      ++claim_stats_.by_caller[std::min(size_t(caller), size_t(ClaimCaller::kCount) - 1)]
                             [target.depth ? 1 : 0]
                                 .claims;
    }
    return;
  }
  claim_ops_.clear();
  edram_.SetUseCopies(REXCVAR_GET(native_edram_valid_copies));
  const uint64_t kept_tiles_before = edram_.copy_kept_tiles();
  edram_.Claim(target, rect, overwrite, claim_ops_);
  if (profile_transfers_) {
    claim_stats_.kept_tiles += edram_.copy_kept_tiles() - kept_tiles_before;
    ++claim_stats_.claims;
    claim_stats_.no_ops += claim_ops_.empty();
    ClaimStats::ByCaller& by_caller =
        claim_stats_.by_caller[std::min(size_t(caller), size_t(ClaimCaller::kCount) - 1)]
                              [target.depth ? 1 : 0];
    ++by_caller.claims;
    // Count ownership's requested operations, even if native_edram_transfers is
    // off. Rect area is guest pixels with repetitions, not unique bytes or GPU time.
    for (const EdramOp& op : claim_ops_) {
      if (op.kind == EdramOp::Kind::kClear) {
        ++claim_stats_.clears;
        ++by_caller.clears;
      } else {
        ++claim_stats_.transfers;
        ++by_caller.transfers;
        claim_stats_.transfer_rects += op.rects.size();
        claim_stats_.reinterpret += op.src->depth != op.dst->depth || op.src->format != op.dst->format;
        claim_stats_.stencil += bool(op.parts & kEdramPartStencil);
        for (const Rect& area : op.rects) {
          const uint64_t pixels = uint64_t(std::max(area.right - area.left, 0)) *
                                  uint64_t(std::max(area.bottom - area.top, 0));
          claim_stats_.transfer_guest_pixels += pixels;
          by_caller.transfer_guest_pixels += pixels;
        }
      }
    }
  }
  if (REXCVAR_GET(native_log_edram)) {
    for (const EdramOp& op : claim_ops_) {
      Rect bounds = op.rects.empty() ? Rect() : op.rects.front();
      for (const Rect& op_rect : op.rects) {
        bounds.left = std::min(bounds.left, op_rect.left);
        bounds.top = std::min(bounds.top, op_rect.top);
        bounds.right = std::max(bounds.right, op_rect.right);
        bounds.bottom = std::max(bounds.bottom, op_rect.bottom);
      }
      REXGPU_INFO(
          "EDRAM log: {} into {} base {} format {} pitch {} MSAA {} ({}x{}) from {} base {} "
          "format {} pitch {} MSAA {} ({}x{}), parts {}, raw {:08X}, {} rectangles within "
          "{},{}-{},{} (claimed {},{}-{},{}, overwriting {}, by a {})",
          op.kind == EdramOp::Kind::kClear ? "clear" : "transfer",
          op.dst->depth ? "depth" : "colour", op.dst->surface.base_tiles, op.dst->format,
          op.dst->surface.pitch_pixels, uint32_t(op.dst->surface.msaa), op.dst->surface.width,
          op.dst->surface.height, op.src->depth ? "depth" : "colour", op.src->surface.base_tiles,
          op.src->format, op.src->surface.pitch_pixels, uint32_t(op.src->surface.msaa),
          op.src->surface.width, op.src->surface.height, op.parts, op.raw, op.rects.size(),
          bounds.left, bounds.top, bounds.right, bounds.bottom, rect.left, rect.top, rect.right,
          rect.bottom, overwrite, ClaimCallerName(caller));
    }
  }
  if (!REXCVAR_GET(native_edram_transfers)) {
    // Diagnostics: the claim is recorded, its contents not brought in.
    return;
  }
  // native_drop_transfers, parsed again when it changes (hot reload).
  if (const std::string& drop_text = REXCVAR_GET(native_drop_transfers);
      drop_text != drop_transfers_text_) {
    if (!drop_transfers_text_.empty()) {
      REXGPU_INFO("native_drop_transfers \"{}\": {} transfers left out", drop_transfers_text_,
                  dropped_transfers_);
    }
    drop_transfers_text_ = drop_text;
    dropped_transfers_ = 0;
    auto filter = ParseTransferFilter(drop_text);
    if (!filter) {
      REXGPU_WARN("native_drop_transfers \"{}\" is malformed; nothing is left out", drop_text);
    } else if (!filter->empty()) {
      REXGPU_INFO("native_drop_transfers \"{}\": {} rules", drop_text, filter->rules.size());
    }
    drop_transfers_ = filter ? std::move(*filter) : TransferFilter{};
  }
  for (const EdramOp& op : claim_ops_) {
    // Every EdramTarget is a RenderTarget, living until shutdown.
    auto& dst = static_cast<RenderTarget&>(*op.dst);
    auto& src = const_cast<RenderTarget&>(static_cast<const RenderTarget&>(*op.src));
    if (op.kind == EdramOp::Kind::kClear) {
      // The source's raw cell read in the destination's format.
      if (!dst.depth) {
        float color[4];
        UnpackColorRaw(op.raw, ColorRenderTargetFormat(dst.format), color);
        ClearColor(dst, op.rects, color);
      } else {
        VkImageAspectFlags aspects =
            ((op.parts & kEdramPartMain) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
            ((op.parts & kEdramPartStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ClearDepthStencil(dst, op.rects,
                          DecodeDepth24(op.raw >> 8, xenos::DepthRenderTargetFormat(dst.format)),
                          op.raw & 0xFF, aspects);
      }
    } else if (transfers_) {
      if (!drop_transfers_.empty() &&
          drop_transfers_.Drops(src.depth, src.format, 1u << uint32_t(src.surface.msaa), dst.depth,
                                dst.format, 1u << uint32_t(dst.surface.msaa))) {
        ++dropped_transfers_;
        continue;
      }
      transfers_->Transfer(dst, src, op.parts, op.rects);
      NoteGraphicsStateChange();
    }
  }
}

}  // namespace rex::graphics::native
