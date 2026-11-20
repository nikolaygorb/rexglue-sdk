#pragma once
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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/assert.h>
#include <rex/math.h>
#include <rex/ui/gpu_breadcrumbs.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/device.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

class DeferredCommandBuffer {
 public:
  DeferredCommandBuffer(const VulkanCommandProcessor& command_processor,
                        size_t initial_size_bytes = 1024 * 1024);
  // For a user other than VulkanCommandProcessor (the native renderer): the
  // commands are replayed with this device's functions.
  explicit DeferredCommandBuffer(const ui::vulkan::VulkanDevice* vulkan_device,
                                 size_t initial_size_bytes = 1024 * 1024);

  void Reset();
  void CmdBreadcrumb(uint32_t sequence) {
    if (sequence) *reinterpret_cast<uint32_t*>(WriteCommand(Command::kBreadcrumb, sizeof(uint32_t))) = sequence;
  }
  // Device-loss breadcrumbs (ReIGlue 1100): the second marker, written by the
  // GPU once the work recorded before the draw numbered `sequence` finished.
  void CmdSetupBreadcrumb(uint32_t sequence) {
    if (sequence) {
      *reinterpret_cast<uint32_t*>(WriteCommand(Command::kSetupBreadcrumb, sizeof(uint32_t))) =
          sequence;
    }
  }
  // Counts the commands written to the main stream by kind while on (with
  // device-loss breadcrumbs); TakeBreadcrumbWork returns the counts since its
  // last call and starts again. The hoisted stream is not counted.
  void SetCountBreadcrumbWork(bool count) { count_breadcrumb_work_ = count; }
  ui::GpuBreadcrumbWork TakeBreadcrumbWork() {
    const ui::GpuBreadcrumbWork work = breadcrumb_work_;
    breadcrumb_work_ = ui::GpuBreadcrumbWork();
    return work;
  }
  void Execute(VkCommandBuffer command_buffer);

  // The recorded commands, taken out for another thread to execute (threaded
  // submission).
  struct Streams {
    std::vector<uintmax_t> main;
    std::vector<uintmax_t> hoisted;
  };
  // Swaps the recorded commands with `streams` (normally empty ones, whose
  // allocations are reused) and starts recording anew.
  void TakeStreams(Streams& streams) {
    command_stream_.swap(streams.main);
    hoisted_stream_.swap(streams.hoisted);
    Reset();
  }
  // Executes commands taken with TakeStreams. Touches only the replay's scratch
  // vectors, so it may run on another thread while new commands are recorded.
  void ExecuteStreams(VkCommandBuffer command_buffer, const Streams& streams) const;

  // Hoisting: commands recorded later but executed earlier, at a hoist point
  // written into the stream before (the shared memory uploads that
  // VulkanSharedMemory moves before the current render pass instead of ending
  // it). The hoisted commands go to a second stream; a hoist point covers a
  // contiguous range of it, which Execute replays where the hoist point is.
  // Writes a hoist point covering nothing yet, and returns its handle.
  size_t CmdHoistPoint();
  // Directs the commands written from now on to the hoisted stream (true) or
  // the main one.
  void SetWritingHoisted(bool hoisted) { write_stream_ = hoisted ? &hoisted_stream_ : &command_stream_; }
  size_t hoisted_stream_size() const { return hoisted_stream_.size(); }
  // The size of the stream commands are written to now.
  size_t write_stream_size() const { return write_stream_->size(); }
  bool is_writing_hoisted() const { return write_stream_ == &hoisted_stream_; }
  // How much has been recorded in the main stream since Reset.
  size_t stream_size() const { return command_stream_.size(); }
  // Drops the hoisted commands from `size` on (only those of the latest hoist
  // point).
  void TruncateHoistedStream(size_t size) {
    assert_true(size <= hoisted_stream_.size());
    hoisted_stream_.resize(size);
  }
  // Makes the hoist point cover the hoisted stream from where it was at
  // CmdHoistPoint to its current end.
  void ExtendHoistPoint(size_t hoist_point);

  // render_pass_begin->pNext must be null.
  void CmdVkBeginRenderPass(const VkRenderPassBeginInfo* render_pass_begin,
                            VkSubpassContents contents,
                            const VkSampleLocationsInfoEXT* sample_locations = nullptr,
                            bool has_depth_attachment = false) {
    assert_null(render_pass_begin->pNext);
    size_t arguments_size = sizeof(ArgsVkBeginRenderPass);
    uint32_t clear_value_count = render_pass_begin->clearValueCount;
    size_t clear_values_offset = 0;
    if (clear_value_count) {
      arguments_size = rex::align(arguments_size, alignof(VkClearValue));
      clear_values_offset = arguments_size;
      arguments_size += sizeof(VkClearValue) * clear_value_count;
    }
    size_t sample_locations_offset = 0;
    if (sample_locations) {
      arguments_size = rex::align(arguments_size, alignof(VkSampleLocationEXT));
      sample_locations_offset = arguments_size;
      arguments_size += sizeof(VkSampleLocationEXT) * sample_locations->sampleLocationsCount;
    }
    uint8_t* args_ptr =
        reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkBeginRenderPass, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkBeginRenderPass*>(args_ptr);
    args.render_pass = render_pass_begin->renderPass;
    args.framebuffer = render_pass_begin->framebuffer;
    args.render_area = render_pass_begin->renderArea;
    args.clear_value_count = clear_value_count;
    args.contents = contents;
    args.sample_locations_count = sample_locations ? sample_locations->sampleLocationsCount : 0;
    args.has_depth_attachment = has_depth_attachment;
    if (sample_locations) {
      args.sample_locations_per_pixel = sample_locations->sampleLocationsPerPixel;
      args.sample_location_grid_size = sample_locations->sampleLocationGridSize;
      std::memcpy(args_ptr + sample_locations_offset, sample_locations->pSampleLocations,
                  sizeof(VkSampleLocationEXT) * args.sample_locations_count);
    }
    if (clear_value_count) {
      std::memcpy(args_ptr + clear_values_offset, render_pass_begin->pClearValues,
                  sizeof(VkClearValue) * clear_value_count);
    }
  }

