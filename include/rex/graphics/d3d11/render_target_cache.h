/**
 * @file        graphics/d3d11/render_target_cache.h
 * @brief       Host render target cache of the experimental Direct3D 11 backend
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/d3d11/render_target_shaders.h>
#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/util/draw.h>
#include <rex/memory.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;
class D3D11TextureCache;

// Host render targets only (the RTV path; ROV needs optional Direct3D 11.3
// features absent on the target hardware): render targets are created, bound,
// cleared by resolves, and EDRAM ownership is transferred between them with the
// shaders of the D3D12 backend (RenderTargetShaders). Resolves dump the render
// targets to the EDRAM buffer and copy from it to the shared memory or, with
// resolution scaling, to the texture cache's scaled resolve memory.
class D3D11RenderTargetCache final : public RenderTargetCache {
 public:
  D3D11RenderTargetCache(const RegisterFile& register_file, const memory::Memory& memory,
                         uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
                         D3D11CommandProcessor& command_processor)
      : RenderTargetCache(register_file, memory, draw_resolution_scale_x,
                          draw_resolution_scale_y),
        command_processor_(command_processor) {}
  ~D3D11RenderTargetCache() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  Path GetPath() const override { return Path::kHostRenderTargets; }

  void ClearCache() override;
  void BeginFrame() override;

  // Call under the context mutex.
  bool Update(bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask, const Shader& vertex_shader,
              int32_t window_offset_tiles) override;

  // Performs the resolve in the current registers: the copy to the shared
  // memory or the scaled resolve memory (written_scaled_out), with the
  // textures of the range invalidated (written_length_out 0 if nothing was
  // copied), and the clears. copy_dest_info_out is the destination as the
  // written extent was calculated for. Call under the context mutex.
  bool Resolve(const memory::Memory& memory, D3D11SharedMemory& shared_memory,
               D3D11TextureCache& texture_cache, uint32_t& written_address_out,
               uint32_t& written_length_out, reg::RB_COPY_DEST_INFO& copy_dest_info_out,
               bool& written_scaled_out);

  // After the output merger bindings were changed outside the cache.
  void InvalidateBoundRenderTargets() { are_bound_render_targets_valid_ = false; }

  bool msaa_2x_supported() const { return msaa_2x_supported_; }
  bool gamma_render_target_as_unorm16() const { return gamma_render_target_as_unorm16_; }
  // Using R16G16[B16A16]_SNORM, which are -1...1, not the needed -32...32.
  bool IsFixed16TruncatedToMinus1To1() const {
    return !REXCVAR_GET(snorm16_render_target_full_range);
  }
  bool depth_float24_round() const { return depth_float24_round_; }
  bool depth_float24_convert_in_pixel_shader() const {
    return depth_float24_convert_in_pixel_shader_;
  }

  const RenderTargetShaders& shaders() const { return *shaders_; }
  DXGI_FORMAT GetColorDrawDXGIFormat(xenos::ColorRenderTargetFormat format) const {
    return shaders_->GetColorDrawDXGIFormat(format);
  }

  // Stage 4 stand-in for resolves and the front buffer: the largest color
  // render target bound to slot 0 in the current frame, the latest one if
  // several have the same size. Null if none.
  ID3D11Texture2D* GetTemporaryGuestOutput(DXGI_FORMAT& format_out, uint32_t& width_out,
                                           uint32_t& height_out) const;

 protected:
  uint32_t GetMaxRenderTargetWidth() const override { return D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION; }
  uint32_t GetMaxRenderTargetHeight() const override {
    return D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  }
  // OMSetRenderTargets drops views of different sizes.
  bool AreMixedHeightRenderTargetsSupported() const override { return false; }

  RenderTarget* CreateRenderTarget(RenderTargetKey key, uint32_t height) override;
  RenderTarget* GrowRenderTarget(RenderTarget* source, uint32_t height,
                                 const std::vector<Transfer>& transfers) override;

  bool CanReuseRenderTarget(const RenderTarget& render_target) const override {
    return &render_target != temporary_guest_output_;
  }
  void RenderTargetReused(RenderTarget& render_target) override;

  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override;

  bool IsGammaFormatHostStorageSeparate() const override {
    return gamma_render_target_as_unorm16_;
  }

 private:
  class D3D11RenderTarget final : public RenderTarget {
   public:
    // rtv_load_separate is present when the formats are different for drawing
    // and bit-exact loading (see D3D12RenderTarget).
    D3D11RenderTarget(RenderTargetKey key, uint32_t height,
                      Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
                      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv,
                      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_load_separate,
                      Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv,
                      Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
                      Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_stencil)
        : RenderTarget(key, height),
          texture_(std::move(texture)),
          rtv_(std::move(rtv)),
          rtv_load_separate_(std::move(rtv_load_separate)),
          dsv_(std::move(dsv)),
          srv_(std::move(srv)),
          srv_stencil_(std::move(srv_stencil)) {}

    ID3D11Texture2D* texture() const { return texture_.Get(); }
    ID3D11RenderTargetView* rtv() const { return rtv_.Get(); }
    ID3D11RenderTargetView* rtv_load() const {
      return rtv_load_separate_ ? rtv_load_separate_.Get() : rtv_.Get();
    }
    ID3D11DepthStencilView* dsv() const { return dsv_.Get(); }
    // Color in the ownership transfer format, or depth.
    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    ID3D11ShaderResourceView* srv_stencil() const { return srv_stencil_.Get(); }

   private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_load_separate_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_stencil_;
  };

  // Like D3D12RenderTargetCache::PerformTransfersAndResolveClears without the
  // clears.
  bool PerformTransfers(uint32_t render_target_count, RenderTarget* const* render_targets,
                        const std::vector<Transfer>* render_target_transfers,
                        const Transfer::Rectangle* resolve_clear_rectangle);
  ID3D11PixelShader* GetOrCreateTransferPixelShader(RenderTargetShaders::TransferShaderKey key);
  // Writes 6 vertices per rectangle into the transfer vertex buffer and binds
  // it, returns the vertex count, 0 on failure.
  uint32_t UploadTransferRectangles(const Transfer::Rectangle* rectangles, uint32_t count,
                                    RenderTargetKey dest_key);
  // A copy of a depth render target, the host depth source of transfers into
  // the render target itself.
  ID3D11ShaderResourceView* CopyHostDepth(const D3D11RenderTarget& render_target);
  // Dumps the render targets owning the range to the EDRAM buffer, like
  // D3D12RenderTargetCache::DumpRenderTargets: in the scaled layout, or the
  // 1x1 one (native_layout) for the unscaled copy of only native sources.
  bool DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
                         uint32_t dump_pitch, bool native_layout);
  // As VulkanRenderTargetCache::CopyResolveIntoTexture (d3d11_resolve_into_textures):
  // after an unscaled resolve of a 32bpp k_8_8_8_8 surface owned by one
  // render target (the dump just made of it), copies it from the render
  // target into its texture if that exists.
  void CopyResolveIntoTexture(const draw_util::ResolveInfo& resolve_info,
                              draw_util::ResolveCopyShaderIndex copy_shader,
                              D3D11TextureCache& texture_cache);
  ID3D11ComputeShader* GetOrCreateDumpShader(RenderTargetShaders::DumpPipelineKey key);
  void BindRenderTargets(RenderTarget* const* depth_and_color_render_targets);
  void ClearRenderTarget(D3D11RenderTarget& render_target, uint64_t clear_value,
                         const D3D11_RECT& rect);
  // A rectangle of a depth buffer or of a 32-bit float color render target
  // cleared by drawing.
  void ClearByDrawing(D3D11RenderTarget& render_target, uint64_t clear_value,
                      const D3D11_RECT& rect);

  D3D11CommandProcessor& command_processor_;

  bool msaa_2x_supported_ = false;
  bool gamma_render_target_as_unorm16_ = false;
  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;
  bool use_stencil_reference_output_ = false;

  std::unique_ptr<RenderTargetShaders> shaders_;

  // Ownership transfers.
  Microsoft::WRL::ComPtr<ID3D11VertexShader> transfer_vs_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> transfer_input_layout_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> transfer_rasterizer_state_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> transfer_depth_state_;
  // Stencil set to 0 in the rectangles, then one state per stencil bit.
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> transfer_stencil_clear_state_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> transfer_stencil_bit_states_[8];
  // Indexed by TransferCBVRegister; one dword each, padded to 16 bytes.
  Microsoft::WRL::ComPtr<ID3D11Buffer> transfer_constants_[3];
  Microsoft::WRL::ComPtr<ID3D11Buffer> transfer_vertex_buffer_;
  uint32_t transfer_vertex_buffer_size_ = 0;
  uint32_t transfer_vertex_buffer_position_ = 0;
  // Null for keys whose shader failed.
  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11PixelShader>> transfer_pixel_shaders_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> host_depth_copy_;

  // Resolves: render targets are dumped to the EDRAM buffer, from which the
  // resolve copy shaders write to the shared memory.
  Microsoft::WRL::ComPtr<ID3D11Buffer> edram_buffer_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> edram_buffer_raw_srv_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> edram_buffer_raw_uav_;
  // Indexed by DumpCbuffer.
  Microsoft::WRL::ComPtr<ID3D11Buffer> dump_constants_[RenderTargetShaders::kDumpCbufferCount];
  // Null for keys whose shader failed.
  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11ComputeShader>> dump_shaders_;
  std::vector<ResolveCopyDumpRectangle> dump_rectangles_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader>
      resolve_copy_shaders_[size_t(draw_util::ResolveCopyShaderIndex::kCount)];
  // Only with resolution scaling; null for the copies it doesn't support.
  Microsoft::WRL::ComPtr<ID3D11ComputeShader>
      resolve_copy_scaled_shaders_[size_t(draw_util::ResolveCopyShaderIndex::kCount)];
  Microsoft::WRL::ComPtr<ID3D11Buffer> resolve_copy_constants_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> host_depth_copy_srv_;

  // Clears by drawing.
  Microsoft::WRL::ComPtr<ID3D11VertexShader> clear_vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> clear_uint2_ps_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> clear_uint2_constants_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> clear_depth_stencil_state_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> clear_color_depth_stencil_state_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> clear_rasterizer_state_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> clear_blend_state_;

  RenderTarget* bound_render_targets_[1 + xenos::kMaxColorRenderTargets] = {};
  bool are_bound_render_targets_valid_ = false;

  std::vector<Transfer> clear_transfers_[2];

  // Replaced by grown copies, destroyed at the next frame.
  std::vector<std::unique_ptr<RenderTarget>> retired_render_targets_;

  const D3D11RenderTarget* temporary_guest_output_ = nullptr;

};

}  // namespace rex::graphics::d3d11
