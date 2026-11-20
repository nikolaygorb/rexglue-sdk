/**
 * @file        graphics/native/submission.cpp
 * @brief       Native renderer: command buffers and their submissions
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/native/submission.h>

#include <algorithm>
#include <optional>
#include <vector>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/ui/vulkan/instance.h>

REXCVAR_DEFINE_BOOL(native_gpu_time_by_kind, false, "GPU",
                    "Native renderer: the graphics card's time also by kind of work (draws, EDRAM "
                    "transfers, resolves, texture loads, uploads, the swap, other), from a "
                    "timestamp wherever the kind changes: in the frame statistics "
                    "(frame_stats_interval) and, in builds with performance counters, the debug "
                    "overlay and perf_log_csv. Off: only the total, with frame_stats_interval")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::native {

const char* RenderPassEndName(RenderPassEnd reason) {
  static const char* const kNames[] = {
      "other",     "switch",  "submit",       "target layout", "EDRAM",
      "resolve",   "composite", "texture load", "memory barrier", "upload",
      "VIZ",       "memexport", "present",
  };
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == size_t(RenderPassEnd::kCount));
  return uint32_t(reason) < uint32_t(RenderPassEnd::kCount) ? kNames[uint32_t(reason)] : "?";
}

GpuSection GpuSectionOf(GpuKind kind) {
  switch (kind) {
    case GpuKind::kDraws:
      return GpuSection::kDraws;
    case GpuKind::kEdram:
    case GpuKind::kEdramLayout:
      return GpuSection::kTransfers;
    case GpuKind::kResolveDump:
    case GpuKind::kResolveDumpMsaa:
    case GpuKind::kResolveDumpDepth:
    case GpuKind::kResolveMemory:
    case GpuKind::kResolveImage:
    case GpuKind::kResolveTexture:
    case GpuKind::kReadback:
    case GpuKind::kResolveLayout:
      return GpuSection::kResolves;
    case GpuKind::kTextureLoads:
      return GpuSection::kTextureLoads;
    case GpuKind::kUploads:
      return GpuSection::kUploads;
    case GpuKind::kSwap:
      return GpuSection::kSwap;
    default:
      return GpuSection::kOther;
  }
}

const char* GpuKindName(GpuKind kind) {
  static const char* const kNames[] = {
      "draws",           "EDRAM",           "EDRAM layouts",      "resolve dump",
      "resolve dump MSAA", "resolve dump depth", "resolve memory", "resolve image",
      "resolve texture", "readback",        "resolve layouts",    "texture loads",
      "uploads",         "swap",            "other",
  };
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == size_t(GpuKind::kCount));
  return uint32_t(kind) < uint32_t(GpuKind::kCount) ? kNames[uint32_t(kind)] : "?";
}

GpuKind GpuKindOf(RenderPassEnd reason) {
  switch (reason) {
    case RenderPassEnd::kEdram:
      return GpuKind::kEdram;
    case RenderPassEnd::kResolve:
      return GpuKind::kResolveImage;
    case RenderPassEnd::kTextureLoad:
      return GpuKind::kTextureLoads;
    case RenderPassEnd::kUpload:
      return GpuKind::kUploads;
    case RenderPassEnd::kPresent:
      return GpuKind::kSwap;
    default:
      return GpuKind::kOther;
  }
}

GpuKind GpuLayoutKindOf(GpuKind kind) {
  if (kind == GpuKind::kCount) {
    return GpuKind::kCount;
  }
  switch (GpuSectionOf(kind)) {
    case GpuSection::kTransfers:
      return GpuKind::kEdramLayout;
    case GpuSection::kResolves:
      return GpuKind::kResolveLayout;
    default:
      return GpuKind::kCount;
  }
}

Submitter::~Submitter() {
  Shutdown();
}

bool Submitter::Initialize(ui::vulkan::VulkanDevice* vulkan_device) {
  vulkan_device_ = vulkan_device;
  timeline_ = std::make_unique<ui::vulkan::VulkanGPUCompletionTimeline>(vulkan_device);
  recorder_ = std::make_unique<CommandRecorder>(vulkan_device);
  InitializeGpuTime();
  if (threaded_submission_requested_) {
    submission_shutdown_ = false;
    submission_failed_ = false;
    submission_submitted_ = current() - 1;
    callbacks_queued_ = 0;
    callbacks_done_ = 0;
    timeline_->SetSubmittedLimit(&submission_submitted_);
    submission_thread_ = rex::thread::Thread::Create({}, [this]() { SubmissionThread(); });
    if (submission_thread_) {
      submission_thread_->set_name("Native Submission");
    } else {
      REXGPU_WARN("Native renderer: failed to create the submission thread, submitting on the "
                  "GPU command processor thread");
      timeline_->SetSubmittedLimit(nullptr);
    }
  }
  return true;
}

void Submitter::InitializeGpuTime() {
  // As VulkanCommandProcessor::InitializeGpuTime.
  const ui::vulkan::VulkanInstance::Functions& ifn =
      vulkan_device_->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device_->physical_device();
  VkPhysicalDeviceProperties properties;
  ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);
  uint32_t queue_family_count = 0;
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count,
                                               queue_families.data());
  const uint32_t queue_family = vulkan_device_->queue_family_graphics_compute();
  const uint32_t valid_bits =
      queue_family < queue_family_count ? queue_families[queue_family].timestampValidBits : 0;
  if (!valid_bits || properties.limits.timestampPeriod <= 0.0f) {
    REXGPU_INFO("Native renderer: this device can't time command buffers; no GPU time in "
                "frame_stats_interval");
    return;
  }
  gpu_time_ns_per_tick_ = double(properties.limits.timestampPeriod);
  gpu_time_valid_mask_ = valid_bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << valid_bits) - 1;
  VkQueryPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  gpu_time_by_kind_ = REXCVAR_GET(native_gpu_time_by_kind);
  gpu_time_slot_queries_ = gpu_time_by_kind_ ? 2 + kGpuKindMarks : 2;
  gpu_time_slot_count_ = gpu_time_by_kind_ ? kGpuTimeSlotsByKind : kGpuTimeSlots;
  pool_info.queryCount = gpu_time_slot_count_ * gpu_time_slot_queries_;
  if (vulkan_device_->functions().vkCreateQueryPool(vulkan_device_->device(), &pool_info, nullptr,
                                                    &gpu_time_pool_) != VK_SUCCESS) {
    gpu_time_pool_ = VK_NULL_HANDLE;
    gpu_time_by_kind_ = false;
    REXGPU_WARN("Native renderer: failed to create a timestamp query pool; no GPU time in "
                "frame_stats_interval");
  }
}

void Submitter::ReadGpuTimes(uint64_t completed_submission) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  while (!gpu_time_submitted_.empty() &&
         gpu_time_submitted_.front().submission <= completed_submission) {
    const GpuTimeSubmission& done = gpu_time_submitted_.front();
    // Start, end, then the marks.
    const uint32_t query_count = 2 + uint32_t(done.marks.size());
    gpu_time_stamps_.resize(query_count);
    if (dfn.vkGetQueryPoolResults(vulkan_device_->device(), gpu_time_pool_,
                                  done.slot * gpu_time_slot_queries_, query_count,
                                  sizeof(uint64_t) * query_count, gpu_time_stamps_.data(),
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      const uint64_t start = gpu_time_stamps_[0] & gpu_time_valid_mask_;
      const uint64_t total =
          ((gpu_time_stamps_[1] & gpu_time_valid_mask_) - start) & gpu_time_valid_mask_;
      gpu_time_ticks_ += total;
      if (gpu_time_by_kind_) {
        // The marks in the order the graphics card wrote them (hoisted
        // commands ran before commands recorded earlier), as offsets from the
        // start; each begins its kind, up to the next one or the end.
        gpu_time_sorted_marks_.clear();
        for (size_t i = 0; i < done.marks.size(); ++i) {
          const uint64_t offset =
              ((gpu_time_stamps_[2 + i] & gpu_time_valid_mask_) - start) & gpu_time_valid_mask_;
          gpu_time_sorted_marks_.emplace_back(std::min(offset, total), done.marks[i]);
        }
        std::stable_sort(gpu_time_sorted_marks_.begin(), gpu_time_sorted_marks_.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        GpuKind kind = done.start_kind;
        uint64_t previous = 0;
        auto add = [&](uint64_t until) {
          const uint64_t ticks = until - previous;
          gpu_kind_ticks_[size_t(kind)] += ticks;
          gpu_kind_perf_ticks_[size_t(kind)] += ticks;
          previous = until;
        };
        for (const auto& [offset, begun] : gpu_time_sorted_marks_) {
          add(offset);
          kind = begun;
        }
        add(total);
      }
    }
    gpu_time_submitted_.pop_front();
    --gpu_time_slots_used_;
  }
}

GpuKind Submitter::gpu_kind() const {
  return recorder_ && recorder_->is_writing_hoisted() ? gpu_kind_hoisted_ : gpu_kind_main_;
}

void Submitter::MarkGpuKind(GpuKind kind) {
  if (!gpu_time_by_kind_) {
    return;
  }
  const bool hoisted = recorder_->is_writing_hoisted();
  GpuKind& last = hoisted ? gpu_kind_hoisted_ : gpu_kind_main_;
  if (last == kind) {
    return;
  }
  if (open_gpu_time_slot_ == kNoGpuTimeSlot) {
    // Not timed (no slot free): still the kind a timed one starts with.
    last = kind;
    return;
  }
  if (open_gpu_marks_.size() >= kGpuKindMarks) {
    // The rest of the command buffer counts as the last kind marked.
    ++gpu_kind_marks_dropped_;
    return;
  }
  last = kind;
  recorder_->CmdVkWriteTimestamp(
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_time_pool_,
      open_gpu_time_slot_ * gpu_time_slot_queries_ + 2 + uint32_t(open_gpu_marks_.size()));
  open_gpu_marks_.push_back(kind);
  if (hoisted) {
    gpu_kind_hoisted_marked_ = true;
  }
}

void Submitter::TakeGpuKindNs(uint64_t (&ns_out)[size_t(GpuKind::kCount)],
                              uint64_t& marks_dropped_out) {
  for (size_t i = 0; i < size_t(GpuKind::kCount); ++i) {
    ns_out[i] = uint64_t(double(gpu_kind_ticks_[i]) * gpu_time_ns_per_tick_);
    gpu_kind_ticks_[i] = 0;
  }
  marks_dropped_out = gpu_kind_marks_dropped_;
  gpu_kind_marks_dropped_ = 0;
}

void Submitter::FlushGpuTimePerfCounters() {
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  if (!gpu_time_by_kind_) {
    return;
  }
  ReadGpuTimes(completed());
  static constexpr rex::perf::CounterId kCounters[size_t(GpuSection::kCount)] = {
      rex::perf::CounterId::kGpuDrawUs,    rex::perf::CounterId::kGpuTransferUs,
      rex::perf::CounterId::kGpuResolveUs, rex::perf::CounterId::kGpuTextureLoadUs,
      rex::perf::CounterId::kGpuUploadUs,  rex::perf::CounterId::kGpuSwapUs,
      rex::perf::CounterId::kGpuOtherUs};
  // The kinds folded into the SDK's sections.
  uint64_t section_ticks[size_t(GpuSection::kCount)] = {};
  for (size_t i = 0; i < size_t(GpuKind::kCount); ++i) {
    section_ticks[size_t(GpuSectionOf(GpuKind(i)))] += gpu_kind_perf_ticks_[i];
    gpu_kind_perf_ticks_[i] = 0;
  }
  uint64_t total_ticks = 0;
  for (size_t i = 0; i < size_t(GpuSection::kCount); ++i) {
    if (section_ticks[i]) {
      total_ticks += section_ticks[i];
      rex::perf::IncrementCounter(
          kCounters[i], int64_t(double(section_ticks[i]) * gpu_time_ns_per_tick_ / 1000.0));
    }
  }
  if (total_ticks) {
    rex::perf::IncrementCounter(rex::perf::CounterId::kGpuTimeUs,
                                int64_t(double(total_ticks) * gpu_time_ns_per_tick_ / 1000.0));
  }
#endif
}

void Submitter::ReleaseOpenGpuTimeSlot() {
  if (open_gpu_time_slot_ == kNoGpuTimeSlot) {
    return;
  }
  // The latest slot taken: the ring steps back over it.
  gpu_time_next_slot_ = open_gpu_time_slot_;
  --gpu_time_slots_used_;
  open_gpu_time_slot_ = kNoGpuTimeSlot;
  open_gpu_marks_.clear();
}

uint64_t Submitter::TakeGpuTimeNs() {
  if (gpu_time_pool_ == VK_NULL_HANDLE) {
    return 0;
  }
  ReadGpuTimes(completed());
  uint64_t ns = uint64_t(double(gpu_time_ticks_) * gpu_time_ns_per_tick_);
  gpu_time_ticks_ = 0;
  return ns;
}

void Submitter::Shutdown() {
  if (!vulkan_device_) {
    return;
  }
  // Its owner may be gone already.
  before_end_render_pass_ = nullptr;
  if (is_open()) {
    // Recorded but never submitted: nothing on the GPU refers to it.
    EndRenderPass();
    recorder_->Reset();
    after_replay_.clear();
    open_ = false;
    ReleaseOpenGpuTimeSlot();
  }
  before_submit_.clear();
  // Everything handed to the thread reaches the queue (or has its fence
  // signalled) before it ends.
  ShutdownSubmissionThread();
  if (timeline_) {
    timeline_->AwaitAllSubmissions();
  }
  gpu_time_submitted_.clear();
  gpu_time_slots_used_ = 0;
  if (gpu_time_pool_ != VK_NULL_HANDLE) {
    vulkan_device_->functions().vkDestroyQueryPool(vulkan_device_->device(), gpu_time_pool_,
                                                   nullptr);
    gpu_time_pool_ = VK_NULL_HANDLE;
  }
  for (auto& destruction : destructions_) {
    destruction.second();
  }
  destructions_.clear();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  for (auto& submitted : submitted_buffers_) {
    free_buffers_.push_back(submitted.second);
  }
  submitted_buffers_.clear();
  for (CommandBuffer& buffer : free_buffers_) {
    // Destroying the pool frees its command buffer.
    dfn.vkDestroyCommandPool(device, buffer.pool, nullptr);
  }
  free_buffers_.clear();
  recorder_.reset();
  timeline_.reset();
  vulkan_device_ = nullptr;
}

void Submitter::ShutdownSubmissionThread() {
  if (!submission_thread_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(submission_mutex_);
    submission_shutdown_ = true;
  }
  submission_request_cond_.notify_all();
  // The thread empties the queue before it returns.
  rex::thread::Wait(submission_thread_.get(), false);
  submission_thread_.reset();
  submission_jobs_.clear();
  submission_free_jobs_.clear();
  if (timeline_) {
    timeline_->SetSubmittedLimit(nullptr);
  }
}

uint64_t Submitter::current() const {
  return timeline_->GetUpcomingSubmission();
}

uint64_t Submitter::completed() {
  return timeline_->UpdateAndGetCompletedSubmission();
}

void Submitter::RecycleCompletedCommandBuffers() {
  uint64_t completed_submission = completed();
  if (gpu_time_pool_ != VK_NULL_HANDLE) {
    // Frees the ring's slots as the command buffers complete.
    ReadGpuTimes(completed_submission);
  }
  while (!submitted_buffers_.empty() && submitted_buffers_.front().first <= completed_submission) {
    free_buffers_.push_back(submitted_buffers_.front().second);
    submitted_buffers_.pop_front();
  }
}

bool Submitter::AcquireCommandBuffer(CommandBuffer& out) {
  RecycleCompletedCommandBuffers();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (!free_buffers_.empty()) {
    // Completed on the GPU, so the submission thread is done with it too.
    out = free_buffers_.back();
    free_buffers_.pop_back();
    if (dfn.vkResetCommandPool(device, out.pool, 0) != VK_SUCCESS) {
      dfn.vkDestroyCommandPool(device, out.pool, nullptr);
      return false;
    }
    return true;
  }
  // One pool per command buffer: a pool reset is the cheapest reset.
  VkCommandPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &out.pool) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to create a command pool");
    return false;
  }
  VkCommandBufferAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate_info.commandPool = out.pool;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device, &allocate_info, &out.buffer) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to allocate a command buffer");
    dfn.vkDestroyCommandPool(device, out.pool, nullptr);
    return false;
  }
  return true;
}

CommandRecorder* Submitter::Open() {
  if (open_) {
    return recorder_.get();
  }
  open_ = true;
  ++buffer_serial_;
  if (gpu_time_enabled_ && gpu_time_slots_used_ < gpu_time_slot_count_) {
    // Not in a render pass yet: the reset can be recorded here.
    open_gpu_time_slot_ = gpu_time_next_slot_;
    gpu_time_next_slot_ = (gpu_time_next_slot_ + 1) % gpu_time_slot_count_;
    ++gpu_time_slots_used_;
    recorder_->CmdVkResetQueryPool(gpu_time_pool_, open_gpu_time_slot_ * gpu_time_slot_queries_,
                                   gpu_time_slot_queries_);
    recorder_->CmdVkWriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_time_pool_,
                                   open_gpu_time_slot_ * gpu_time_slot_queries_);
    // The kind the last command buffer ended with goes on.
    open_gpu_start_kind_ = gpu_kind_main_;
    open_gpu_marks_.clear();
  }
  return recorder_.get();
}

CommandRecorder* Submitter::OpenOutsideRenderPass(RenderPassEnd reason) {
  return OpenOutsideRenderPass(reason, GpuKindOf(reason));
}

CommandRecorder* Submitter::OpenOutsideRenderPass(RenderPassEnd reason, GpuKind kind) {
  CommandRecorder* command_buffer = Open();
  if (command_buffer) {
    EndRenderPass(reason);
    MarkGpuKind(kind);
  }
  return command_buffer;
}

CommandRecorder* Submitter::EnterRenderPass(VkRenderPass render_pass, VkFramebuffer framebuffer,
                                            VkExtent2D extent) {
  CommandRecorder* command_buffer = Open();
  if (!command_buffer) {
    return nullptr;
  }
  if (render_pass_ == render_pass && render_pass_framebuffer_ == framebuffer &&
      render_pass_extent_.width == extent.width && render_pass_extent_.height == extent.height) {
    return command_buffer;
  }
  EndRenderPass(RenderPassEnd::kSwitch);
  ++counters_.render_passes;
  ++render_pass_serial_;
  render_pass_ = render_pass;
  render_pass_framebuffer_ = framebuffer;
  render_pass_extent_ = extent;
  // Draws from here (their render pass begin with its loads and clears),
  // marked before the hoist point: the hoisted commands run after the mark,
  // then the draws again (EndRenderPass marks that).
  MarkGpuKind(GpuKind::kDraws);
  gpu_kind_hoisted_ = GpuKind::kDraws;
  gpu_kind_hoisted_marked_ = false;
  if (hoist_before_render_passes_) {
    // What BeginBeforeRenderPass records goes here, before the render pass.
    hoist_point_ = command_buffer->CmdHoistPoint();
  }
  VkRenderPassBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin_info.renderPass = render_pass;
  begin_info.framebuffer = framebuffer;
  begin_info.renderArea.extent = extent;
  command_buffer->CmdVkBeginRenderPass(&begin_info, VK_SUBPASS_CONTENTS_INLINE);
  // The draws bind their state anew in each render pass (passes of their own
  // between them bind theirs).
  ++buffer_serial_;
  return command_buffer;
}

void Submitter::EndRenderPass(RenderPassEnd reason) {
  if (render_pass_ == VK_NULL_HANDLE) {
    return;
  }
  assert_false(recorder_->is_writing_hoisted());
  // Hoisted commands of another kind are followed by the render pass.
  if (gpu_kind_hoisted_marked_ && hoist_point_ != kNoHoistPoint &&
      gpu_kind_hoisted_ != GpuKind::kDraws) {
    recorder_->SetWritingHoisted(true);
    MarkGpuKind(GpuKind::kDraws);
    recorder_->ExtendHoistPoint(hoist_point_);
    recorder_->SetWritingHoisted(false);
  }
  gpu_kind_hoisted_marked_ = false;
  ++counters_.render_pass_ends[std::min(uint32_t(reason), uint32_t(RenderPassEnd::kCount) - 1)];
  if (before_end_render_pass_) {
    before_end_render_pass_();
  }
  recorder_->CmdVkEndRenderPass();
  hoist_point_ = kNoHoistPoint;
  ++buffer_serial_;
  render_pass_ = VK_NULL_HANDLE;
  render_pass_framebuffer_ = VK_NULL_HANDLE;
  render_pass_extent_ = {};
}

bool Submitter::can_record_before_render_pass() const {
  return open_ && render_pass_ != VK_NULL_HANDLE && hoist_point_ != kNoHoistPoint &&
         !recorder_->is_writing_hoisted();
}

CommandRecorder* Submitter::BeginBeforeRenderPass(GpuKind kind) {
  if (!can_record_before_render_pass()) {
    return nullptr;
  }
  recorder_->SetWritingHoisted(true);
  MarkGpuKind(kind);
  return recorder_.get();
}

void Submitter::EndBeforeRenderPass() {
  if (!recorder_ || !recorder_->is_writing_hoisted()) {
    return;
  }
  recorder_->ExtendHoistPoint(hoist_point_);
  recorder_->SetWritingHoisted(false);
}

void Submitter::AddBeforeSubmit(std::function<void()> fn) {
  before_submit_.push_back(std::move(fn));
}

void Submitter::SetBeforeEndRenderPass(std::function<void()> fn) {
  before_end_render_pass_ = std::move(fn);
}

void Submitter::RecordAfterReplay(std::function<void(VkCommandBuffer)> fn) {
  after_replay_.push_back(std::move(fn));
}

bool Submitter::RecordCommandBuffer(
    VkCommandBuffer command_buffer, const CommandRecorder::Streams* streams,
    std::vector<std::function<void(VkCommandBuffer)>>& after_replay, uint32_t gpu_time_slot) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn.vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to begin a command buffer");
    return false;
  }
  if (streams) {
    recorder_->ExecuteStreams(command_buffer, *streams);
  } else {
    recorder_->Execute(command_buffer);
  }
  for (std::function<void(VkCommandBuffer)>& fn : after_replay) {
    fn(command_buffer);
  }
  if (gpu_time_slot != kNoGpuTimeSlot) {
    dfn.vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_time_pool_,
                            gpu_time_slot * gpu_time_slot_queries_ + 1);
  }
  if (dfn.vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
    REXGPU_ERROR("Native renderer: failed to end a command buffer");
    return false;
  }
  return true;
}

bool Submitter::Submit() {
  if (!open_) {
    return true;
  }
  EndRenderPass(RenderPassEnd::kSubmit);
  for (const std::function<void()>& fn : before_submit_) {
    fn();
  }
  open_ = false;
  ++buffer_serial_;
  const uint32_t gpu_time_slot = open_gpu_time_slot_;
  CommandBuffer buffer;
  if (!AcquireCommandBuffer(buffer)) {
    recorder_->Reset();
    after_replay_.clear();
    ReleaseOpenGpuTimeSlot();
    return false;
  }
  const uint64_t submission = current();
  if (submission_thread_) {
    VkResult fence_result = VK_SUCCESS;
    auto fence_acquisition = timeline_->AcquireFenceForSubmission(&fence_result);
    if (!fence_acquisition.has_value()) {
      REXGPU_ERROR("Native renderer: failed to acquire a submission fence ({})",
                   int32_t(fence_result));
      free_buffers_.push_back(buffer);
      recorder_->Reset();
      after_replay_.clear();
      ReleaseOpenGpuTimeSlot();
      return false;
    }
    Job job;
    {
      std::lock_guard<std::mutex> lock(submission_mutex_);
      if (!submission_free_jobs_.empty()) {
        job = std::move(submission_free_jobs_.back());
        submission_free_jobs_.pop_back();
      }
    }
    job.submission = submission;
    job.command_buffer = buffer;
    // Marks the submission as made: releasing the acquisition on return
    // advances the timeline, and the thread signals the fence.
    job.fence = fence_acquisition->GetFenceForSubmitting();
    job.gpu_time_slot = gpu_time_slot;
    job.after_replay.swap(after_replay_);
    job.callback = nullptr;
    recorder_->TakeStreams(job.streams);
    {
      std::lock_guard<std::mutex> lock(submission_mutex_);
      submission_jobs_.push_back(std::move(job));
    }
    submission_request_cond_.notify_one();
  } else {
    bool recorded =
        RecordCommandBuffer(buffer.buffer, nullptr, after_replay_, gpu_time_slot);
    recorder_->Reset();
    after_replay_.clear();
    if (!recorded) {
      free_buffers_.push_back(buffer);
      ReleaseOpenGpuTimeSlot();
      return false;
    }
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &buffer.buffer;
    VkResult result = timeline_->AcquireFenceAndSubmit(
        vulkan_device_->queue_family_graphics_compute(), 0, 1, &submit_info);
    if (result != VK_SUCCESS) {
      REXGPU_ERROR("Native renderer: vkQueueSubmit failed ({})", int32_t(result));
      // Not submitted: the timeline did not advance, the buffer is unused.
      free_buffers_.push_back(buffer);
      ReleaseOpenGpuTimeSlot();
      return false;
    }
  }
  submitted_buffers_.emplace_back(submission, buffer);
  ++counters_.submissions;
  if (gpu_time_slot != kNoGpuTimeSlot) {
    GpuTimeSubmission& timed = gpu_time_submitted_.emplace_back();
    timed.submission = submission;
    timed.slot = gpu_time_slot;
    timed.start_kind = open_gpu_start_kind_;
    timed.marks.swap(open_gpu_marks_);
    open_gpu_marks_.clear();
    open_gpu_time_slot_ = kNoGpuTimeSlot;
  }
  DestroyCompleted();
  return true;
}

void Submitter::SubmissionThread() {
  const uint32_t queue_family = vulkan_device_->queue_family_graphics_compute();
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(submission_mutex_);
      submission_request_cond_.wait(
          lock, [this] { return submission_shutdown_ || !submission_jobs_.empty(); });
      if (submission_jobs_.empty()) {
        return;
      }
      job = std::move(submission_jobs_.front());
      submission_jobs_.pop_front();
    }
    if (job.callback) {
      // After a failure the step still counts, so nobody waits for it.
      if (!submission_failed_.load(std::memory_order_relaxed)) {
        job.callback();
      }
      job.callback = nullptr;
      {
        std::lock_guard<std::mutex> lock(submission_mutex_);
        callbacks_done_.fetch_add(1, std::memory_order_release);
        submission_free_jobs_.push_back(std::move(job));
      }
      submission_done_cond_.notify_all();
      continue;
    }
    if (!submission_failed_.load(std::memory_order_relaxed)) {
      bool submitted = false;
      if (RecordCommandBuffer(job.command_buffer.buffer, &job.streams, job.after_replay,
                              job.gpu_time_slot)) {
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &job.command_buffer.buffer;
        ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
            vulkan_device_->AcquireQueue(queue_family, 0);
        VkResult result = vulkan_device_->SubmitAndUpdateLost(queue_acquisition.queue(), 1,
                                                              &submit_info, job.fence);
        submitted = result == VK_SUCCESS;
        if (!submitted) {
          REXGPU_ERROR("Native renderer: vkQueueSubmit failed ({}) on the submission thread",
                       int32_t(result));
        }
      }
      if (!submitted) {
        submission_failed_ = true;
      }
    }
    if (submission_failed_.load(std::memory_order_relaxed)) {
      // The fence must still be signalled: the timeline waits for it.
      ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
          vulkan_device_->AcquireQueue(queue_family, 0);
      vulkan_device_->SubmitAndUpdateLost(queue_acquisition.queue(), 0, nullptr, job.fence);
    }
    job.streams.main.clear();
    job.streams.hoisted.clear();
    job.after_replay.clear();
    {
      std::lock_guard<std::mutex> lock(submission_mutex_);
      submission_submitted_.store(job.submission, std::memory_order_release);
      submission_free_jobs_.push_back(std::move(job));
    }
    submission_done_cond_.notify_all();
  }
}

void Submitter::AwaitSubmitted(uint64_t submission) {
  if (!submission_thread_ ||
      submission_submitted_.load(std::memory_order_acquire) >= submission) {
    return;
  }
  const uint64_t wait_start = rex::chrono::Clock::QueryHostTickCount();
  {
    std::unique_lock<std::mutex> lock(submission_mutex_);
    submission_done_cond_.wait(lock, [this, submission] {
      return submission_submitted_.load(std::memory_order_acquire) >= submission;
    });
  }
  ++counters_.thread_waits;
  counters_.thread_wait_ticks += rex::chrono::Clock::QueryHostTickCount() - wait_start;
}

void Submitter::QueueAfterSubmissions(std::function<void()> fn) {
  if (!submission_thread_) {
    fn();
    return;
  }
  Job job;
  job.callback = std::move(fn);
  ++callbacks_queued_;
  {
    std::lock_guard<std::mutex> lock(submission_mutex_);
    submission_jobs_.push_back(std::move(job));
  }
  submission_request_cond_.notify_one();
}

void Submitter::AwaitQueued() {
  if (!submission_thread_ ||
      callbacks_done_.load(std::memory_order_acquire) == callbacks_queued_) {
    return;
  }
  std::unique_lock<std::mutex> lock(submission_mutex_);
  submission_done_cond_.wait(lock, [this] {
    return callbacks_done_.load(std::memory_order_acquire) == callbacks_queued_;
  });
}

bool Submitter::Await(uint64_t submission) {
  if (submission >= current()) {
    if (!is_open() || !Submit()) {
      // Nothing recorded under that number (or the submit failed).
      submission = current() - 1;
    }
  }
  if (!submission) {
    return true;
  }
  // Only submissions on the queue can be awaited.
  AwaitSubmitted(submission);
  // Counted only where it blocks (frame_stats_interval).
  const bool blocks = submission > completed();
  const uint64_t wait_start = blocks ? rex::chrono::Clock::QueryHostTickCount() : 0;
  bool result = timeline_->AwaitSubmissionAndUpdateCompleted(submission);
  if (blocks) {
    ++counters_.gpu_waits;
    counters_.gpu_wait_ticks += rex::chrono::Clock::QueryHostTickCount() - wait_start;
  }
  DestroyCompleted();
  return result;
}

bool Submitter::AwaitAll() {
  return Await(current());
}

void Submitter::DestroyAfter(std::function<void()> fn) {
  destructions_.emplace_back(current(), std::move(fn));
}

void Submitter::DestroyCompleted() {
  uint64_t completed_submission = completed();
  while (!destructions_.empty() && destructions_.front().first <= completed_submission) {
    // Pop first: a destructor may queue another destruction.
    std::function<void()> fn = std::move(destructions_.front().second);
    destructions_.pop_front();
    fn();
  }
}

}  // namespace rex::graphics::native
