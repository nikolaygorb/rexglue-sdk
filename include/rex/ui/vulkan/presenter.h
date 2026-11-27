#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/ui/presenter.h>
#include <rex/ui/surface.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/dlss_frame_generation.h>
#include <rex/ui/vulkan/frame_generation.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/gpu_completion_timeline.h>
#include <rex/ui/vulkan/ui_samplers.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanUIDrawContext final : public UIDrawContext {
 public:
  VulkanUIDrawContext(Presenter& presenter, uint32_t render_target_width,
                      uint32_t render_target_height, VkCommandBuffer draw_command_buffer,
                      uint64_t submission_index_current, uint64_t submission_index_completed,
                      VkRenderPass render_pass, VkFormat render_pass_format)
      : UIDrawContext(presenter, render_target_width, render_target_height),
        draw_command_buffer_(draw_command_buffer),
        submission_index_current_(submission_index_current),
        submission_index_completed_(submission_index_completed),
        render_pass_(render_pass),
        render_pass_format_(render_pass_format) {}

  VkCommandBuffer draw_command_buffer() const { return draw_command_buffer_; }
  uint64_t submission_index_current() const { return submission_index_current_; }
  uint64_t submission_index_completed() const { return submission_index_completed_; }
  VkRenderPass render_pass() const { return render_pass_; }
  VkFormat render_pass_format() const { return render_pass_format_; }

 private:
  VkCommandBuffer draw_command_buffer_;
  uint64_t submission_index_current_;
  uint64_t submission_index_completed_;
  // Has 1 subpass with a single render_pass_format_ attachment.
  VkRenderPass render_pass_;
  VkFormat render_pass_format_;
};

class VulkanNisUpscaler;

class VulkanPresenter final : public Presenter {
 public:
  // Maximum number of different guest output image versions still potentially
  // considered alive that may be given to the refresher - this many instances
  // of dependent objects (such as framebuffers) may need to be kept by the
  // refresher across invocations (due to multiple-buffering of guest output
  // images inside the presenter, different versions may be given even every
  // invocation), to avoid recreation of dependent objects every frame.
  static constexpr size_t kMaxActiveGuestOutputImageVersions = kGuestOutputMailboxSize;

  static constexpr VkFormat kGuestOutputFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  // The guest output is expected to be acquired and released in this state by
  // the refresher. The exception is the first write to the current guest output
  // image - in this case, a barrier is only needed from
  // VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT without access. Also if the image is
  // being refreshed for the first time, it's in VK_IMAGE_LAYOUT_UNDEFINED (but
  // it's safe, and preferred, to transition it from VK_IMAGE_LAYOUT_UNDEFINED
  // when writing to it in general).
  static constexpr VkPipelineStageFlagBits kGuestOutputInternalStageMask =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  static constexpr VkAccessFlags kGuestOutputInternalAccessMask = VK_ACCESS_SHADER_READ_BIT;
  static constexpr VkImageLayout kGuestOutputInternalLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  // Frame generation inputs of a guest output image (ui/vulkan/frame_generation.h):
  // all of the frontbuffer's size, acquired and released like the guest output
  // image (kGuestOutputInternalLayout; from UNDEFINED at the first write).
  static constexpr VkFormat kFrameGenerationDepthFormat = VK_FORMAT_R32_SFLOAT;
  static constexpr VkFormat kFrameGenerationMotionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  struct FrameGenerationTargets {
    // The frame without the HUD, as the guest output image (kGuestOutputFormat).
    VkImage hudless = VK_NULL_HANDLE;
    VkImageView hudless_view = VK_NULL_HANDLE;
    uint64_t hudless_version = 0;
    // Guest clip z/w (storage).
    VkImage depth = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    // xy: current -> previous pixels without jitter, zw unused (storage).
    VkImage motion = VK_NULL_HANDLE;
    VkImageView motion_view = VK_NULL_HANDLE;
    bool ever_written_previously = false;
    // native_frame_generation_debug: 4 asks for the inputs drawn into `hudless`.
    int32_t debug = 0;
    // Set valid once all three are written.
    FrameGenerationInputs* inputs = nullptr;
  };

  // The callback must use the graphics and compute queue 0 of the device.
  class VulkanGuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    VulkanGuestOutputRefreshContext(bool& is_8bpc_out_ref, VkImage image, VkImageView image_view,
                                    uint64_t image_version, bool image_ever_written_previously,
                                    const FrameGenerationTargets* frame_generation = nullptr)
        : GuestOutputRefreshContext(is_8bpc_out_ref),
          image_(image),
          image_view_(image_view),
          image_version_(image_version),
          image_ever_written_previously_(image_ever_written_previously),
          frame_generation_(frame_generation) {}