  void CmdVkBindDescriptorSets(VkPipelineBindPoint pipeline_bind_point, VkPipelineLayout layout,
                               uint32_t first_set, uint32_t descriptor_set_count,
                               const VkDescriptorSet* descriptor_sets,
                               uint32_t dynamic_offset_count, const uint32_t* dynamic_offsets) {
    size_t arguments_size = rex::align(sizeof(ArgsVkBindDescriptorSets), alignof(VkDescriptorSet));
    size_t descriptor_sets_offset = arguments_size;
    arguments_size += sizeof(VkDescriptorSet) * descriptor_set_count;
    size_t dynamic_offsets_offset = 0;
    if (dynamic_offset_count) {
      arguments_size = rex::align(arguments_size, alignof(uint32_t));
      dynamic_offsets_offset = arguments_size;
      arguments_size += sizeof(uint32_t) * dynamic_offset_count;
    }
    uint8_t* args_ptr =
        reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkBindDescriptorSets, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkBindDescriptorSets*>(args_ptr);
    args.pipeline_bind_point = pipeline_bind_point;
    args.layout = layout;
    args.first_set = first_set;
    args.descriptor_set_count = descriptor_set_count;
    args.dynamic_offset_count = dynamic_offset_count;
    std::memcpy(args_ptr + descriptor_sets_offset, descriptor_sets,
                sizeof(VkDescriptorSet) * descriptor_set_count);
    if (dynamic_offset_count) {
      std::memcpy(args_ptr + dynamic_offsets_offset, dynamic_offsets,
                  sizeof(uint32_t) * dynamic_offset_count);
    }
  }

