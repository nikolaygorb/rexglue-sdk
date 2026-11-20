/**
 * @file        graphics/native/texture_cache.h
 * @brief       Native renderer: guest textures on the host
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Ported from the SDK's VulkanTextureCache (Xenia, Copyright 2022 Ben Vanik, BSD
 *              license): its host format table, image creation, views, loading through the
 *              SDK's texture load shaders, bindings and samplers, without its resolution
 *              scaling, direct loads, replacement textures and resolve copies.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/graphics/native/command_recorder.h>
#include <rex/assert.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/mem_alloc.h>

namespace rex::graphics::native {

class NativeSharedMemory;
class Submitter;

// The Vulkan source of a component of a Xenos host swizzle (3 bits per
// component); IDENTITY where it's the component itself, as the portability
// subset requires.
inline VkComponentSwizzle GuestComponentSwizzle(uint32_t texture_swizzle,
                                                uint32_t component_index) {
  xenos::XE_GPU_TEXTURE_SWIZZLE component =
      xenos::XE_GPU_TEXTURE_SWIZZLE((texture_swizzle >> (3 * component_index)) & 0b111);
  if (component == xenos::XE_GPU_TEXTURE_SWIZZLE(component_index)) {
    return VK_COMPONENT_SWIZZLE_IDENTITY;
  }
  switch (component) {
    case xenos::XE_GPU_TEXTURE_SWIZZLE_R:
      return VK_COMPONENT_SWIZZLE_R;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_G:
      return VK_COMPONENT_SWIZZLE_G;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_B:
      return VK_COMPONENT_SWIZZLE_B;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_A:
      return VK_COMPONENT_SWIZZLE_A;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_0:
      return VK_COMPONENT_SWIZZLE_ZERO;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_1:
      return VK_COMPONENT_SWIZZLE_ONE;
    default:
      return VK_COMPONENT_SWIZZLE_IDENTITY;
  }
}

// The SDK's TextureCache (keys, bindings from the fetch constants, signedness
// and swizzles for the translated shaders, write watches through the shared
// memory, the memory limits) with the native renderer's Vulkan half. Textures
// are loaded with the SDK's precompiled texture load compute shaders from the
// shared memory into a scratch buffer, then copied into the image; textures
// at resolution scale 1 only (scaling is milestone N6). Command processor
// thread only; every command goes outside the draws' render pass.
class NativeTextureCache final : public TextureCache {
 public:
  // As the SDK's VulkanTextureCache::SamplerParameters.
  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;         // 3
      xenos::ClampMode clamp_y : 3;         // 6
      xenos::ClampMode clamp_z : 3;         // 9
      xenos::BorderColor border_color : 2;  // 11
      uint32_t mag_linear : 1;              // 12
      uint32_t min_linear : 1;              // 13
      uint32_t mip_linear : 1;              // 14
      xenos::AnisoFilter aniso_filter : 3;  // 17
      uint32_t mip_min_level : 4;           // 21
      uint32_t mip_base_map : 1;            // 22
      // The border colour's alpha forced to 1 (with a border clamp mode).
      uint32_t force_bc_w_to_max : 1;  // 23
    };
    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
  };

  NativeTextureCache(const RegisterFile& register_file, NativeSharedMemory& shared_memory,
                     Submitter& submitter);
  ~NativeTextureCache() override;

  bool Initialize();

  // To call before the textures of a draw are requested: tells the base the
  // submission its textures are used in, and destroys what the completed
  // submissions no longer use. poll_completed = false: without asking the
  // GPU which submissions completed (a fence query; draws leave it to the
  // frame's end, where it is done anyway).
  void UpdateSubmissions(bool poll_completed = true);
  // Changes whenever a binding's views may have changed (a texture bound,
  // replaced or reset in a fetch constant's binding).
  uint64_t binding_generation() const { return binding_generation_; }
  // The same for one fetch constant's binding.
  uint64_t binding_generation(uint32_t fetch_constant_index) const {
    return binding_generations_[fetch_constant_index & (xenos::kTextureFetchConstantCount - 1)];
  }

  // Loads the used textures and moves them to the sampled layout (outside the
  // render pass).
  void RequestTextures(uint32_t used_texture_mask) override;
  // After the draw entered its render pass: the used textures (and 3D-as-2D
  // copies) as sampled in it, so they are no longer loaded or changed before
  // it (native_hoist_texture_loads).
  void MarkSampledInRenderPass(uint32_t used_texture_mask);
  // After RequestTextures: of the fetch constants the shaders fetch as 1D or
  // 2D, the 3D textures get a 2D copy of their first slice, loaded and
  // sampled (gpu_3d_to_2d_texture, as the SDK's backends; reloaded when the
  // guest rewrites it, as its Direct3D 12 one).
  void Request3DAs2DTextures(uint32_t fetched_as_2d_mask);

  // The view of the binding, VK_NULL_HANDLE if there is none (an invalid or
  // unsupported texture, or a 3D texture fetched as 2D without its copy): the
  // caller binds its placeholder.
  VkImageView GetActiveBindingImageView(uint32_t fetch_constant_index,
                                        xenos::FetchOpDimension dimension, bool is_signed);

  // For textures the game rendered, sampled at the resolution scale
  // (Composites): the bound 2D texture of a fetch constant (one layer, read
  // unsigned only), its unsigned host format, its view with the identity
  // swizzle (a 2D array, in the sampled layout) and its guest memory size.
  // False if there is no such texture.
  struct BoundTexture2D {
    TextureKey key;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageView identity_view = VK_NULL_HANDLE;
    uint32_t guest_size = 0;
  };
  bool GetActiveBoundTexture2D(uint32_t fetch_constant_index, BoundTexture2D& out);

  // The front buffer from guest memory (texture fetch constant 0, which
  // VdSwap sets), as the SDK's RequestSwapTexture: a 2D view in the sampled
  // layout with the fetch's swizzle, and the texture's size and format;
  // VK_NULL_HANDLE if the fetch constant isn't a 2D texture or the load fails.
  VkImageView RequestSwapTexture(uint32_t& width_out, uint32_t& height_out,
                                 xenos::TextureFormat& format_out);

  // Resolves at 1x straight into the textures the game samples them as
  // (native_resolve_into_textures; the SDK's VulkanTextureCache::
  // CopyResolveIntoTexture, there 32bpp colour only). A resolve whose
  // rectangle starts at the destination's origin, described by its guest
  // memory layout: a texture made for it exists when one with the key a fetch
  // constant gives (2D, one level, tiled) is of the resolve's size or up to 7
  // texels narrower or shorter (a resolve covers whole 8x8 blocks).
  struct ResolveDestination {
    // Physical, 4 KB aligned.
    uint32_t base = 0;
    // Aligned to 32 texels.
    uint32_t pitch_texels = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
    xenos::Endian endianness = xenos::Endian::kNone;
  };
  enum class ResolveCopy : uint32_t {
    kCopied,
    // No texture is made for the destination (usually not sampled yet).
    kNoTexture,
    // A replacement, with mips, or a format the load converts (not the
    // image's values bit for bit).
    kFormat,
    // Red and blue swapped by the resolve, and image views can't swap them
    // back on this device.
    kSwap,
    // Copied, but its memory isn't valid as a whole: loaded again anyway.
    kNotMarked,
    kCount,
  };
  bool HasResolveDestinationTexture(const ResolveDestination& destination);
  // The host format unsigned textures of `format` are kept in on this device
  // (after the fallbacks for formats it can't filter).
  VkFormat GetUnsignedHostFormat(xenos::TextureFormat format) const {
    return host_formats_[uint32_t(format) & 63].format_unsigned.format;
  }
  // Copies the texture's part of the source image (the resolve's host image,
  // at 1x, the destination's origin at its (0, 0), in
  // VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) into the texture and marks it up to
  // date: it isn't loaded again from the memory the resolve wrote, until that
  // changes. source_is_depth: the image holds depth as floats, as the texture
  // cache's k_24_8 / k_24_8_FLOAT. Outside a render pass.
  ResolveCopy CopyResolveIntoTexture(CommandRecorder* command_buffer,
                                     const ResolveDestination& destination, VkImage source_image,
                                     VkFormat source_format, bool source_is_depth,
                                     bool red_blue_swapped);

  SamplerParameters GetSamplerParameters(const SpirvShader::SamplerBinding& binding) const;
  // VK_NULL_HANDLE if it can't be created (or the device's sampler count is
  // used up: there is no eviction yet).
  VkSampler UseSampler(SamplerParameters parameters);
  // UseSampler(GetSamplerParameters(binding)), the last answer for its fetch
  // constant while the constant's words, the binding's filters and
  // anisotropic_override stay the same (the draws' samplers are mostly the
  // same as the last time).
  VkSampler GetSampler(const SpirvShader::SamplerBinding& binding);

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;
  void UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) override;

 private:
  struct HostFormat {
    LoadShaderIndex load_shader;
    // Normalized and float formats only: integer formats aren't filterable
    // (fixed-point guest values are scaled after sampling instead).
    VkFormat format;
    // Block-compressed on the host too (not decompressed when loading).
    bool block_compressed;
    // Set up for the device.
    bool linear_filterable;
  };
  struct HostFormatPair {
    HostFormat format_unsigned;
    HostFormat format_signed;
    // Xenos swizzle components to the host format's components.
    uint32_t swizzle;
    // Whether the signed and unsigned formats can share an image.
    bool unsigned_signed_compatible;
  };

  class NativeTexture final : public Texture {
   public:
    enum class Usage {
      kUndefined,
      kTransferDestination,
      kSampled,
    };
    // Takes the image and its memory. Without track_usage, the texture isn't
    // in the base's usage list (a 3D-as-2D copy, owned by its 3D texture).
    NativeTexture(NativeTextureCache& texture_cache, const TextureKey& key, VkImage image,
                  VmaAllocation allocation, bool track_usage = true);
    ~NativeTexture() override;

    // Submitter::render_pass_serial() of the last draws' render pass a draw
    // sampled it in (MarkSampledInRenderPass): its loads and layout changes
    // may go before the open render pass only while this isn't that one's.
    uint64_t render_pass_sampled_serial = 0;
    // Diagnostics only, updated after a resident load has recorded successfully.
    uint64_t resident_load_count = 0;
    uint64_t resident_load_submission = UINT64_MAX;
    uint32_t resident_loaded_parts = 0;
    WatchStats resident_watch_snapshot;
    VkImage image() const { return image_; }
    Usage usage() const { return usage_; }
    void set_usage(Usage usage) { usage_ = usage; }
    // The image has red and blue the other way than the guest memory
    // (CopyResolveIntoTexture of a resolve that swaps them): its views swap
    // them back. Bindings using it need RebindTexture after a change.
    bool red_blue_swapped() const { return red_blue_swapped_; }
    void set_red_blue_swapped(bool swapped) {
      red_blue_swapped_ = swapped;
      last_view_arguments_ = UINT32_MAX;
      last_view_ = VK_NULL_HANDLE;
    }
    // 1D and 2D textures (and 3D-as-2D copies) as 2D arrays, as the
    // translated shaders sample them, or as 2D (the present pass).
    VkImageView GetView(bool is_signed, uint32_t host_swizzle, bool is_array = true);

    // The 2D copy of a 3D texture's first slice, created on first use and
    // loaded when outdated; nullptr if that fails.
    NativeTexture* Get3DAs2D();
    NativeTexture* texture_3d_as_2d() const { return texture_3d_as_2d_.get(); }

   private:
    VkImageView GetViewUncached(bool is_signed, uint32_t host_swizzle, bool is_array);

    VkImage image_;
    VmaAllocation allocation_;
    Usage usage_ = Usage::kUndefined;
    bool red_blue_swapped_ = false;
    // By 2D (not an array) << 13 | signed (separate view) << 12 | host swizzle.
    std::unordered_map<uint32_t, VkImageView> views_;
    // The last GetView's arguments (signed << 13 | array << 12 | host swizzle)
    // and view: the draws ask for the same one again and again (a scaled render
    // texture lookup for each 2D binding).
    uint32_t last_view_arguments_ = UINT32_MAX;
    VkImageView last_view_ = VK_NULL_HANDLE;
    std::unique_ptr<NativeTexture> texture_3d_as_2d_;
  };

  struct BindingViews {
    VkImageView view_unsigned = VK_NULL_HANDLE;
    VkImageView view_signed = VK_NULL_HANDLE;
  };

  const HostFormatPair& GetHostFormatPair(TextureKey key) const;
  void GetTextureImageFormats(TextureKey key, VkFormat (&formats)[2]) const;
  // The image of the key (a 2D one for a 3D key with image_2d: the 3D-as-2D
  // copy).
  std::unique_ptr<NativeTexture> CreateNativeTexture(TextureKey key, bool image_2d,
                                                     bool track_usage);
  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;
  // Barriers from the texture's usage to `usage` (always, two writes in a row
  // are ordered too). A texture sampled before any load wrote it (the load
  // failed, or it has no data) is initialized first.
  void TransitionTexture(CommandRecorder* command_buffer, NativeTexture& texture,
                         NativeTexture::Usage usage);
  // Whether a load or layout change of `texture` can be recorded before the
  // open draws' render pass (Submitter::BeginBeforeRenderPass, its hoist
  // point) instead of ending it: none of its draws sampled the texture and
  // they only read the shared memory (no memory export), so moving the
  // commands ahead of them changes nothing they see. Else outside the render
  // pass, ending it.
  bool CanLoadBeforeDraws(const NativeTexture& texture) const;
  // A new texture's first contents (its usage still kUndefined), so what no
  // load writes reads the same on every GPU (AMD and the Steam Deck hand back
  // old pictures, NVIDIA usually zeros): the levels outside [written_first,
  // written_last] (all when written_first > written_last) zero, or with
  // native_poison_new_images every level opaque magenta, the loads then
  // writing over theirs. Copied from the scratch buffer (compressed images
  // can't be cleared), so before a load fills it. Leaves it a transfer
  // destination; false (and kUndefined) if not done.
  bool InitializeTexture(CommandRecorder* command_buffer, NativeTexture& texture,
                         uint32_t written_first, uint32_t written_last);
  bool EnsureScratchBuffer(VkDeviceSize size);
  // The texture made for a resolve's destination (HasResolveDestinationTexture),
  // or nullptr.
  Texture* FindResolveDestinationTexture(const ResolveDestination& destination);
  // The views of the bindings using the texture again (its red and blue order
  // changed).
  void RebindTexture(const Texture& texture);
  VkDescriptorSet AllocateStorageBufferSet(VkBuffer buffer, VkDeviceSize offset,
                                           VkDeviceSize range);
  void FreeStorageBufferSetLater(VkDescriptorSet set);

  static const HostFormatPair kBestHostFormats[64];
  static const HostFormatPair kHostFormatGBGRUnaligned;
  static const HostFormatPair kHostFormatBGRGUnaligned;
  static const HostFormatPair kHostFormatDXT1Unaligned;
  static const HostFormatPair kHostFormatDXT2_3Unaligned;
  static const HostFormatPair kHostFormatDXT4_5Unaligned;
  static const HostFormatPair kHostFormatDXNUnaligned;
  static const HostFormatPair kHostFormatDXT5AUnaligned;

  NativeSharedMemory& native_shared_memory_;
  Submitter& submitter_;
  VmaAllocator vma_allocator_ = VK_NULL_HANDLE;
  HostFormatPair host_formats_[64];

  // Load shaders: set 0 the destination (scratch buffer), set 1 the source
  // (shared memory), both a compute storage buffer; LoadConstants pushed.
  VkDescriptorSetLayout load_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout load_pipeline_layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_{};
  // Storage buffer sets (freed individually).
  VkDescriptorPool load_set_pool_ = VK_NULL_HANDLE;
  // The whole shared memory, when maxStorageBufferRange allows it (the
  // offset is then pushed), else a set per load.
  VkDescriptorSet shared_memory_set_ = VK_NULL_HANDLE;
  // Untiled data before the copy into images, grown as needed.
  VkBuffer scratch_buffer_ = VK_NULL_HANDLE;
  VmaAllocation scratch_allocation_ = VK_NULL_HANDLE;
  VkDeviceSize scratch_size_ = 0;
  VkDescriptorSet scratch_set_ = VK_NULL_HANDLE;

  std::array<BindingViews, xenos::kTextureFetchConstantCount> binding_views_{};
  uint64_t binding_generation_ = 1;
  std::array<uint64_t, xenos::kTextureFetchConstantCount> binding_generations_{};

  uint32_t sampler_max_count_ = 0;
  xenos::AnisoFilter max_anisotropy_ = xenos::AnisoFilter::kDisabled;
  std::unordered_map<uint32_t, VkSampler> samplers_;
  uint32_t custom_border_color_sampler_count_ = 0;
  // GetSampler's last answer for each fetch constant, by its fetch constant
  // with the base and mip addresses reduced to whether they are zero (all
  // GetSamplerParameters reads of them): a slot sampling another texture of
  // the same kind keeps its sampler (it was looked up again each time).
  struct SamplerMemo {
    uint32_t fetch[6] = {};
    // The binding's filters and border color (SamplerMemoBinding in the .cpp).
    uint32_t binding = UINT32_MAX;
    int32_t anisotropic_override = 0;
    VkSampler sampler = VK_NULL_HANDLE;
  };
  std::array<SamplerMemo, xenos::kTextureFetchConstantCount> sampler_memos_{};

  // The texture size each resolved surface was last found with
  // (FindResolveDestinationTexture), as the SDK's VulkanTextureCache: trying
  // all 64 sizes for every resolve of every frame would cost more than the
  // copy saves; a resolve with no texture at any of them is tried again only
  // once another texture has been created.
  struct ResolvedSurfaceSize {
    // What FindResolveDestinationTexture identifies it by; 0: unused.
    uint64_t resolve = 0;
    // When none was found: textures_created() then.
    uint64_t textures_created = 0;
    // The texture's size; 0: none was found.
    uint32_t width = 0;
    uint32_t height = 0;
  };
  std::array<ResolvedSurfaceSize, 64> resolved_surface_sizes_{};

 public:
  struct ResidentLoadStats {
    uint64_t attempts = 0, recorded = 0, initial = 0, reload = 0, same_submission = 0;
    uint64_t base = 0, mips = 0, guest_bytes = 0;
  };
  ResidentLoadStats TakeResidentLoadStats() {
    auto stats = resident_load_stats_;
    resident_load_stats_ = {};
    return stats;
  }
  bool profile_transfers() const { return profile_transfers_; }
  // Emits at most 8 guest keys, ranked within the first 128 keys admitted
  // this interval. Overflow is reported explicitly, never silently sampled.
  void LogResidentLoadDetails(bool emit);

 private:
  bool profile_transfers_ = false;
  ResidentLoadStats resident_load_stats_;
  struct ResidentDetail {
    uint64_t recorded = 0, bytes = 0, repeat_parts = 0, new_parts_only = 0;
    uint64_t cpu = 0, gpu = 0, both = 0, no_watch = 0;
    uint64_t base_cpu = 0, base_gpu = 0, mips_cpu = 0, mips_gpu = 0;
  };
  ResidentDetail resident_detail_total_;
  std::unordered_map<TextureKey, ResidentDetail, TextureKey::Hasher> resident_details_;
  uint64_t resident_detail_overflow_ = 0;

  uint64_t submission_begun_ = 0;
  uint64_t completed_reported_ = 0;
  uint32_t logged_ = 0;
};

}  // namespace rex::graphics::native
