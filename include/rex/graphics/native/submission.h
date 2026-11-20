/**
 * @file        graphics/native/submission.h
 * @brief       Native renderer: command buffers and their submissions
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/vulkan/gpu_time.h>
#include <rex/thread.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/gpu_completion_timeline.h>

namespace rex::graphics::native {

// Why the draws' render pass was ended, for frame_stats_interval.
enum class RenderPassEnd : uint32_t {
  kOther,
  // Another draw render pass, framebuffer or extent.
  kSwitch,
  kSubmit,
  // A render target or a resolved image changing its layout for a draw.
  kTargetLayout,
  // EDRAM transfers and clears between surfaces.
  kEdram,
  // Resolves: copies, the resolve pass, into memory, read back.
  kResolve,
  kComposite,
  kTextureLoad,
  // A barrier on the shared memory between uses (upload, draw, export).
  kMemoryBarrier,
  // Shared memory and index uploads.
  kUpload,
  // VIZ predicate copies.
  kViz,
  kMemexport,
  kPresent,
  kCount,
};
// Short names for the log, by RenderPassEnd.
const char* RenderPassEndName(RenderPassEnd reason);

// Kinds of graphics card work, as the SDK's Vulkan backend times them
// (vulkan_gpu_time; native_gpu_time_by_kind here).
using GpuSection = vulkan::GpuSection;
// native_gpu_time_by_kind's kinds: GpuSection's, with the resolves split by
// step.
enum class GpuKind : uint32_t {
  kDraws,
  kEdram,
  // Layout changes of render targets (RenderTargets::Transition) in EDRAM
  // transfers and in resolves, timed apart from the work around them (a
  // driver may decompress the whole target there), in their section.
  kEdramLayout,
  // ResolveMemory: the render target into its dump buffer (single-sampled
  // colour, multisampled colour, depth), then that into the guest memory (the
  // SDK's resolve copy shaders).
  kResolveDump,
  kResolveDumpMsaa,
  kResolveDumpDepth,
  kResolveMemory,
  // The resolve's own image (the resolve pass or a copy), and its copy into
  // the texture cache's texture (native_resolve_into_textures).
  kResolveImage,
  kResolveTexture,
  // readback_resolve's copies for the CPU.
  kReadback,
  kResolveLayout,
  kTextureLoads,
  kUploads,
  kSwap,
  kOther,
  kCount,
};
GpuSection GpuSectionOf(GpuKind kind);
// Short names for the log.
const char* GpuKindName(GpuKind kind);
// The kind of the work a module records after OpenOutsideRenderPass(reason)
// without one of its own.
GpuKind GpuKindOf(RenderPassEnd reason);
// The layout-change kind of `kind`'s section (kEdramLayout, kResolveLayout),
// or kCount: not timed apart.
GpuKind GpuLayoutKindOf(GpuKind kind);

// One command buffer's worth of commands open at a time, for the graphics and
// compute queue 0 (the queue the presenter's guest output refresher must use),
// submitted with a fence of a VulkanGPUCompletionTimeline. Submissions are
// numbered from 1; current() is the number the open (or next) one gets.
// Objects still in use by the GPU are destroyed later with DestroyAfter.
// Called from the command processor thread only.
//
// Commands are recorded into a CommandRecorder (the SDK's
// DeferredCommandBuffer, a CPU-side stream). Submit replays it into a Vulkan
// command buffer and submits that: on the command processor thread, or with
// SetThreadedSubmission on a thread of its own, as the SDK's Vulkan backend
// (vulkan_threaded_submission), so the driver's work leaves the command
// processor thread. Await and completed() only see submissions the thread has
// given to the queue.
//
// The draws' render pass stays open across draws (EnterRenderPass);
// everything else (copies, uploads, barriers, passes of its own) records with
// OpenOutsideRenderPass, which ends it first.
//
// With SetHoistBeforeRenderPasses, each draws' render pass begins at a hoist
// point of the recorder: commands recorded between BeginBeforeRenderPass and
// EndBeforeRenderPass run there, before the render pass, which stays open:
// uploads of memory its draws haven't read yet and loads of textures they
// haven't sampled (native_hoist_uploads, native_hoist_texture_loads).
//
// For frame_stats_interval it can time its command buffers on the graphics
// card (SetGpuTimeEnabled): a timestamp at the start and one at the end of
// each, both at the bottom of the pipe as in the SDK's Vulkan backend, so the
// time of one submission isn't counted again in the next.
//
// With native_gpu_time_by_kind (read at Initialize) also by kind of work
// (GpuKind): a timestamp wherever the kind changes, where commands of the
// new kind begin: EnterRenderPass (draws, before its hoist point),
// OpenOutsideRenderPass (its kind, or by its reason, GpuKindOf) and
// BeginBeforeRenderPass (by its argument, in the hoisted commands), and one at
// the end of a render pass's hoisted commands (the render pass follows them).
// The commands hoisted before a render pass run before commands recorded
// earlier, so the stamps are put in their order on the graphics card when
// read.
class Submitter {
 public:
  Submitter() = default;
  Submitter(const Submitter&) = delete;
  Submitter& operator=(const Submitter&) = delete;
  ~Submitter();

  bool Initialize(ui::vulkan::VulkanDevice* vulkan_device);
  // Waits for the GPU, runs the pending destructions.
  void Shutdown();

  ui::vulkan::VulkanDevice* vulkan_device() const { return vulkan_device_; }

  // Opens a command buffer if none is open; null on failure. May be inside
  // the draws' render pass: for draws only.
  CommandRecorder* Open();
  // Open, with the draws' render pass ended (for `reason`, counted).
  CommandRecorder* OpenOutsideRenderPass(RenderPassEnd reason = RenderPassEnd::kOther);
  // The same, its commands timed as `kind` (native_gpu_time_by_kind).
  CommandRecorder* OpenOutsideRenderPass(RenderPassEnd reason, GpuKind kind);
  bool is_open() const { return open_; }
  // Where commands go now (in the draws' render pass or outside it); null when
  // no command buffer is open.
  CommandRecorder* open_command_buffer() { return open_ ? recorder_.get() : nullptr; }
  // Changes each time a command buffer or a draws' render pass is begun or
  // left: the bound pipeline, sets and dynamic state recorded before are not
  // relied upon (passes of their own bind theirs in between).
  uint64_t buffer_serial() const { return buffer_serial_; }

  // Before Initialize.
  void SetHoistBeforeRenderPasses(bool enabled) { hoist_before_render_passes_ = enabled; }
  // Before Initialize: replay and submit on a thread of its own.
  void SetThreadedSubmission(bool enabled) { threaded_submission_requested_ = enabled; }
  bool threaded_submission() const { return submission_thread_ != nullptr; }
  // In a draws' render pass with hoisting: commands recorded through the
  // returned recorder until EndBeforeRenderPass run before that render pass
  // (outside it); null otherwise (nothing to end then).
  bool can_record_before_render_pass() const;
  // kind: the kind of the work hoisted (native_gpu_time_by_kind).
  CommandRecorder* BeginBeforeRenderPass(GpuKind kind = GpuKind::kOther);
  void EndBeforeRenderPass();
  // Changes with every draws' render pass begun.
  uint64_t render_pass_serial() const { return render_pass_serial_; }
  // frame_stats_interval: an upload recorded before the open render pass.
  void CountUploadBeforeRenderPass() { ++counters_.uploads_before_render_pass; }
  // frame_stats_interval: a texture load recorded before the open render pass.
  void CountTextureLoadBeforeRenderPass() { ++counters_.texture_loads_before_render_pass; }

  // Begins the draws' render pass (attachments loaded and stored, moved to
  // their attachment layouts by the caller) unless the same pass, framebuffer
  // and extent are already open; null on failure.
  CommandRecorder* EnterRenderPass(VkRenderPass render_pass, VkFramebuffer framebuffer,
                                   VkExtent2D extent);
  void EndRenderPass(RenderPassEnd reason = RenderPassEnd::kOther);
  bool in_render_pass() const { return render_pass_ != VK_NULL_HANDLE; }
  // The open draws' render pass and its framebuffer (VK_NULL_HANDLE outside).
  VkRenderPass render_pass() const { return render_pass_; }
  VkFramebuffer render_pass_framebuffer() const { return render_pass_framebuffer_; }

  // Called before every submission, with the render pass ended (upload
  // buffer flushes). Hooks stay until Shutdown.
  void AddBeforeSubmit(std::function<void()> fn);
  // Called in the draws' render pass just before it ends, whatever ends it
  // (occlusion queries can't outlive a render pass instance).
  void SetBeforeEndRenderPass(std::function<void()> fn);
  // For what must write into the Vulkan command buffer itself (the SDK's query
  // pool resolves): run when the open command buffer's commands are replayed,
  // after them, on the submission thread with threaded submission.
  void RecordAfterReplay(std::function<void(VkCommandBuffer)> fn);

  // Ends and submits the open command buffer (nothing when none is open).
  // False if the submission failed (the device may be lost).
  bool Submit();

  uint64_t current() const;
  uint64_t completed();
  // Waits until the submission has completed (submits the open one first if
  // it is that one).
  bool Await(uint64_t submission);
  bool AwaitAll();

  // Runs fn once the submissions up to and including the current one have
  // completed: for objects referenced by recorded commands.
  void DestroyAfter(std::function<void()> fn);
  void DestroyCompleted();

  // With threaded submission: runs fn on the submission thread once the
  // submissions made so far are on the queue (the presenter's publish step,
  // GuestOutputPublishDeferrer), in order; without, now. AwaitQueued waits for
  // all of those queued so far.
  void QueueAfterSubmissions(std::function<void()> fn);
  void AwaitQueued();

  // Command buffers opened from now on are timed (false: not, or the device
  // can't time them).
  void SetGpuTimeEnabled(bool enabled) {
    gpu_time_enabled_ = enabled && gpu_time_pool_ != VK_NULL_HANDLE;
  }
  bool can_time_gpu() const { return gpu_time_pool_ != VK_NULL_HANDLE; }
  // Graphics card time, in nanoseconds, of the timed command buffers that
  // completed since the last call.
  uint64_t TakeGpuTimeNs();
  // native_gpu_time_by_kind: whether the kinds are timed.
  bool gpu_time_by_kind() const { return gpu_time_by_kind_; }
  // native_gpu_time_by_kind: the kind of the work being recorded in the
  // stream being written (the hoisted one or the main one; kCount: none yet),
  // and a timestamp where `kind`'s work begins there, unless the last one
  // written there began it.
  GpuKind gpu_kind() const;
  void MarkGpuKind(GpuKind kind);
  // Its nanoseconds by GpuKind since the last call (after TakeGpuTimeNs,
  // which reads the completed submissions), and the timestamps not written
  // because a command buffer had used all its own.
  void TakeGpuKindNs(uint64_t (&ns_out)[size_t(GpuKind::kCount)], uint64_t& marks_dropped_out);
  // Builds with performance counters: adds the time by kind of the
  // submissions completed since the last call to the debug overlay's and
  // perf_log_csv's counters (kGpuTimeUs, kGpuDrawUs...), as the SDK's Vulkan
  // backend.
  void FlushGpuTimePerfCounters();
  struct Counters {
    uint64_t submissions = 0;
    // EnterRenderPass beginning a render pass.
    uint64_t render_passes = 0;
    // The draws' render pass ended, by why.
    uint64_t render_pass_ends[size_t(RenderPassEnd::kCount)] = {};
    uint64_t uploads_before_render_pass = 0;
    uint64_t texture_loads_before_render_pass = 0;
    // Await calls that blocked on the graphics card, and their host ticks.
    uint64_t gpu_waits = 0;
    uint64_t gpu_wait_ticks = 0;
    // Await calls that waited for the submission thread to submit, and their
    // host ticks.
    uint64_t thread_waits = 0;
    uint64_t thread_wait_ticks = 0;
  };
  // Counted since the last call.
  Counters TakeCounters() {
    Counters counters = counters_;
    counters_ = {};
    return counters;
  }

 private:
  struct CommandBuffer {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
  };
  // A submission for the thread, or (callback set) work to run after those
  // before it.
  struct Job {
    uint64_t submission = 0;
    CommandRecorder::Streams streams;
    std::vector<std::function<void(VkCommandBuffer)>> after_replay;
    CommandBuffer command_buffer;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t gpu_time_slot = UINT32_MAX;
    std::function<void()> callback;
  };
  bool AcquireCommandBuffer(CommandBuffer& out);
  void RecycleCompletedCommandBuffers();
  // Begins command_buffer, replays the commands (`streams`, or the recorder's
  // own without), runs after_replay, writes the end timestamp, ends it.
  bool RecordCommandBuffer(VkCommandBuffer command_buffer, const CommandRecorder::Streams* streams,
                           std::vector<std::function<void(VkCommandBuffer)>>& after_replay,
                           uint32_t gpu_time_slot);
  void SubmissionThread();
  void ShutdownSubmissionThread();
  // Waits until the thread has given `submission` to the queue.
  void AwaitSubmitted(uint64_t submission);
  void InitializeGpuTime();
  // Adds the times of the timed submissions up to `completed_submission`.
  void ReadGpuTimes(uint64_t completed_submission);
  // Gives back the open command buffer's timing slot (not submitted).
  void ReleaseOpenGpuTimeSlot();

  ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;
  std::unique_ptr<ui::vulkan::VulkanGPUCompletionTimeline> timeline_;

  std::unique_ptr<CommandRecorder> recorder_;
  bool open_ = false;
  uint64_t buffer_serial_ = 0;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkFramebuffer render_pass_framebuffer_ = VK_NULL_HANDLE;
  VkExtent2D render_pass_extent_ = {};
  bool hoist_before_render_passes_ = false;
  // The open render pass's hoist point in the recorder.
  static constexpr size_t kNoHoistPoint = SIZE_MAX;
  size_t hoist_point_ = kNoHoistPoint;
  uint64_t render_pass_serial_ = 0;
  std::vector<std::function<void()>> before_submit_;
  std::function<void()> before_end_render_pass_;
  std::vector<std::function<void(VkCommandBuffer)>> after_replay_;
  std::vector<CommandBuffer> free_buffers_;
  // (submission, buffer), oldest first.
  std::deque<std::pair<uint64_t, CommandBuffer>> submitted_buffers_;
  // (last submission that may use the object, destructor), oldest first.
  std::deque<std::pair<uint64_t, std::function<void()>>> destructions_;

  // Threaded submission.
  bool threaded_submission_requested_ = false;
  std::unique_ptr<rex::thread::Thread> submission_thread_;
  std::mutex submission_mutex_;
  std::condition_variable submission_request_cond_;
  std::condition_variable submission_done_cond_;
  std::deque<Job> submission_jobs_;
  std::vector<Job> submission_free_jobs_;
  bool submission_shutdown_ = false;
  std::atomic<bool> submission_failed_{false};
  // The last submission the thread gave to the queue (the timeline's
  // submitted limit).
  std::atomic<uint64_t> submission_submitted_{0};
  uint64_t callbacks_queued_ = 0;
  std::atomic<uint64_t> callbacks_done_{0};

  // Timing: a ring of slots of timestamp queries (start, end, and with
  // native_gpu_time_by_kind kGpuKindMarks more: RDR's command buffers needed
  // more than 254), taken in submission order and freed in completion order.
  static constexpr uint32_t kGpuTimeSlots = 256;
  static constexpr uint32_t kGpuTimeSlotsByKind = 48;
  static constexpr uint32_t kGpuKindMarks = 2046;
  static constexpr uint32_t kNoGpuTimeSlot = UINT32_MAX;
  VkQueryPool gpu_time_pool_ = VK_NULL_HANDLE;
  double gpu_time_ns_per_tick_ = 0.0;
  uint64_t gpu_time_valid_mask_ = 0;
  bool gpu_time_enabled_ = false;
  bool gpu_time_by_kind_ = false;
  // Queries a slot has, and slots in the ring.
  uint32_t gpu_time_slot_queries_ = 2;
  uint32_t gpu_time_slot_count_ = kGpuTimeSlots;
  uint32_t open_gpu_time_slot_ = kNoGpuTimeSlot;
  uint32_t gpu_time_next_slot_ = 0;
  uint32_t gpu_time_slots_used_ = 0;
  struct GpuTimeSubmission {
    uint64_t submission = 0;
    uint32_t slot = 0;
    // The kind of work at the start stamp, and what each mark (queries 2...)
    // began, in recording order.
    GpuKind start_kind = GpuKind::kOther;
    std::vector<GpuKind> marks;
  };
  // Oldest first.
  std::deque<GpuTimeSubmission> gpu_time_submitted_;
  // The open command buffer's.
  GpuKind open_gpu_start_kind_ = GpuKind::kOther;
  std::vector<GpuKind> open_gpu_marks_;
  // The kind the last mark in each stream began (kCount: none yet): the main
  // stream's, and the open render pass's hoisted commands' (which start after
  // its draws mark).
  GpuKind gpu_kind_main_ = GpuKind::kOther;
  GpuKind gpu_kind_hoisted_ = GpuKind::kCount;
  bool gpu_kind_hoisted_marked_ = false;
  uint64_t gpu_time_ticks_ = 0;
  uint64_t gpu_kind_ticks_[size_t(GpuKind::kCount)] = {};
  uint64_t gpu_kind_perf_ticks_[size_t(GpuKind::kCount)] = {};
  uint64_t gpu_kind_marks_dropped_ = 0;
  std::vector<uint64_t> gpu_time_stamps_;
  std::vector<std::pair<uint64_t, GpuKind>> gpu_time_sorted_marks_;
  Counters counters_;
};

}  // namespace rex::graphics::native