    // The format is kGuestOutputFormat.
    // Supports usage as a color attachment and as a sampled image, as well as
    // transfer source (but the reason of that is guest output capturing).
    VkImage image() const { return image_; }
    VkImageView image_view() const { return image_view_; }
    uint64_t image_version() const { return image_version_; }
    // Whether a proper barrier must be done to acquire the image.
    bool image_ever_written_previously() const { return image_ever_written_previously_; }
    // Non-null while the presenter generates frames and wants this frame's inputs.
    const FrameGenerationTargets* frame_generation() const { return frame_generation_; }

   private:
    VkImage image_;
    VkImageView image_view_;
    uint64_t image_version_;
    bool image_ever_written_previously_;
    const FrameGenerationTargets* frame_generation_;
  };

  static std::unique_ptr<VulkanPresenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                 VulkanDevice* vulkan_device,
                                                 const UISamplers* ui_samplers) {
    auto presenter = std::unique_ptr<VulkanPresenter>(
        new VulkanPresenter(host_gpu_loss_callback, vulkan_device, ui_samplers));
    if (!presenter->InitializeSurfaceIndependent()) {
      return nullptr;
    }
    return presenter;
  }

  ~VulkanPresenter();

  VulkanDevice* vulkan_device() const { return vulkan_device_; }

  static Surface::TypeFlags GetSurfaceTypesSupportedByInstance(
      const VulkanInstance::Extensions& instance_extensions);
  Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  bool CaptureGuestOutput(RawImage& image_out) override;

  void AwaitUISubmissionCompletionFromUIThread(uint64_t submission_index) {
    ui_completion_timeline_.AwaitSubmissionAndUpdateCompleted(submission_index);
  }
  VkCommandBuffer AcquireUISetupCommandBufferFromUIThread();

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;

  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override;
  void GuestOutputRefreshSubmitted(uint32_t mailbox_index) override;

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  // Usable for both the guest output image itself and for intermediate images.
  class GuestOutputImage {
   public:
    static std::unique_ptr<GuestOutputImage> Create(const VulkanDevice* const vulkan_device,
                                                    const uint32_t width, const uint32_t height,
                                                    VkFormat format = kGuestOutputFormat) {
      assert_not_zero(width);
      assert_not_zero(height);
      auto image =
          std::unique_ptr<GuestOutputImage>(new GuestOutputImage(vulkan_device, width, height));
      image->format_ = format;
      if (!image->Initialize()) {
        return nullptr;
      }
      return std::move(image);
    }

    GuestOutputImage(const GuestOutputImage& image) = delete;
    GuestOutputImage& operator=(const GuestOutputImage& image) = delete;
    ~GuestOutputImage();

    const VkExtent2D& extent() const { return extent_; }

    VkImage image() const { return image_; }
    VkDeviceMemory memory() const { return memory_; }
    VkImageView view() const { return view_; }
    VkFormat format() const { return format_; }

    // Frame generation inputs (a guest output image's), living as long as it.
    bool CreateFrameGenerationImages();
    const GuestOutputImage* frame_generation_hudless() const { return fg_hudless_.get(); }
    const GuestOutputImage* frame_generation_depth() const { return fg_depth_.get(); }
    const GuestOutputImage* frame_generation_motion() const { return fg_motion_.get(); }

   private:
    GuestOutputImage(const VulkanDevice* const vulkan_device, const uint32_t width,
                     const uint32_t height)
        : vulkan_device_(vulkan_device) {
      extent_.width = width;
      extent_.height = height;
    }

    bool Initialize();

    const VulkanDevice* vulkan_device_;

    VkExtent2D extent_;
    VkFormat format_ = kGuestOutputFormat;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    std::unique_ptr<GuestOutputImage> fg_hudless_, fg_depth_, fg_motion_;
  };

  struct GuestOutputImageInstance {
    // Refresher-side reference (painting has its own references for the purpose
    // of destruction only after painting is done on the GPU).
    std::shared_ptr<GuestOutputImage> image;
    uint64_t version = UINT64_MAX;
    uint64_t hudless_version = UINT64_MAX;  // Of its frame generation hudless image.
    uint64_t last_refresher_submission = 0;
    // For choosing the barrier stage and access mask and layout depending on
    // whether the image has previously been written. If an image is active
    // after a refresh, it can be assumed that this is true.
    bool ever_successfully_refreshed = false;

    void SetToNewImage(const std::shared_ptr<GuestOutputImage>& new_image, uint64_t new_version) {
      image = new_image;
      version = new_version;
      hudless_version = UINT64_MAX;
      last_refresher_submission = 0;
      ever_successfully_refreshed = false;
    }
  };

  struct GuestOutputPaintRectangleConstants {
    union {
      struct {
        float x;
        float y;
      };
      float offset[2];
    };
    union {
      struct {
        float width;
        float height;
      };
      float size[2];
    };
  };

  enum GuestOutputPaintPipelineLayoutIndex : size_t {
    kGuestOutputPaintPipelineLayoutIndexBilinear,
    kGuestOutputPaintPipelineLayoutIndexCasSharpen,
    kGuestOutputPaintPipelineLayoutIndexCasResample,
    kGuestOutputPaintPipelineLayoutIndexFsrEasu,
    kGuestOutputPaintPipelineLayoutIndexFsrRcas,

    kGuestOutputPaintPipelineLayoutCount,
  };

  static constexpr GuestOutputPaintPipelineLayoutIndex GetGuestOutputPaintPipelineLayoutIndex(
      GuestOutputPaintEffect effect) {
    switch (effect) {
      case GuestOutputPaintEffect::kBilinear:
      case GuestOutputPaintEffect::kBilinearDither:
        return kGuestOutputPaintPipelineLayoutIndexBilinear;
      case GuestOutputPaintEffect::kCasSharpen:
      case GuestOutputPaintEffect::kCasSharpenDither:
        return kGuestOutputPaintPipelineLayoutIndexCasSharpen;
      case GuestOutputPaintEffect::kCasResample:
      case GuestOutputPaintEffect::kCasResampleDither:
        return kGuestOutputPaintPipelineLayoutIndexCasResample;
      case GuestOutputPaintEffect::kFsrEasu:
        return kGuestOutputPaintPipelineLayoutIndexFsrEasu;
      case GuestOutputPaintEffect::kFsrRcas:
      case GuestOutputPaintEffect::kFsrRcasDither:
        return kGuestOutputPaintPipelineLayoutIndexFsrRcas;
      default:
        assert_unhandled_case(effect);
        return kGuestOutputPaintPipelineLayoutCount;
    }
  }

  struct PaintContext {
    class Submission {
     public:
      static std::unique_ptr<Submission> Create(const VulkanDevice* const vulkan_device) {
        auto submission = std::unique_ptr<Submission>(new Submission(vulkan_device));
        if (!submission->Initialize()) {
          return nullptr;
        }
        return submission;
      }

      Submission(const Submission& submission) = delete;
      Submission& operator=(const Submission& submission) = delete;
      ~Submission();

      VkSemaphore acquire_semaphore() const { return acquire_semaphore_; }
      VkCommandPool draw_command_pool() const { return draw_command_pool_; }
      VkCommandBuffer draw_command_buffer() const { return draw_command_buffer_; }

     private:
      explicit Submission(const VulkanDevice* const vulkan_device)
          : vulkan_device_(vulkan_device) {}
      bool Initialize();

      const VulkanDevice* vulkan_device_;
      VkSemaphore acquire_semaphore_ = VK_NULL_HANDLE;
      VkCommandPool draw_command_pool_ = VK_NULL_HANDLE;
      VkCommandBuffer draw_command_buffer_ = VK_NULL_HANDLE;
    };

    static constexpr uint32_t kSubmissionCount = 3;

    struct GuestOutputPaintPipeline {
      // Created on initialization.
      VkPipeline intermediate_pipeline = VK_NULL_HANDLE;
      // Created during guest output painting (after awaiting the last guest
      // output paint if outdated and needs to be recreated), when needed, for
      // the up-to-date render pass that draws to the swapchain with the actual
      // image format.
      VkPipeline swapchain_pipeline = VK_NULL_HANDLE;
      VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    };

    enum GuestOutputDescriptorSet : uint32_t {
      kGuestOutputDescriptorSetGuestOutput0Sampled,

      kGuestOutputDescriptorSetIntermediate0Sampled =
          kGuestOutputDescriptorSetGuestOutput0Sampled + kGuestOutputMailboxSize,

      // The frame generation hudless image of each guest output paint reference.
      kGuestOutputDescriptorSetHudless0Sampled =
          kGuestOutputDescriptorSetIntermediate0Sampled + kMaxGuestOutputPaintEffects - 1,

      kGuestOutputDescriptorSetCount =
          kGuestOutputDescriptorSetHudless0Sampled + kGuestOutputMailboxSize,
    };

    struct UISetupCommandBuffer {
      UISetupCommandBuffer(VkCommandPool command_pool, VkCommandBuffer command_buffer,
                           uint64_t last_usage_submission_index = 0)
          : command_pool(command_pool),
            command_buffer(command_buffer),
            last_usage_submission_index(last_usage_submission_index) {}

      VkCommandPool command_pool;
      VkCommandBuffer command_buffer;
      uint64_t last_usage_submission_index;
    };

    struct SwapchainFramebuffer {
      SwapchainFramebuffer(VkImageView image_view, VkFramebuffer framebuffer)
          : image_view(image_view), framebuffer(framebuffer) {}

      VkImageView image_view;
      VkFramebuffer framebuffer;
    };

    explicit PaintContext(VulkanDevice* const vulkan_device)
        : vulkan_device(vulkan_device), completion_timeline(vulkan_device) {}
    PaintContext(const PaintContext& paint_context) = delete;
    PaintContext& operator=(const PaintContext& paint_context) = delete;

    // The old swapchain, if passed, should be assumed to be retired after this
    // call (though it may fail before the vkCreateSwapchainKHR that will
    // technically retire it, so it will be in an undefined state), and needs to
    // be destroyed externally no matter what the result is.
    // frame_generation: its proxy swapchain first (old_swapchain must be null
    // then); frame_generation_out tells which kind was created.
    // dlss_frame_generation: a regular swapchain with the copies and extra
    // images DLSS frame generation needs where the surface allows them (its
    // minImageCount and the images' usage out).
    static VkSwapchainKHR CreateSwapchainForVulkanSurface(
        const VulkanDevice* vulkan_device, VkSurfaceKHR surface, uint32_t width, uint32_t height,
        VkSwapchainKHR old_swapchain, uint32_t& present_queue_family_out,
        VkFormat& image_format_out, VkExtent2D& image_extent_out, bool& is_fifo_out,
        bool& ui_surface_unusable_out, VulkanFrameGeneration* frame_generation = nullptr,
        bool* frame_generation_out = nullptr, bool dlss_frame_generation = false,
        uint32_t* surface_min_image_count_out = nullptr, VkImageUsageFlags* image_usage_out = nullptr);
    // Destroys a swapchain PrepareForSwapchainRetirement returned, of the kind
    // swapchain_frame_generation said before it.
    void DestroyRetiredSwapchain(VkSwapchainKHR old_swapchain, bool was_frame_generation);
    void DestroyFrameGenerationHudless();

    // Destroys the swapchain and its derivatives, nulls `swapchain` and returns
    // the original swapchain object, if it existed, for use as oldSwapchain if
    // needed and for destruction.
    VkSwapchainKHR PrepareForSwapchainRetirement();
    // May be called from the destructor of the presenter.
    void DestroySwapchainAndVulkanSurface();

    // Connection-indepedent.

    VulkanDevice* vulkan_device;

    std::array<std::unique_ptr<PaintContext::Submission>, kSubmissionCount> submissions;
    VulkanGPUCompletionTimeline completion_timeline;

    std::array<GuestOutputPaintPipeline, size_t(GuestOutputPaintEffect::kCount)>
        guest_output_paint_pipelines;

    VkDescriptorPool guest_output_descriptor_pool = VK_NULL_HANDLE;
    // Descriptors are updated while painting if they're out of date.
    VkDescriptorSet guest_output_descriptor_sets[kGuestOutputDescriptorSetCount];

    // Refreshed and cleaned up during guest output painting. The first is the
    // paint submission index in which the guest output image (and its
    // descriptors) was last used, the second is the reference to the image,
    // which may be null. The indices are not mailbox indices here, rather, if
    // the reference is not in this array yet, the most outdated reference, if
    // needed, is replaced with the new one, awaiting the usage completion of
    // the last paint usage.
    std::array<std::pair<uint64_t, std::shared_ptr<GuestOutputImage>>, kGuestOutputMailboxSize>
        guest_output_image_paint_refs;
    // The latest submission index at which any guest output image was drawn.
    uint64_t guest_output_image_paint_last_submission = 0;

    // Current intermediate images for guest output painting, refreshed when
    // painting guest output.
    std::array<std::unique_ptr<GuestOutputImage>, kMaxGuestOutputPaintEffects - 1>
        guest_output_intermediate_images;
    // Created and destroyed alongside the images. UNORM only.
    std::array<VkFramebuffer, kMaxGuestOutputPaintEffects - 1>
        guest_output_intermediate_framebuffers = {};
    uint64_t guest_output_intermediate_image_last_submission = 0;

    // Command buffers optionally executed before the draw command buffer,
    // outside the painting render pass.
    std::vector<UISetupCommandBuffer> ui_setup_command_buffers;
    size_t ui_setup_command_buffer_current_index = SIZE_MAX;

    // Connection-specific.

    // May be reused between connections if the format stays the same.
    VkRenderPass swapchain_render_pass = VK_NULL_HANDLE;
    VkFormat swapchain_render_pass_format = VK_FORMAT_UNDEFINED;
    bool swapchain_render_pass_clear_load_op = false;

    VkSurfaceKHR vulkan_surface = VK_NULL_HANDLE;
    uint32_t present_queue_family = UINT32_MAX;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D swapchain_extent = {};
    bool swapchain_is_fifo = false;
    std::vector<VkImage> swapchain_images;
    std::vector<SwapchainFramebuffer> swapchain_framebuffers;
    std::vector<VkSemaphore> swapchain_image_present_semaphores;

    // Frame generation (the presenter's, may be null): `swapchain` is its proxy
    // while swapchain_frame_generation.
    VulkanFrameGeneration* frame_generation = nullptr;
    bool swapchain_frame_generation = false;
    // DLSS frame generation (the presenter's, may be null): generating on the
    // regular `swapchain` while swapchain_dlss_frame_generation.
    VulkanDlssFrameGeneration* dlss_frame_generation = nullptr;
    bool swapchain_dlss_frame_generation = false;
    uint32_t swapchain_surface_min_image_count = 0;
    VkImageUsageFlags swapchain_image_usage = 0;
    // The frame without the HUD painted like the guest output (swapchain size
    // and format, left in SHADER_READ_ONLY_OPTIMAL), and its pass.
    VkRenderPass fg_hudless_render_pass = VK_NULL_HANDLE;
    VkFormat fg_hudless_render_pass_format = VK_FORMAT_UNDEFINED;
    // Two, alternating per generated frame: with async generation the proxy
    // still reads the last one while the next paint writes (AMD's rule).
    std::array<VkImage, 2> fg_hudless_image{};
    std::array<VkDeviceMemory, 2> fg_hudless_memory{};
    std::array<VkImageView, 2> fg_hudless_view{};
    std::array<VkFramebuffer, 2> fg_hudless_framebuffer{};
    uint32_t fg_hudless_index = 0;
    // Neutral inputs for frames without the title's (8x8: far depth for a
    // forward and an inverted depth, still motion), cleared at the first paint.
    std::unique_ptr<GuestOutputImage> fg_neutral_depth[2], fg_neutral_motion;
    bool fg_neutral_ready = false;
    // The last frame presented with inputs (its camera and rectangle), and the
    // frames since presented with neutral inputs.
    VulkanFrameGeneration::Frame fg_last_generated{};
    bool fg_has_last_generated = false;
    uint32_t fg_gap_frames = 0;
    // The producer frame of the last real frame presented with generation inputs.
    uint64_t fg_last_frame = 0;
    std::chrono::steady_clock::time_point fg_last_real_present{};
    std::chrono::steady_clock::time_point fg_stats_since{};
    uint64_t fg_stats_real = 0, fg_stats_generating = 0, fg_stats_presents_base = 0;
    uint64_t fg_stats_held_base = 0, fg_stats_resets_base = 0;
  };

  // Out of line: the members include a std::unique_ptr of a type complete
  // only in vulkan_presenter.cpp.
  explicit VulkanPresenter(HostGpuLossCallback host_gpu_loss_callback,
                           VulkanDevice* vulkan_device, const UISamplers* ui_samplers);

  bool InitializeSurfaceIndependent();

  [[nodiscard]] VkPipeline CreateGuestOutputPaintPipeline(GuestOutputPaintEffect effect,
                                                          VkRenderPass render_pass);

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  bool EnsureTemporalUpscalerContext(uint32_t render_width, uint32_t render_height,
                                     uint32_t output_width, uint32_t output_height);
  bool DispatchTemporalUpscaler(VkCommandBuffer command_buffer, VkImage input_image,
                                uint32_t input_width, uint32_t input_height, VkImage output_image,
                                uint32_t output_width, uint32_t output_height,
                                const GuestOutputPaintConfig& config);
  void DestroyTemporalUpscalerContext();