  void CmdVkBindIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType index_type) {
    auto& args = *reinterpret_cast<ArgsVkBindIndexBuffer*>(
        WriteCommand(Command::kVkBindIndexBuffer, sizeof(ArgsVkBindIndexBuffer)));
    args.buffer = buffer;
    args.offset = offset;
    args.index_type = index_type;
  }

  void CmdVkBindPipeline(VkPipelineBindPoint pipeline_bind_point, VkPipeline pipeline) {
    auto& args = *reinterpret_cast<ArgsVkBindPipeline*>(
        WriteCommand(Command::kVkBindPipeline, sizeof(ArgsVkBindPipeline)));
    args.pipeline_bind_point = pipeline_bind_point;
    args.pipeline = pipeline;
  }

  void CmdVkBindVertexBuffers(uint32_t first_binding, uint32_t binding_count,
                              const VkBuffer* buffers, const VkDeviceSize* offsets) {
    size_t arguments_size = rex::align(sizeof(ArgsVkBindVertexBuffers), alignof(VkBuffer));
    size_t buffers_offset = arguments_size;
    arguments_size =
        rex::align(arguments_size + sizeof(VkBuffer) * binding_count, alignof(VkDeviceSize));
    size_t offsets_offset = arguments_size;
    arguments_size += sizeof(VkDeviceSize) * binding_count;
    uint8_t* args_ptr =
        reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkBindVertexBuffers, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkBindVertexBuffers*>(args_ptr);
    args.first_binding = first_binding;
    args.binding_count = binding_count;
    std::memcpy(args_ptr + buffers_offset, buffers, sizeof(VkBuffer) * binding_count);
    std::memcpy(args_ptr + offsets_offset, offsets, sizeof(VkDeviceSize) * binding_count);
  }

  void CmdClearAttachmentsEmplace(uint32_t attachment_count, VkClearAttachment*& attachments_out,
                                  uint32_t rect_count, VkClearRect*& rects_out) {
    size_t arguments_size = rex::align(sizeof(ArgsVkClearAttachments), alignof(VkClearAttachment));
    size_t attachments_offset = arguments_size;
    arguments_size = rex::align(arguments_size + sizeof(VkClearAttachment) * attachment_count,
                                alignof(VkClearRect));
    size_t rects_offset = arguments_size;
    arguments_size += sizeof(VkClearRect) * rect_count;
    uint8_t* args_ptr =
        reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkClearAttachments, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkClearAttachments*>(args_ptr);
    args.attachment_count = attachment_count;
    args.rect_count = rect_count;
    attachments_out = reinterpret_cast<VkClearAttachment*>(args_ptr + attachments_offset);
    rects_out = reinterpret_cast<VkClearRect*>(args_ptr + rects_offset);
  }
  void CmdVkClearAttachments(uint32_t attachment_count, const VkClearAttachment* attachments,
                             uint32_t rect_count, const VkClearRect* rects) {
    VkClearAttachment* attachments_arg;
    VkClearRect* rects_arg;
    CmdClearAttachmentsEmplace(attachment_count, attachments_arg, rect_count, rects_arg);
    std::memcpy(attachments_arg, attachments, sizeof(VkClearAttachment) * attachment_count);
    std::memcpy(rects_arg, rects, sizeof(VkClearRect) * rect_count);
  }

  VkImageSubresourceRange* CmdClearColorImageEmplace(VkImage image, VkImageLayout image_layout,
                                                     const VkClearColorValue* color,
                                                     uint32_t range_count) {
    const size_t header_size =
        rex::align(sizeof(ArgsVkClearColorImage), alignof(VkImageSubresourceRange));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(WriteCommand(
        Command::kVkClearColorImage, header_size + sizeof(VkImageSubresourceRange) * range_count));
    auto& args = *reinterpret_cast<ArgsVkClearColorImage*>(args_ptr);
    args.image = image;
    args.image_layout = image_layout;
    args.color = *color;
    args.range_count = range_count;
    return reinterpret_cast<VkImageSubresourceRange*>(args_ptr + header_size);
  }
  void CmdVkClearColorImage(VkImage image, VkImageLayout image_layout,
                            const VkClearColorValue* color, uint32_t range_count,
                            const VkImageSubresourceRange* ranges) {
    std::memcpy(CmdClearColorImageEmplace(image, image_layout, color, range_count), ranges,
                sizeof(VkImageSubresourceRange) * range_count);
  }

  void CmdVkClearDepthStencilImage(VkImage image, VkImageLayout image_layout,
                                   const VkClearDepthStencilValue* depth_stencil,
                                   uint32_t range_count, const VkImageSubresourceRange* ranges) {
    const size_t header_size =
        rex::align(sizeof(ArgsVkClearDepthStencilImage), alignof(VkImageSubresourceRange));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkClearDepthStencilImage,
                     header_size + sizeof(VkImageSubresourceRange) * range_count));
    auto& args = *reinterpret_cast<ArgsVkClearDepthStencilImage*>(args_ptr);
    args.image = image;
    args.image_layout = image_layout;
    args.depth_stencil = *depth_stencil;
    args.range_count = range_count;
    std::memcpy(args_ptr + header_size, ranges, sizeof(VkImageSubresourceRange) * range_count);
  }

  VkBufferCopy* CmdCopyBufferEmplace(VkBuffer src_buffer, VkBuffer dst_buffer,
                                     uint32_t region_count) {
    const size_t header_size = rex::align(sizeof(ArgsVkCopyBuffer), alignof(VkBufferCopy));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkCopyBuffer, header_size + sizeof(VkBufferCopy) * region_count));
    auto& args = *reinterpret_cast<ArgsVkCopyBuffer*>(args_ptr);
    args.src_buffer = src_buffer;
    args.dst_buffer = dst_buffer;
    args.region_count = region_count;
    return reinterpret_cast<VkBufferCopy*>(args_ptr + header_size);
  }
  void CmdVkCopyBuffer(VkBuffer src_buffer, VkBuffer dst_buffer, uint32_t region_count,
                       const VkBufferCopy* regions) {
    std::memcpy(CmdCopyBufferEmplace(src_buffer, dst_buffer, region_count), regions,
                sizeof(VkBufferCopy) * region_count);
  }

  void CmdVkBeginQuery(VkQueryPool query_pool, uint32_t query, VkQueryControlFlags flags) {
    auto& args = *reinterpret_cast<ArgsVkBeginQuery*>(
        WriteCommand(Command::kVkBeginQuery, sizeof(ArgsVkBeginQuery)));
    args.query_pool = query_pool;
    args.query = query;
    args.flags = flags;
  }

  VkBufferImageCopy* CmdCopyBufferToImageEmplace(VkBuffer src_buffer, VkImage dst_image,
                                                 VkImageLayout dst_image_layout,
                                                 uint32_t region_count) {
    const size_t header_size =
        rex::align(sizeof(ArgsVkCopyBufferToImage), alignof(VkBufferImageCopy));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(WriteCommand(
        Command::kVkCopyBufferToImage, header_size + sizeof(VkBufferImageCopy) * region_count));
    auto& args = *reinterpret_cast<ArgsVkCopyBufferToImage*>(args_ptr);
    args.src_buffer = src_buffer;
    args.dst_image = dst_image;
    args.dst_image_layout = dst_image_layout;
    args.region_count = region_count;
    return reinterpret_cast<VkBufferImageCopy*>(args_ptr + header_size);
  }
  void CmdVkCopyBufferToImage(VkBuffer src_buffer, VkImage dst_image,
                              VkImageLayout dst_image_layout, uint32_t region_count,
                              const VkBufferImageCopy* regions) {
    std::memcpy(CmdCopyBufferToImageEmplace(src_buffer, dst_image, dst_image_layout, region_count),
                regions, sizeof(VkBufferImageCopy) * region_count);
  }

  VkBufferImageCopy* CmdCopyImageToBufferEmplace(VkImage src_image,
                                                 VkImageLayout src_image_layout,
                                                 VkBuffer dst_buffer, uint32_t region_count) {
    const size_t header_size =
        rex::align(sizeof(ArgsVkCopyImageToBuffer), alignof(VkBufferImageCopy));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(WriteCommand(
        Command::kVkCopyImageToBuffer, header_size + sizeof(VkBufferImageCopy) * region_count));
    auto& args = *reinterpret_cast<ArgsVkCopyImageToBuffer*>(args_ptr);
    args.src_image = src_image;
    args.src_image_layout = src_image_layout;
    args.dst_buffer = dst_buffer;
    args.region_count = region_count;
    return reinterpret_cast<VkBufferImageCopy*>(args_ptr + header_size);
  }

  VkImageCopy* CmdCopyImageEmplace(VkImage src_image, VkImageLayout src_image_layout,
                                   VkImage dst_image, VkImageLayout dst_image_layout,
                                   uint32_t region_count) {
    const size_t header_size = rex::align(sizeof(ArgsVkCopyImage), alignof(VkImageCopy));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkCopyImage, header_size + sizeof(VkImageCopy) * region_count));
    auto& args = *reinterpret_cast<ArgsVkCopyImage*>(args_ptr);
    args.src_image = src_image;
    args.src_image_layout = src_image_layout;
    args.dst_image = dst_image;
    args.dst_image_layout = dst_image_layout;
    args.region_count = region_count;
    return reinterpret_cast<VkImageCopy*>(args_ptr + header_size);
  }

  void CmdVkCopyImage(VkImage src_image, VkImageLayout src_image_layout, VkImage dst_image,
                      VkImageLayout dst_image_layout, uint32_t region_count,
                      const VkImageCopy* regions) {
    std::memcpy(CmdCopyImageEmplace(src_image, src_image_layout, dst_image, dst_image_layout,
                                    region_count),
                regions, sizeof(VkImageCopy) * region_count);
  }

  void CmdVkResolveImage(VkImage src_image, VkImageLayout src_image_layout, VkImage dst_image,
                         VkImageLayout dst_image_layout, uint32_t region_count,
                         const VkImageResolve* regions) {
    const size_t header_size = rex::align(sizeof(ArgsVkResolveImage), alignof(VkImageResolve));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(WriteCommand(
        Command::kVkResolveImage, header_size + sizeof(VkImageResolve) * region_count));
    auto& args = *reinterpret_cast<ArgsVkResolveImage*>(args_ptr);
    args.src_image = src_image;
    args.src_image_layout = src_image_layout;
    args.dst_image = dst_image;
    args.dst_image_layout = dst_image_layout;
    args.region_count = region_count;
    std::memcpy(args_ptr + header_size, regions, sizeof(VkImageResolve) * region_count);
  }

  void CmdVkBlitImage(VkImage src_image, VkImageLayout src_image_layout, VkImage dst_image,
                      VkImageLayout dst_image_layout, uint32_t region_count,
                      const VkImageBlit* regions, VkFilter filter) {
    const size_t header_size = rex::align(sizeof(ArgsVkBlitImage), alignof(VkImageBlit));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkBlitImage, header_size + sizeof(VkImageBlit) * region_count));
    auto& args = *reinterpret_cast<ArgsVkBlitImage*>(args_ptr);
    args.src_image = src_image;
    args.src_image_layout = src_image_layout;
    args.dst_image = dst_image;
    args.dst_image_layout = dst_image_layout;
    args.region_count = region_count;
    args.filter = filter;
    std::memcpy(args_ptr + header_size, regions, sizeof(VkImageBlit) * region_count);
  }

  // data_size: at most 65536, a multiple of 4 (as vkCmdUpdateBuffer).
  void CmdVkUpdateBuffer(VkBuffer dst_buffer, VkDeviceSize dst_offset, VkDeviceSize data_size,
                         const void* data) {
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkUpdateBuffer, sizeof(ArgsVkUpdateBuffer) + size_t(data_size)));
    auto& args = *reinterpret_cast<ArgsVkUpdateBuffer*>(args_ptr);
    args.dst_buffer = dst_buffer;
    args.dst_offset = dst_offset;
    args.data_size = data_size;
    std::memcpy(args_ptr + sizeof(ArgsVkUpdateBuffer), data, size_t(data_size));
  }

  // VK_KHR_push_descriptor. The writes' image, buffer or texel buffer view
  // arrays are copied; their pNext must be null.
  void CmdVkPushDescriptorSetKHR(VkPipelineBindPoint pipeline_bind_point, VkPipelineLayout layout,
                                 uint32_t set, uint32_t descriptor_write_count,
                                 const VkWriteDescriptorSet* descriptor_writes);

  void CmdVkWriteTimestamp(VkPipelineStageFlagBits pipeline_stage, VkQueryPool query_pool,
                           uint32_t query) {
    auto& args = *reinterpret_cast<ArgsVkWriteTimestamp*>(
        WriteCommand(Command::kVkWriteTimestamp, sizeof(ArgsVkWriteTimestamp)));
    args.pipeline_stage = pipeline_stage;
    args.query_pool = query_pool;
    args.query = query;
  }

  void CmdVkCopyQueryPoolResults(VkQueryPool query_pool, uint32_t first_query, uint32_t query_count,
                                 VkBuffer dst_buffer, VkDeviceSize dst_offset, VkDeviceSize stride,
                                 VkQueryResultFlags flags) {
    auto& args = *reinterpret_cast<ArgsVkCopyQueryPoolResults*>(
        WriteCommand(Command::kVkCopyQueryPoolResults, sizeof(ArgsVkCopyQueryPoolResults)));
    args.query_pool = query_pool;
    args.first_query = first_query;
    args.query_count = query_count;
    args.dst_buffer = dst_buffer;
    args.dst_offset = dst_offset;
    args.stride = stride;
    args.flags = flags;
  }

  void CmdVkDispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) {
    auto& args = *reinterpret_cast<ArgsVkDispatch*>(
        WriteCommand(Command::kVkDispatch, sizeof(ArgsVkDispatch)));
    args.group_count_x = group_count_x;
    args.group_count_y = group_count_y;
    args.group_count_z = group_count_z;
  }

  void CmdVkDraw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                 uint32_t first_instance) {
    auto& args = *reinterpret_cast<ArgsVkDraw*>(WriteCommand(Command::kVkDraw, sizeof(ArgsVkDraw)));
    args.vertex_count = vertex_count;
    args.instance_count = instance_count;
    args.first_vertex = first_vertex;
    args.first_instance = first_instance;
  }

  void CmdVkDrawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                        int32_t vertex_offset, uint32_t first_instance) {
    auto& args = *reinterpret_cast<ArgsVkDrawIndexed*>(
        WriteCommand(Command::kVkDrawIndexed, sizeof(ArgsVkDrawIndexed)));
    args.index_count = index_count;
    args.instance_count = instance_count;
    args.first_index = first_index;
    args.vertex_offset = vertex_offset;
    args.first_instance = first_instance;
  }

  void CmdVkEndQuery(VkQueryPool query_pool, uint32_t query) {
    auto& args = *reinterpret_cast<ArgsVkEndQuery*>(
        WriteCommand(Command::kVkEndQuery, sizeof(ArgsVkEndQuery)));
    args.query_pool = query_pool;
    args.query = query;
  }

  void CmdVkEndRenderPass() { WriteCommand(Command::kVkEndRenderPass, 0); }

  // Dynamic rendering (VK_KHR_dynamic_rendering / Vulkan 1.3).
  void CmdVkBeginRendering(const VkRenderingInfo* rendering_info);
  void CmdVkEndRendering() { WriteCommand(Command::kVkEndRendering, 0); }

  void CmdVkResetQueryPool(VkQueryPool query_pool, uint32_t first_query, uint32_t query_count) {
    auto& args = *reinterpret_cast<ArgsVkResetQueryPool*>(
        WriteCommand(Command::kVkResetQueryPool, sizeof(ArgsVkResetQueryPool)));
    args.query_pool = query_pool;
    args.first_query = first_query;
    args.query_count = query_count;
  }

  void CmdVkBeginConditionalRenderingEXT(VkBuffer buffer, VkDeviceSize offset,
                                         VkConditionalRenderingFlagsEXT flags) {
    auto& args = *reinterpret_cast<ArgsVkBeginConditionalRenderingEXT*>(WriteCommand(
        Command::kVkBeginConditionalRenderingEXT, sizeof(ArgsVkBeginConditionalRenderingEXT)));
    args.buffer = buffer;
    args.offset = offset;
    args.flags = flags;
  }

  void CmdVkEndConditionalRenderingEXT() {
    WriteCommand(Command::kVkEndConditionalRenderingEXT, 0);
  }

  void CmdVkFillBuffer(VkBuffer dst_buffer, VkDeviceSize dst_offset, VkDeviceSize size,
                       uint32_t data) {
    auto& args = *reinterpret_cast<ArgsVkFillBuffer*>(
        WriteCommand(Command::kVkFillBuffer, sizeof(ArgsVkFillBuffer)));
    args.dst_buffer = dst_buffer;
    args.dst_offset = dst_offset;
    args.size = size;
    args.data = data;
  }

  // pNext of all barriers must be null, except for VkSampleLocationsInfoEXT.
  void CmdVkPipelineBarrier(VkPipelineStageFlags src_stage_mask,
                            VkPipelineStageFlags dst_stage_mask, VkDependencyFlags dependency_flags,
                            uint32_t memory_barrier_count, const VkMemoryBarrier* memory_barriers,
                            uint32_t buffer_memory_barrier_count,
                            const VkBufferMemoryBarrier* buffer_memory_barriers,
                            uint32_t image_memory_barrier_count,
                            const VkImageMemoryBarrier* image_memory_barriers);

  void CmdVkPushConstants(VkPipelineLayout layout, VkShaderStageFlags stage_flags, uint32_t offset,
                          uint32_t size, const void* values) {
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkPushConstants, sizeof(ArgsVkPushConstants) + size));
    auto& args = *reinterpret_cast<ArgsVkPushConstants*>(args_ptr);
    args.layout = layout;
    args.stage_flags = stage_flags;
    args.offset = offset;
    args.size = size;
    std::memcpy(args_ptr + sizeof(ArgsVkPushConstants), values, size);
  }

  void CmdVkSetBlendConstants(const float* blend_constants) {
    auto& args = *reinterpret_cast<ArgsVkSetBlendConstants*>(
        WriteCommand(Command::kVkSetBlendConstants, sizeof(ArgsVkSetBlendConstants)));
    std::memcpy(args.blend_constants, blend_constants, sizeof(float) * 4);
  }

  void CmdVkSetDepthBias(float depth_bias_constant_factor, float depth_bias_clamp,
                         float depth_bias_slope_factor) {
    auto& args = *reinterpret_cast<ArgsVkSetDepthBias*>(
        WriteCommand(Command::kVkSetDepthBias, sizeof(ArgsVkSetDepthBias)));
    args.depth_bias_constant_factor = depth_bias_constant_factor;
    args.depth_bias_clamp = depth_bias_clamp;
    args.depth_bias_slope_factor = depth_bias_slope_factor;
  }

  // VK_EXT_depth_bias_control.
  void CmdVkSetDepthBias2EXT(float depth_bias_constant_factor, float depth_bias_clamp,
                             float depth_bias_slope_factor,
                             VkDepthBiasRepresentationEXT depth_bias_representation,
                             bool depth_bias_exact) {
    auto& args = *reinterpret_cast<ArgsVkSetDepthBias2EXT*>(
        WriteCommand(Command::kVkSetDepthBias2EXT, sizeof(ArgsVkSetDepthBias2EXT)));
    args.depth_bias_constant_factor = depth_bias_constant_factor;
    args.depth_bias_clamp = depth_bias_clamp;
    args.depth_bias_slope_factor = depth_bias_slope_factor;
    args.depth_bias_representation = depth_bias_representation;
    args.depth_bias_exact = depth_bias_exact ? VK_TRUE : VK_FALSE;
  }

  void CmdVkSetScissor(uint32_t first_scissor, uint32_t scissor_count, const VkRect2D* scissors) {
    const size_t header_size = rex::align(sizeof(ArgsVkSetScissor), alignof(VkRect2D));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkSetScissor, header_size + sizeof(VkRect2D) * scissor_count));
    auto& args = *reinterpret_cast<ArgsVkSetScissor*>(args_ptr);
    args.first_scissor = first_scissor;
    args.scissor_count = scissor_count;
    std::memcpy(args_ptr + header_size, scissors, sizeof(VkRect2D) * scissor_count);
  }

  void CmdVkSetStencilCompareMask(VkStencilFaceFlags face_mask, uint32_t compare_mask) {
    auto& args = *reinterpret_cast<ArgsSetStencilMaskReference*>(
        WriteCommand(Command::kVkSetStencilCompareMask, sizeof(ArgsSetStencilMaskReference)));
    args.face_mask = face_mask;
    args.mask_reference = compare_mask;
  }

  void CmdVkSetStencilReference(VkStencilFaceFlags face_mask, uint32_t reference) {
    auto& args = *reinterpret_cast<ArgsSetStencilMaskReference*>(
        WriteCommand(Command::kVkSetStencilReference, sizeof(ArgsSetStencilMaskReference)));
    args.face_mask = face_mask;
    args.mask_reference = reference;
  }

  void CmdVkSetStencilWriteMask(VkStencilFaceFlags face_mask, uint32_t write_mask) {
    auto& args = *reinterpret_cast<ArgsSetStencilMaskReference*>(
        WriteCommand(Command::kVkSetStencilWriteMask, sizeof(ArgsSetStencilMaskReference)));
    args.face_mask = face_mask;
    args.mask_reference = write_mask;
  }

  void CmdVkSetViewport(uint32_t first_viewport, uint32_t viewport_count,
                        const VkViewport* viewports) {
    const size_t header_size = rex::align(sizeof(ArgsVkSetViewport), alignof(VkViewport));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkSetViewport, header_size + sizeof(VkViewport) * viewport_count));
    auto& args = *reinterpret_cast<ArgsVkSetViewport*>(args_ptr);
    args.first_viewport = first_viewport;
    args.viewport_count = viewport_count;
    std::memcpy(args_ptr + header_size, viewports, sizeof(VkViewport) * viewport_count);
  }

 private:
  enum class Command {
    kBreadcrumb,
    kSetupBreadcrumb,
    kVkBeginRenderPass,
    kVkBeginQuery,
    kVkBindDescriptorSets,
    kVkBindIndexBuffer,
    kVkBindPipeline,
    kVkBindVertexBuffers,
    kVkClearAttachments,
    kVkClearColorImage,
    kVkCopyBuffer,
    kVkCopyBufferToImage,
    kVkCopyImage,
    kVkCopyImageToBuffer,
    kVkCopyQueryPoolResults,
    kVkDispatch,
    kVkDraw,
    kVkDrawIndexed,
    kVkEndQuery,
    kVkEndRenderPass,
    kVkBeginRendering,
    kVkEndRendering,
    kVkPipelineBarrier,
    kVkPushConstants,
    kVkResetQueryPool,
    kVkBeginConditionalRenderingEXT,
    kVkEndConditionalRenderingEXT,
    kVkFillBuffer,
    kVkSetBlendConstants,
    kVkSetDepthBias,
    kVkSetDepthBias2EXT,
    kVkSetScissor,
    kVkSetStencilCompareMask,
    kVkSetStencilReference,
    kVkSetStencilWriteMask,
    kVkSetViewport,
    kVkWriteTimestamp,
    kVkResolveImage,
    kVkBlitImage,
    kVkUpdateBuffer,
    kVkPushDescriptorSetKHR,
    kVkClearDepthStencilImage,
    kHoistedCommands,
  };

  struct CommandHeader {
    Command command;
    uint32_t arguments_size_elements;
  };
  static constexpr size_t kCommandHeaderSizeElements =
      (sizeof(CommandHeader) + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);

  struct ArgsVkBeginRenderPass {
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkRect2D render_area;
    uint32_t clear_value_count;
    VkSubpassContents contents;
    VkSampleCountFlagBits sample_locations_per_pixel;
    VkExtent2D sample_location_grid_size;
    uint32_t sample_locations_count;
    bool has_depth_attachment;
    // Followed by aligned optional VkClearValue[], VkSampleLocationEXT[].
    static_assert(alignof(VkClearValue) <= alignof(uintmax_t));
    static_assert(alignof(VkSampleLocationEXT) <= alignof(uintmax_t));
  };

  struct ArgsVkBeginRendering {
    VkRenderingFlags flags;
    VkRect2D render_area;
    uint32_t layer_count;
    uint32_t view_mask;
    uint32_t color_attachment_count;
    bool has_depth_attachment;
    bool has_stencil_attachment;
    // Followed by:
    // - VkRenderingAttachmentInfo[color_attachment_count]
    // - VkRenderingAttachmentInfo depth (if has_depth_attachment)
    // - VkRenderingAttachmentInfo stencil (if has_stencil_attachment)
    static_assert(alignof(VkRenderingAttachmentInfo) <= alignof(uintmax_t));
  };

  struct ArgsVkBeginQuery {
    VkQueryPool query_pool;
    uint32_t query;
    VkQueryControlFlags flags;
  };

  struct ArgsVkBindDescriptorSets {
    VkPipelineBindPoint pipeline_bind_point;
    VkPipelineLayout layout;
    uint32_t first_set;
    uint32_t descriptor_set_count;
    uint32_t dynamic_offset_count;
    // Followed by aligned VkDescriptorSet[], optional uint32_t[].
    static_assert(alignof(VkDescriptorSet) <= alignof(uintmax_t));
  };

  struct ArgsVkBindIndexBuffer {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkIndexType index_type;
  };

  struct ArgsVkBindPipeline {
    VkPipelineBindPoint pipeline_bind_point;
    VkPipeline pipeline;
  };

  struct ArgsVkBindVertexBuffers {
    uint32_t first_binding;
    uint32_t binding_count;
    // Followed by aligned VkBuffer[], VkDeviceSize[].
    static_assert(alignof(VkBuffer) <= alignof(uintmax_t));
    static_assert(alignof(VkDeviceSize) <= alignof(uintmax_t));
  };

  struct ArgsVkClearAttachments {
    uint32_t attachment_count;
    uint32_t rect_count;
    // Followed by aligned VkClearAttachment[], VkClearRect[].
    static_assert(alignof(VkClearAttachment) <= alignof(uintmax_t));
    static_assert(alignof(VkClearRect) <= alignof(uintmax_t));
  };

  struct ArgsVkClearColorImage {
    VkImage image;
    VkImageLayout image_layout;
    VkClearColorValue color;
    uint32_t range_count;
    // Followed by aligned VkImageSubresourceRange[].
    static_assert(alignof(VkImageSubresourceRange) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyBuffer {
    VkBuffer src_buffer;
    VkBuffer dst_buffer;
    uint32_t region_count;
    // Followed by aligned VkBufferCopy[].
    static_assert(alignof(VkBufferCopy) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyImage {
    VkImage src_image;
    VkImageLayout src_image_layout;
    VkImage dst_image;
    VkImageLayout dst_image_layout;
    uint32_t region_count;
    // Followed by aligned VkImageCopy[].
    static_assert(alignof(VkImageCopy) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyBufferToImage {
    VkBuffer src_buffer;
    VkImage dst_image;
    VkImageLayout dst_image_layout;
    uint32_t region_count;
    // Followed by aligned VkBufferImageCopy[].
    static_assert(alignof(VkBufferImageCopy) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyImageToBuffer {
    VkImage src_image;
    VkImageLayout src_image_layout;
    VkBuffer dst_buffer;
    uint32_t region_count;
    // Followed by aligned VkBufferImageCopy[].
    static_assert(alignof(VkBufferImageCopy) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyQueryPoolResults {
    VkQueryPool query_pool;
    uint32_t first_query;
    uint32_t query_count;
    VkBuffer dst_buffer;
    VkDeviceSize dst_offset;
    VkDeviceSize stride;
    VkQueryResultFlags flags;
  };

  struct ArgsVkDispatch {
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
  };

  struct ArgsVkDraw {
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
  };

  struct ArgsVkDrawIndexed {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    int32_t vertex_offset;
    uint32_t first_instance;
  };

  struct ArgsVkEndQuery {
    VkQueryPool query_pool;
    uint32_t query;
  };

  struct ArgsVkResetQueryPool {
    VkQueryPool query_pool;
    uint32_t first_query;
    uint32_t query_count;
  };

  struct ArgsVkBeginConditionalRenderingEXT {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkConditionalRenderingFlagsEXT flags;
  };

  struct ArgsVkFillBuffer {
    VkBuffer dst_buffer;
    VkDeviceSize dst_offset;
    VkDeviceSize size;
    uint32_t data;
  };

  struct ArgsVkPipelineBarrier {
    VkPipelineStageFlags src_stage_mask;
    VkPipelineStageFlags dst_stage_mask;
    VkDependencyFlags dependency_flags;
    uint32_t memory_barrier_count;
    uint32_t buffer_memory_barrier_count;
    uint32_t image_memory_barrier_count;
    uint32_t sample_locations_info_count;
    // Followed by aligned optional VkMemoryBarrier[],
    // optional VkBufferMemoryBarrier[], optional VkImageMemoryBarrier[], then
    // sample_locations_info_count aligned ArgsSampleLocationsInfo records,
    // each followed by its VkSampleLocationEXT[].
    static_assert(alignof(VkMemoryBarrier) <= alignof(uintmax_t));
    static_assert(alignof(VkBufferMemoryBarrier) <= alignof(uintmax_t));
    static_assert(alignof(VkImageMemoryBarrier) <= alignof(uintmax_t));
  };

  struct ArgsSampleLocationsInfo {
    uint32_t image_memory_barrier_index;
    VkSampleCountFlagBits samples_per_pixel;
    VkExtent2D grid_size;
    uint32_t count;
  };

  struct ArgsVkPushConstants {
    VkPipelineLayout layout;
    VkShaderStageFlags stage_flags;
    uint32_t offset;
    uint32_t size;
    // Followed by `size` bytes of values.
  };

  struct ArgsVkSetBlendConstants {
    float blend_constants[4];
  };

  struct ArgsVkSetDepthBias {
    float depth_bias_constant_factor;
    float depth_bias_clamp;
    float depth_bias_slope_factor;
  };

  struct ArgsVkSetDepthBias2EXT {
    float depth_bias_constant_factor;
    float depth_bias_clamp;
    float depth_bias_slope_factor;
    VkDepthBiasRepresentationEXT depth_bias_representation;
    VkBool32 depth_bias_exact;
  };

  struct ArgsVkSetScissor {
    uint32_t first_scissor;
    uint32_t scissor_count;
    // Followed by aligned VkRect2D[].
    static_assert(alignof(VkRect2D) <= alignof(uintmax_t));
  };

  struct ArgsSetStencilMaskReference {
    VkStencilFaceFlags face_mask;
    uint32_t mask_reference;
  };

  struct ArgsVkWriteTimestamp {
    VkPipelineStageFlagBits pipeline_stage;
    VkQueryPool query_pool;
    uint32_t query;
  };

  struct ArgsVkClearDepthStencilImage {
    VkImage image;
    VkImageLayout image_layout;
    VkClearDepthStencilValue depth_stencil;
    uint32_t range_count;
    // Followed by aligned VkImageSubresourceRange[].
  };

  struct ArgsVkResolveImage {
    VkImage src_image;
    VkImageLayout src_image_layout;
    VkImage dst_image;
    VkImageLayout dst_image_layout;
    uint32_t region_count;
    // Followed by aligned VkImageResolve[].
    static_assert(alignof(VkImageResolve) <= alignof(uintmax_t));
  };

  struct ArgsVkBlitImage {
    VkImage src_image;
    VkImageLayout src_image_layout;
    VkImage dst_image;
    VkImageLayout dst_image_layout;
    uint32_t region_count;
    VkFilter filter;
    // Followed by aligned VkImageBlit[].
    static_assert(alignof(VkImageBlit) <= alignof(uintmax_t));
  };

  struct ArgsVkUpdateBuffer {
    VkBuffer dst_buffer;
    VkDeviceSize dst_offset;
    VkDeviceSize data_size;
    // Followed by data_size bytes.
  };

  struct ArgsVkPushDescriptorSetKHR {
    VkPipelineBindPoint pipeline_bind_point;
    VkPipelineLayout layout;
    uint32_t set;
    uint32_t descriptor_write_count;
    // Followed by descriptor_write_count aligned ArgsPushDescriptorWrite
    // records, each followed by its aligned info array.
  };

  enum class PushDescriptorInfo : uint32_t {
    kImage,
    kBuffer,
    kTexelBufferView,
  };

  struct ArgsPushDescriptorWrite {
    uint32_t dst_binding;
    uint32_t dst_array_element;
    uint32_t descriptor_count;
    VkDescriptorType descriptor_type;
    PushDescriptorInfo info;
    // Followed by aligned VkDescriptorImageInfo[], VkDescriptorBufferInfo[] or
    // VkBufferView[] (descriptor_count of them).
    static_assert(alignof(VkDescriptorImageInfo) <= alignof(uintmax_t));
    static_assert(alignof(VkDescriptorBufferInfo) <= alignof(uintmax_t));
    static_assert(alignof(VkBufferView) <= alignof(uintmax_t));
  };
  static PushDescriptorInfo GetPushDescriptorInfo(VkDescriptorType type);
  static size_t GetPushDescriptorInfoSize(PushDescriptorInfo info);

  struct ArgsHoistedCommands {
    // Range of hoisted_stream_, in elements.
    size_t begin;
    size_t end;
  };

  struct ArgsVkSetViewport {
    uint32_t first_viewport;
    uint32_t viewport_count;
    // Followed by aligned VkViewport[].
    static_assert(alignof(VkViewport) <= alignof(uintmax_t));
  };

  void* WriteCommand(Command command, size_t arguments_size_bytes);
  void ExecuteStream(VkCommandBuffer command_buffer, const uintmax_t* stream,
                     size_t stream_remaining, const uintmax_t* hoisted_stream) const;

  struct BarrierArrays {
    const VkMemoryBarrier* memory = nullptr;
    const VkBufferMemoryBarrier* buffer = nullptr;
    const VkImageMemoryBarrier* image = nullptr;
    // Where the sample locations records start.
    size_t end_offset_bytes = 0;
  };
  static BarrierArrays GetBarrierArrays(const ArgsVkPipelineBarrier& args);
  // Commands that only set state for later draws and dispatches: a barrier
  // may be executed before them instead of after.
  static bool IsStateCommand(Command command);
  // vulkan_tight_render_area: the render area a pass's draws and clears need.
  static VkRect2D PassRenderArea(const uintmax_t* stream, size_t stream_remaining,
                                 const VkRect2D& full, VkRect2D scissor, bool has_scissor);
  // vulkan_merge_adjacent_barriers: with stream at a barrier's arguments,
  // executes it and the barriers after it (only state commands between) as
  // one barrier, then those state commands, and moves stream past the last
  // barrier taken. False, with nothing executed, when nothing follows to merge.
  bool MergeBarriersAt(VkCommandBuffer command_buffer, const uintmax_t*& stream,
                       size_t& stream_remaining, const uintmax_t* hoisted_stream) const;

  const ui::vulkan::VulkanDevice::Functions& functions() const;

  // One of them: the command processor's device is looked up at replay (it
  // may not exist yet when this is constructed).
  const VulkanCommandProcessor* command_processor_ = nullptr;
  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;

  // uintmax_t to ensure uint64_t and pointer alignment of all structures.
  std::vector<uintmax_t> command_stream_;
  std::vector<uintmax_t> hoisted_stream_;
  std::vector<uintmax_t>* write_stream_ = &command_stream_;
  bool count_breadcrumb_work_ = false;
  ui::GpuBreadcrumbWork breadcrumb_work_;
  void CountBreadcrumbWork(Command command);
  // For rebuilding pNext chains of barriers with sample locations. Scratch of
  // the replay, which runs on one thread at a time.
  mutable std::vector<VkImageMemoryBarrier> image_barriers_with_sample_locations_;
  mutable std::vector<VkSampleLocationsInfoEXT> barrier_sample_locations_;
  // vulkan_merge_adjacent_barriers, latched per Execute, and its scratch.
  mutable bool merge_barriers_ = false;
  // vulkan_tight_render_area, and the scissor 0 the replay last set.
  mutable bool tight_render_area_ = false;
  mutable VkRect2D tight_scissor_ = {};
  mutable bool tight_has_scissor_ = false;
  mutable std::vector<const ArgsVkPipelineBarrier*> merged_args_;
  mutable std::vector<const uintmax_t*> merge_state_commands_;
  mutable std::vector<VkImage> merge_images_;
  mutable std::vector<VkMemoryBarrier> merged_memory_barriers_;
  mutable std::vector<VkBufferMemoryBarrier> merged_buffer_barriers_;
  mutable std::vector<VkImageMemoryBarrier> merged_image_barriers_;
  // Push descriptor writes rebuilt at replay.
  mutable std::vector<VkWriteDescriptorSet> push_descriptor_writes_;
};

}  // namespace rex::graphics::vulkan
