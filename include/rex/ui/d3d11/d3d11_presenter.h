/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Ported for the ReXGlue fork: Direct3D 11 version of the
 *              D3D12 presenter.
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <rex/ui/d3d11/d3d11_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/surface.h>

namespace rex::ui::d3d11 {

// UI drawers draw with the provider's immediate context; the painting thread
// holds the context mutex during the whole drawing.
class D3D11UIDrawContext final : public UIDrawContext {
 public:
  D3D11UIDrawContext(Presenter& presenter, uint32_t render_target_width,
                     uint32_t render_target_height)
      : UIDrawContext(presenter, render_target_width, render_target_height) {}
};

class D3D11Presenter final : public Presenter {
 public:
  static constexpr DXGI_FORMAT kGuestOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr DXGI_FORMAT kGuestOutputIntermediateFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  // The format used internally by Windows composition.
  static constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

  // The refresher writes the guest output texture (kGuestOutputFormat, usable
  // as an SRV, a UAV and a render target) with the provider's immediate
  // context, under the context mutex.
  class D3D11GuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    D3D11GuestOutputRefreshContext(bool& is_8bpc_out_ref, ID3D11Texture2D* texture,
                                   ID3D11UnorderedAccessView* uav, ID3D11RenderTargetView* rtv)
        : GuestOutputRefreshContext(is_8bpc_out_ref), texture_(texture), uav_(uav), rtv_(rtv) {}

    ID3D11Texture2D* texture() const { return texture_; }
    // Null if the device can't store kGuestOutputFormat through a typed UAV.
    ID3D11UnorderedAccessView* uav() const { return uav_; }
    ID3D11RenderTargetView* rtv() const { return rtv_; }

   private:
    ID3D11Texture2D* texture_;
    ID3D11UnorderedAccessView* uav_;
    ID3D11RenderTargetView* rtv_;
  };

  static std::unique_ptr<D3D11Presenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                const D3D11Provider& provider) {
    auto presenter =
        std::unique_ptr<D3D11Presenter>(new D3D11Presenter(host_gpu_loss_callback, provider));
    if (!presenter->InitializeSurfaceIndependent()) {
      return nullptr;
    }
    return presenter;
  }

  ~D3D11Presenter();

  const D3D11Provider& provider() const { return provider_; }

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  bool CaptureGuestOutput(RawImage& image_out) override;

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;

  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out) override;

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct GuestOutputPaintRectangleConstants {
    float x;
    float y;
    float width;
    float height;
  };

  union EffectConstants {
    BilinearConstants bilinear;
    CasSharpenConstants cas_sharpen;
    CasResampleConstants cas_resample;
    FsrEasuConstants fsr_easu;
    FsrRcasConstants fsr_rcas;
  };

  struct GuestOutputTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  struct IntermediateTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  explicit D3D11Presenter(HostGpuLossCallback host_gpu_loss_callback,
                          const D3D11Provider& provider)
      : Presenter(host_gpu_loss_callback), provider_(provider) {}

  bool InitializeSurfaceIndependent();

  void DestroySwapChain();

  const D3D11Provider& provider_;

  // Whether DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING is supported by DXGI, primarily
  // for variable refresh rate support.
  bool dxgi_supports_tearing_ = false;
  // Whether kGuestOutputFormat can be a typed UAV on this device.
  bool guest_output_uav_supported_ = false;

  // Static objects for guest output painting.
  Microsoft::WRL::ComPtr<ID3D11VertexShader> guest_output_paint_vs_;
  std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, size_t(GuestOutputPaintEffect::kCount)>
      guest_output_paint_ps_;
  // VS b0.
  Microsoft::WRL::ComPtr<ID3D11Buffer> guest_output_paint_rectangle_constants_;
  // PS b0, the largest of the effect constant structures.
  Microsoft::WRL::ComPtr<ID3D11Buffer> guest_output_paint_effect_constants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> guest_output_paint_sampler_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> guest_output_paint_rasterizer_state_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> guest_output_paint_blend_state_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> guest_output_paint_depth_stencil_state_;

  // Indexed by the mailbox index. Direct3D 11 keeps a resource alive until the
  // GPU is done with it, and the immediate context executes in order, so no
  // completion tracking is needed for replacing them.
  std::array<GuestOutputTexture, kGuestOutputMailboxSize> guest_output_textures_;

  // Painting state, accessible only by painting and by surface connection
  // lifetime management.
  std::array<IntermediateTexture, kMaxGuestOutputPaintEffects - 1> intermediate_textures_;
  uint32_t swap_chain_width_ = 0;
  uint32_t swap_chain_height_ = 0;
  bool swap_chain_allows_tearing_ = false;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
  // With the flip model in Direct3D 11, buffer 0 is always the current back
  // buffer.
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> back_buffer_rtv_;
};

}  // namespace rex::ui::d3d11