#endif

  // The frame generation hudless image for the swapchain's size and format
  // (and its render pass), at a frame generation swapchain's connection.
  bool CreateFrameGenerationHudless();

  // NVIDIA Image Scaling in the first EASU slot (present_effect = "nis"),
  // recorded into command_buffer of the paint `submission`. False if it is
  // unavailable, then the slot is drawn with EASU.
  bool DispatchNisUpscaler(VkCommandBuffer command_buffer, uint64_t submission,
                           const GuestOutputImage& input, uint32_t input_width,
                           uint32_t input_height, const GuestOutputImage& output,
                           uint32_t output_width, uint32_t output_height,
                           const GuestOutputPaintConfig& config);

  VulkanDevice* vulkan_device_;
  const UISamplers* ui_samplers_;

  // AMD FSR frame generation (native_frame_generation): created with the
  // presenter; used by surface connection and painting (one thread at a time).
  std::unique_ptr<VulkanFrameGeneration> frame_generation_;
  // The refresher's last frame while frame generation runs (its frame rate cap).
  std::chrono::steady_clock::time_point frame_generation_last_refresh_{};
  // Painting's: generation is on (the refresher caps the frame rate only then).
  std::atomic<bool> frame_generation_generating_{false};
  // NVIDIA DLSS frame generation (native_frame_generation = dlss): created with
  // the presenter, generating on the regular swapchain (dlss_frame_generation.h).
  std::unique_ptr<VulkanDlssFrameGeneration> dlss_frame_generation_;

  // Created on the first NIS paint (paint thread), destroyed after the paint
  // submissions complete.
  std::unique_ptr<VulkanNisUpscaler> nis_upscaler_;
  bool nis_upscaler_attempted_ = false;

  // Static objects for guest output presentation, used only when painting the
  // main target (can be destroyed only after awaiting main target usage
  // completion).
  VkDescriptorSetLayout guest_output_paint_image_descriptor_set_layout_ = VK_NULL_HANDLE;
  std::array<VkPipelineLayout, kGuestOutputPaintPipelineLayoutCount>
      guest_output_paint_pipeline_layouts_ = {};
  VkShaderModule guest_output_paint_vs_ = VK_NULL_HANDLE;
  std::array<VkShaderModule, size_t(GuestOutputPaintEffect::kCount)> guest_output_paint_fs_ = {};
  // Not compatible with the swapchain render pass even if the format is the
  // same due to different dependencies (this is shader read > color
  // attachment > shader read).
  VkRenderPass guest_output_intermediate_render_pass_ = VK_NULL_HANDLE;

  // Value monotonically increased every time a new guest output image is
  // initialized, for recreation of dependent objects such as framebuffers in
  // the refreshers - saving and comparing the handle in the refresher is not
  // enough as Create > Destroy > Create may result in the same handle for
  // actually different objects without the refresher being aware of the
  // destruction.
  uint64_t guest_output_image_next_version_ = 0;
  std::array<GuestOutputImageInstance, kGuestOutputMailboxSize> guest_output_images_;
  VulkanGPUCompletionTimeline guest_output_image_refresher_completion_timeline_;

  // UI submission completion timeline with the submission index that can be
  // given to UI drawers (accessible from the UI thread only, at any time).
  VulkanGPUCompletionTimeline ui_completion_timeline_;

  // Accessible only by painting and by surface connection lifetime management
  // (ConnectOrReconnectPaintingToSurfaceFromUIThread,
  // DisconnectPaintingFromSurfaceFromUIThreadImpl) by the thread doing it, as
  // well as by presenter initialization and shutdown.
  PaintContext paint_context_;

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  void* temporal_upscaler_context_ = nullptr;
  uint32_t temporal_upscaler_max_render_width_ = 0;
  uint32_t temporal_upscaler_max_render_height_ = 0;
  uint32_t temporal_upscaler_max_output_width_ = 0;
  uint32_t temporal_upscaler_max_output_height_ = 0;
  bool temporal_upscaler_provider_logged_ = false;
#endif
};

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
