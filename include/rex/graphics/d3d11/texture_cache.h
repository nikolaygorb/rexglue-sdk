/**
 * @file        graphics/d3d11/texture_cache.h
 * @brief       Texture cache of the experimental Direct3D 11 backend
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *              Formats and loading follow the D3D12 texture cache; the upload
 *              through a typeless texture's UINT view follows
 *              TheSimpsonsGameRecomp (tools/rexglue-sdk/src/graphics/d3d11/
 *              texture_upload.cpp, commit 4ff637bc, BSD 3-Clause).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>

#include <rex/assert.h>
#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/d3d11/texture_upload.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

// Textures are created with typeless formats and loaded in two compute passes:
// the shared texture load shaders untile and convert the guest data into a
// buffer, then d3d11_texture_upload_* stores the buffer's texels through a
// UINT unordered access view of the texture (Direct3D 11 can't copy a buffer
// to a texture). A UAV can't write block-compressed formats, so as in D3D12,
// DXT and DXN textures whose size is a multiple of the block size (2D, cube,
// unscaled) are BC textures whose blocks are uploaded into a UINT texture with
// a texel per block and copied from there (d3d11_block_compressed_textures);
// other block-compressed and subsampled textures are decompressed. 16-bit
// packed formats are widened to R8G8B8A8, which has a typed UAV at feature
// level 11.0. With a draw
// resolution scale above 1x1, resolution-scaled resolves go to the scaled
// resolve memory (see EnsureScaledResolveMemoryCommitted), and their textures
// are created and loaded at the scaled size from there. All context access is
// under the context mutex, by the command processor's thread.
class D3D11TextureCache final : public TextureCache {
 public:
  // As in D3D12TextureCache.
  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;         // 3
      xenos::ClampMode clamp_y : 3;         // 6
      xenos::ClampMode clamp_z : 3;         // 9
      xenos::BorderColor border_color : 2;  // 11
      // For anisotropic, these are true.
      uint32_t mag_linear : 1;              // 12
      uint32_t min_linear : 1;              // 13
      uint32_t mip_linear : 1;              // 14
      xenos::AnisoFilter aniso_filter : 3;  // 17
      uint32_t mip_min_level : 4;           // 21
      uint32_t mip_base_map : 1;            // 22
      // Force the border color alpha to 1.0 (only meaningful with a border
      // clamp mode).
      uint32_t force_bc_w_to_max : 1;  // 23
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    bool operator==(const SamplerParameters& parameters) const { return value == parameters.value; }
    bool operator!=(const SamplerParameters& parameters) const { return value != parameters.value; }
  };

  D3D11TextureCache(const RegisterFile& register_file, D3D11SharedMemory& shared_memory,
                    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
                    D3D11CommandProcessor& command_processor);
  ~D3D11TextureCache() override;

  bool Initialize();

  // Scaled resolve memory. Direct3D 11 has no guaranteed tiled resources, so
  // unlike D3D12's sparse buffers over the whole scaled address space
  // (SharedMemory::kBufferSize times the scale area), it's a set of
  // non-overlapping buffers created for the ranges used. A range must be in
  // one buffer to be bound, so a range overlapping several buffers (or
  // reaching past one) gets a new buffer covering all of them, with their
  // contents copied in. Buffers are at most kMaxScaledResolveBufferSize, the
  // limit of a raw view.
  bool EnsureScaledResolveMemoryCommitted(uint32_t start_unscaled, uint32_t length_unscaled,
                                          uint32_t length_scaled_alignment_log2 = 0) override;
  // Commits the range and makes the buffer holding it current. False on
  // failure, and then no buffer is current.
  bool MakeScaledResolveRangeCurrent(uint32_t start_unscaled, uint32_t length_unscaled,
                                     uint32_t length_scaled_alignment_log2 = 0);
  // Of the range of the last successful MakeScaledResolveRangeCurrent: a raw
  // view of the whole buffer and the range's byte offset in it, and a raw
  // unordered access view starting at the range (for the resolve copy
  // shaders, which write relative to the bound buffer). The views are valid
  // until the next MakeScaledResolveRangeCurrent.
  ID3D11ShaderResourceView* GetCurrentScaledResolveBufferSRV() const {
    assert_not_null(scaled_resolve_current_buffer_);
    return scaled_resolve_current_buffer_->srv.Get();
  }
  uint32_t GetCurrentScaledResolveRangeOffset() const { return scaled_resolve_current_offset_; }
  ID3D11UnorderedAccessView* GetCurrentScaledResolveRangeUAV();

  // The view for a texture binding of a translated shader, of the textures
  // of the latest RequestTextures, and the host swizzle to apply to its
  // fetches in the shader. Null (with a zero swizzle, as the null descriptor
  // of D3D12 reads) when the fetch constant has no usable texture.
  ID3D11ShaderResourceView* GetActiveTextureSRV(const DxbcShader::TextureBinding& binding,
                                                uint32_t& host_swizzle_out);

  // For d3d11_log_frames: why GetActiveTextureSRV returned null, and loads.
  struct Stats {
    uint32_t loads = 0;
    uint32_t load_failures = 0;
    uint32_t no_fetch_constant = 0;
    uint32_t dimension_mismatch = 0;
    uint32_t sign_unused = 0;
    uint32_t no_texture = 0;
    uint32_t no_view = 0;
  };
  Stats& stats() { return stats_; }
  // Logs each distinct fetch that has no texture once while enabled.
  void SetLogMissingTextures(bool log) { log_missing_textures_ = log; }

  // The front buffer of a swap (texture fetch constant 0), loaded, as
  // D3D12TextureCache::RequestSwapTexture: a 2D view of it, the host swizzle
  // to apply when reading it (views have no component mapping), its guest
  // format, its guest size and whether it's resolution-scaled (its host size
  // is the guest size times the draw resolution scale then). Null if it's not
  // a single 2D texture or failed.
  ID3D11ShaderResourceView* RequestSwapTexture(uint32_t& host_swizzle_out,
                                               xenos::TextureFormat& format_out,
                                               uint32_t& width_out, uint32_t& height_out,
                                               bool& scaled_out);

  // As VulkanTextureCache::CopyResolveIntoTexture (d3d11_resolve_into_textures):
  // finds the texture the guest would fetch the resolved surface as (2D, one
  // level, tiled) of the resolve's size or up to 7 texels narrower or shorter
  // (a resolve covers whole 8x8 blocks), copies the texture's part of the
  // render target at (source_x, source_y) into it and marks it up to date, so
  // it isn't loaded again from the resolved memory. red_blue_swap: the
  // resolve swapped red and blue in memory (the texture's host swizzle swaps
  // them back). Returns whether it was copied.
  bool CopyResolveIntoTexture(uint32_t dest_base, uint32_t dest_pitch_texels, uint32_t width,
                              uint32_t height, xenos::TextureFormat format,
                              xenos::Endian endianness, bool red_blue_swap,
                              ID3D11Resource* source, uint32_t source_x, uint32_t source_y);

  // Also creates or reloads the 2D copies of the used 3D textures for 2D
  // fetches (gpu_3d_to_2d_texture), as D3D12TextureCache::RequestTextures.
  void RequestTextures(uint32_t used_texture_mask) override;

  // A generated copy of the D3D12 function (texture_cache_copies.cpp).
  SamplerParameters GetSamplerParameters(const DxbcShader::SamplerBinding& binding) const;
  // Follows D3D12TextureCache::WriteSampler. Null on failure.
  ID3D11SamplerState* GetOrCreateSampler(SamplerParameters parameters);

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  // Replacement images are uploaded as they are, also as BC blocks for the
  // DXT and DXN formats of any size (only the upload of guest data needs a
  // UAV, or a size in whole blocks): the texture is BC then, with the same
  // swizzle as the decompressed host format.
  bool CanCreateReplacementTexture(TextureKey key) const override;
  std::unique_ptr<Texture> CreateReplacementTexture(
      TextureKey key, const texture_replacement::Image& image) override;
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

 private:
  struct HostFormat {
    // Typeless (or the only) format of the resource, UNKNOWN if unsupported.
    DXGI_FORMAT resource;
    DXGI_FORMAT unsigned_view;
    DXGI_FORMAT signed_view;
    // UINT format of the unordered access view the upload writes.
    DXGI_FORMAT upload_view;
    LoadShaderIndex load_shader;
    // If not unknown and different from load_shader, the signed version is a
    // separate texture loaded with this shader.
    LoadShaderIndex load_shader_signed;
    // For 16-bit packed sources widened to R8G8B8A8; the layout follows from
    // upload_view otherwise.
    const TextureUploadLayout* packed_upload;
    // Mapping of Xenos swizzle components to the host format's components.
    uint32_t swizzle;
  };

  class D3D11Texture final : public Texture {
   public:
    D3D11Texture(D3D11TextureCache& texture_cache, const TextureKey& key,
                 Microsoft::WRL::ComPtr<ID3D11Resource> resource, bool track_usage = true);
    ~D3D11Texture() override;
    ID3D11Resource* resource() const { return resource_.Get(); }
    // False for the 2D copy of a 3D texture, whose key is still 3D.
    bool is_host_3d() const { return is_host_3d_; }
    // As D3D12Texture::GetOrCreate3DAs2DResource: a single-layer 2D texture
    // of the first slice of the base level of this 3D texture, loaded with 3D
    // tiling, reloaded when outdated. Null if disabled or failed.
    D3D11Texture* GetOrCreate3DAs2D();
    D3D11Texture* texture_3d_as_2d() const { return texture_3d_as_2d_.get(); }
    // Guest blocks stored as BC blocks (IsHostBlockCompressed when created).
    bool host_block_compressed() const { return host_block_compressed_; }
    // Holds red and blue the other way than the guest memory's texels when
    // loaded (CopyResolveIntoTexture of a resolve that swapped them); its
    // host swizzle swaps them back. Cleared by a load.
    bool red_blue_swapped() const { return red_blue_swapped_; }
    void set_red_blue_swapped(bool swapped) { red_blue_swapped_ = swapped; }
    // For a replacement texture: the format of its unsigned views if not the
    // host format's (BC), and its memory (the image's, not the key's).
    void SetReplacementHostData(DXGI_FORMAT unsigned_view_format, uint64_t host_memory_usage) {
      unsigned_view_format_override_ = unsigned_view_format;
      SetHostMemoryUsage(host_memory_usage);
    }
    // Views by FetchOpDimension and signedness, created on first use.
    ID3D11ShaderResourceView* GetOrCreateSRV(xenos::FetchOpDimension dimension, bool is_signed);
    // UINT view of a mip level for the upload.
    ID3D11UnorderedAccessView* GetOrCreateUploadUAV(uint32_t level);
    // Unsigned 2D view of the base level of a single-layer 2D texture.
    ID3D11ShaderResourceView* GetOrCreateSwapSRV();

   private:
    Microsoft::WRL::ComPtr<ID3D11Resource> resource_;
    bool is_host_3d_;
    bool host_block_compressed_;
    bool red_blue_swapped_ = false;
    // The format of the unsigned views if not the host format's: the BC format
    // of a block-compressed or replacement texture, which has no signed views.
    DXGI_FORMAT unsigned_view_format_override_ = DXGI_FORMAT_UNKNOWN;
    std::unique_ptr<D3D11Texture> texture_3d_as_2d_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 8> srvs_;
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, xenos::kTextureMaxMips> uavs_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> swap_srv_;
  };

  static const HostFormat host_formats_[64];

  static TextureUploadLayout GetUploadLayout(const HostFormat& host_format) {
    return host_format.packed_upload ? *host_format.packed_upload
                                     : GetTextureUploadLayout(host_format.upload_view);
  }
  // The BC format of a guest block-compressed format stored as it is, the
  // UINT format with a texel per block its blocks are uploaded with, and the
  // load shader that copies the blocks. UNKNOWN for the formats always
  // decompressed (CTX1 and DXT3A, as in D3D12).
  struct BlockCompressedFormat {
    DXGI_FORMAT format;
    DXGI_FORMAT block_view;
    LoadShaderIndex load_shader;
  };
  static BlockCompressedFormat GetBlockCompressedFormat(xenos::TextureFormat format);
  // Whether the guest blocks of the texture are stored as they are in a BC
  // texture (see the class comment).
  bool IsHostBlockCompressed(const TextureKey& key) const;
  LoadShaderIndex GetLoadShaderIndex(TextureKey key) const;
  // A Texture3D if host_3d, a Texture2D (array or cube) otherwise, with the
  // size and levels of the key. Null on failure.
  Microsoft::WRL::ComPtr<ID3D11Resource> CreateHostResource(const TextureKey& key, bool host_3d);
  // A generated copy of the D3D12 function (texture_cache_copies.cpp).
  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;
  ID3D11Device* GetDevice() const;
  ID3D11DeviceContext* GetContext() const;
  // Grows the decode buffer to at least the size; false on failure.
  bool EnsureDecodeBuffer(uint32_t size);
  // The UINT view of the single-layer texture of 8-byte or 16-byte blocks the
  // blocks of a BC level are uploaded into, grown to at least the size in
  // blocks, and the texture. Null on failure.
  ID3D11UnorderedAccessView* GetBlockUploadTexture(uint32_t block_bytes, uint32_t width_blocks,
                                                   uint32_t height_blocks,
                                                   ID3D11Texture2D*& texture_out);
  bool UpdateConstants(ID3D11Buffer* buffer, const void* data, size_t size);

  D3D11CommandProcessor& command_processor_;
  D3D11SharedMemory& d3d11_shared_memory_;

  // Guest formats whose host views can be sampled with filtering, as bits by
  // xenos::TextureFormat (GetSamplerParameters falls back to point sampling).
  uint64_t host_filterable_unsigned_ = 0;
  uint64_t host_filterable_signed_ = 0;

  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11SamplerState>> samplers_;

  Stats stats_;
  bool log_missing_textures_ = false;
  // Fetches without a texture already logged (fetch constant dwords 0 to 5
  // hashed with the reason).
  std::unordered_map<uint64_t, bool> missing_textures_logged_;
  void CountMissingTexture(const DxbcShader::TextureBinding& binding, uint32_t& counter,
                           const char* reason);

  std::array<Microsoft::WRL::ComPtr<ID3D11ComputeShader>, kLoadShaderCount> load_shaders_;
  // For resolution-scaled resolve targets, empty without resolution scaling.
  std::array<Microsoft::WRL::ComPtr<ID3D11ComputeShader>, kLoadShaderCount> load_shaders_scaled_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> upload_2d_array_shader_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> upload_3d_shader_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> load_constants_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> upload_constants_;

  // d3d11_block_compressed_textures, read at Initialize.
  bool block_compressed_textures_ = false;
  // For 8-byte and 16-byte blocks (GetBlockUploadTexture).
  struct BlockUploadTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    uint32_t width_blocks = 0;
    uint32_t height_blocks = 0;
  };
  std::array<BlockUploadTexture, 2> block_upload_textures_;

  // Decoded data between the two passes (raw views).
  Microsoft::WRL::ComPtr<ID3D11Buffer> decode_buffer_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> decode_buffer_srv_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> decode_buffer_uav_;
  uint32_t decode_buffer_size_ = 0;

  // Scaled resolve memory: buffers by their start in the scaled address
  // space, not overlapping. A raw view addresses at most 2^27 dwords.
  static constexpr uint64_t kMaxScaledResolveBufferSize =
      uint64_t(4) << D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP;
  // New buffers are aligned to this in the scaled address space, for fewer
  // merges of neighbouring ranges.
  static constexpr uint32_t kScaledResolveBufferAlignmentLog2 = 20;
  struct ScaledResolveBuffer {
    uint64_t length_scaled;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    // Views starting at a range (GetCurrentScaledResolveRangeUAV), by the
    // range's offset in the buffer.
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> range_uavs;
  };
  std::map<uint64_t, ScaledResolveBuffer> scaled_resolve_buffers_;

  // As VulkanTextureCache's: the texture size each resolved surface was last
  // found with (CopyResolveIntoTexture), so not all the sizes up to 7 texels
  // smaller are tried for every resolve; a resolve with no texture at any of
  // them is tried again only once another texture has been created.
  struct ResolvedSurfaceSize {
    uint64_t resolve = 0;           // what CopyResolveIntoTexture identifies it by; 0: unused
    uint64_t textures_created = 0;  // when none was found: textures_created() then
    uint32_t width = 0;             // the texture's size; 0: none was found
    uint32_t height = 0;
  };
  std::array<ResolvedSurfaceSize, 64> resolved_surface_sizes_{};
  uint64_t scaled_resolve_buffers_total_size_ = 0;
  bool scaled_resolve_buffer_failure_logged_ = false;
  ScaledResolveBuffer* scaled_resolve_current_buffer_ = nullptr;
  uint32_t scaled_resolve_current_offset_ = 0;
};

}  // namespace rex::graphics::d3d11
