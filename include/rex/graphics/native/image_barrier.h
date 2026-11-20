/**
 * @file        graphics/native/image_barrier.h
 * @brief       Native renderer: image layout transitions
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/graphics/native/command_recorder.h>
#include <rex/cvar.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/device.h>

// command_processor.cpp: new images opaque magenta instead of zero.
REXCVAR_DECLARE(bool, native_poison_new_images);
// command_processor.cpp: resolves and EDRAM claim operations logged.
REXCVAR_DECLARE(bool, native_log_edram);

namespace rex::graphics::native {

// A barrier between everything before and everything after on the image, with
// a layout change (or none: old == new still orders the accesses). Correct for
// any pair of uses; narrower stage and access masks are for milestone N7.
inline void ImageBarrier(const ui::vulkan::VulkanDevice::Functions& dfn,
                         CommandRecorder* command_buffer, VkImage image, VkImageAspectFlags aspect,
                         VkImageLayout old_layout, VkImageLayout new_layout) {
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = aspect;
  barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
  barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
  command_buffer->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                       nullptr, 1, &barrier);
}

// An image's barrier from its tracked layout to new_layout, which becomes the
// tracked one. A new image (VK_IMAGE_LAYOUT_UNDEFINED) is cleared first: its
// memory may hold anything (AMD and the Steam Deck hand back old pictures
// where NVIDIA usually gives zeros), so what nothing writes would read
// differently on each GPU. Colour becomes zero, or opaque magenta with
// native_poison_new_images (so such a read shows on any GPU); depth and
// stencil zero. Outside a render pass; the image is a transfer destination
// of an uncompressed format.
inline void TransitionTrackedImage(const ui::vulkan::VulkanDevice::Functions& dfn,
                                   CommandRecorder* command_buffer, VkImage image,
                                   VkImageAspectFlags aspect, VkImageLayout& layout,
                                   VkImageLayout new_layout) {
  if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
    ImageBarrier(dfn, command_buffer, image, aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange range = {};
    range.aspectMask = aspect;
    range.levelCount = VK_REMAINING_MIP_LEVELS;
    range.layerCount = VK_REMAINING_ARRAY_LAYERS;
    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
      VkClearColorValue color = {};
      if (REXCVAR_GET(native_poison_new_images)) {
        color.float32[0] = 1.0f;
        color.float32[2] = 1.0f;
        color.float32[3] = 1.0f;
      }
      command_buffer->CmdVkClearColorImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                                           &range);
    } else {
      VkClearDepthStencilValue depth_stencil = {};
      command_buffer->CmdVkClearDepthStencilImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                  &depth_stencil, 1, &range);
    }
    layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  }
  ImageBarrier(dfn, command_buffer, image, aspect, layout, new_layout);
  layout = new_layout;
}

}  // namespace rex::graphics::native
