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

#include <rex/ui/d3d11/d3d11_presenter.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/flags.h>
#include <rex/ui/surface_win.h>

REXCVAR_DEFINE_BOOL(d3d11_allow_variable_refresh_rate_and_tearing, true, "UI/D3D11",
                    "Allow variable refresh rate and tearing");

namespace rex::ui::d3d11 {

// Generated with tools/shaders/build_shaders.py (Shader Model 5.0 set).
namespace shaders {
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_bilinear_dither_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_bilinear_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_cas_resample_dither_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_cas_resample_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_cas_sharpen_dither_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_cas_sharpen_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_fsr_easu_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_fsr_rcas_dither_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_ffx_fsr_rcas_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_triangle_strip_rect_vs.h"
}  // namespace shaders

namespace {

template <typename T>
void UpdateDynamicBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const T& data) {
  D3D11_MAPPED_SUBRESOURCE mapping;
  if (SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapping))) {
    std::memcpy(mapping.pData, &data, sizeof(data));
    context->Unmap(buffer, 0);
  }
}

}  // namespace

D3D11Presenter::~D3D11Presenter() { DestroySwapChain(); }

Surface::TypeFlags D3D11Presenter::GetSupportedSurfaceTypes() const {
  Surface::TypeFlags types = 0;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
  types |= Surface::kTypeFlag_Win32Hwnd;
#endif
  return types;
}

bool D3D11Presenter::CaptureGuestOutput(RawImage& image_out) {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> guest_output_texture;
  {
    uint32_t guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(
        ConsumeGuestOutput(guest_output_mailbox_index, nullptr, nullptr));
    if (guest_output_mailbox_index != UINT32_MAX) {
      guest_output_texture = guest_output_textures_[guest_output_mailbox_index].texture;
    }
  }
  if (!guest_output_texture) {
    return false;
  }

  D3D11_TEXTURE2D_DESC desc;
  guest_output_texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  if (FAILED(provider_.GetDevice()->CreateTexture2D(&desc, nullptr, &staging))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output capture texture");
    return false;
  }

  std::lock_guard<ContextMutex> context_lock(provider_.context_mutex());
  ID3D11DeviceContext* context = provider_.GetImmediateContext();
  context->CopyResource(staging.Get(), guest_output_texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapping;
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping))) {
    REXLOG_ERROR("D3D11Presenter: Failed to map the guest output capture texture");
    return false;
  }
  image_out.width = desc.Width;
  image_out.height = desc.Height;
  image_out.stride = sizeof(uint32_t) * image_out.width;
  image_out.data.resize(image_out.stride * image_out.height);
  uint32_t* image_out_pixels = reinterpret_cast<uint32_t*>(image_out.data.data());
  for (uint32_t y = 0; y < image_out.height; ++y) {
    uint32_t* dest_row = &image_out_pixels[size_t(image_out.width) * y];
    const uint32_t* source_row = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(mapping.pData) + size_t(mapping.RowPitch) * y);
    for (uint32_t x = 0; x < image_out.width; ++x) {
      dest_row[x] = Packed10bpcRGBTo8bpcBytes(source_row[x]);
    }
  }
  context->Unmap(staging.Get(), 0);
  return true;
}

Presenter::SurfacePaintConnectResult
D3D11Presenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(Surface& new_surface,
                                                                uint32_t new_surface_width,
                                                                uint32_t new_surface_height,
                                                                bool was_paintable,
                                                                bool& is_vsync_implicit_out) {
  uint32_t new_swap_chain_width =
      std::min(new_surface_width, uint32_t(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION));
  uint32_t new_swap_chain_height =
      std::min(new_surface_height, uint32_t(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION));

  ID3D11Device* device = provider_.GetDevice();

  if (swap_chain_) {
    if (was_paintable && swap_chain_width_ == new_swap_chain_width &&
        swap_chain_height_ == new_swap_chain_height) {
      is_vsync_implicit_out = false;
      return SurfacePaintConnectResult::kSuccessUnchanged;
    }
    // Every reference to the buffers must be released before resizing,
    // including the binding in the context.
    bool swap_chain_resized;
    {
      std::lock_guard<ContextMutex> context_lock(provider_.context_mutex());
      ID3D11DeviceContext* context = provider_.GetImmediateContext();
      context->OMSetRenderTargets(0, nullptr, nullptr);
      back_buffer_rtv_.Reset();
      context->Flush();
      // ResizeBuffers can't toggle the tearing flag, keeping the current one.
      swap_chain_resized = SUCCEEDED(swap_chain_->ResizeBuffers(
          0, UINT(new_swap_chain_width), UINT(new_swap_chain_height), DXGI_FORMAT_UNKNOWN,
          swap_chain_allows_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    }
    if (swap_chain_resized) {
      swap_chain_width_ = new_swap_chain_width;
      swap_chain_height_ = new_swap_chain_height;
    } else {
      REXLOG_ERROR("D3D11Presenter: Failed to resize a swap chain");
      // Retry creating from scratch.
      DestroySwapChain();
    }
  }

  if (!swap_chain_) {
    Surface::TypeIndex surface_type = new_surface.GetType();
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
    swap_chain_desc.Width = UINT(new_swap_chain_width);
    swap_chain_desc.Height = UINT(new_swap_chain_height);
    swap_chain_desc.Format = kSwapChainFormat;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = 3;
    // Don't stretch if the swap chain covers the whole surface (stretching
    // makes the content "shake" while resizing), as on D3D12.
    swap_chain_desc.Scaling =
        (new_swap_chain_width == new_surface_width && new_swap_chain_height == new_surface_height)
            ? DXGI_SCALING_NONE
            : DXGI_SCALING_STRETCH;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (REXCVAR_GET(d3d11_allow_variable_refresh_rate_and_tearing) && dxgi_supports_tearing_) {
      swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    IDXGIFactory2* dxgi_factory = provider_.GetDXGIFactory();
    switch (surface_type) {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
      case Surface::kTypeIndex_Win32Hwnd: {
        HWND surface_hwnd = static_cast<const Win32HwndSurface&>(new_surface).hwnd();
        if (FAILED(dxgi_factory->CreateSwapChainForHwnd(device, surface_hwnd, &swap_chain_desc,
                                                        nullptr, nullptr, &swap_chain_))) {
          REXLOG_ERROR("D3D11Presenter: Failed to create a swap chain for the HWND");
          return SurfacePaintConnectResult::kFailure;
        }
        // Borderless fullscreen is provided by the window; DXGI fullscreen
        // doesn't support tearing.
        dxgi_factory->MakeWindowAssociation(surface_hwnd, DXGI_MWA_NO_ALT_ENTER);
      } break;
#endif
      default:
        assert_unhandled_case(surface_type);
        REXLOG_ERROR("D3D11Presenter: Tried to create a swap chain for an unsupported surface type");
        return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
    swap_chain_width_ = new_swap_chain_width;
    swap_chain_height_ = new_swap_chain_height;
    swap_chain_allows_tearing_ =
        (swap_chain_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
  if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
      FAILED(device->CreateRenderTargetView(back_buffer.Get(), nullptr, &back_buffer_rtv_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to get the swap chain back buffer");
    DestroySwapChain();
    return SurfacePaintConnectResult::kFailure;
  }

  is_vsync_implicit_out = false;
  return SurfacePaintConnectResult::kSuccess;
}

void D3D11Presenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() { DestroySwapChain(); }

void D3D11Presenter::DestroySwapChain() {
  if (!swap_chain_) {
    return;
  }
  {
    std::lock_guard<ContextMutex> context_lock(provider_.context_mutex());
    ID3D11DeviceContext* context = provider_.GetImmediateContext();
    context->OMSetRenderTargets(0, nullptr, nullptr);
    back_buffer_rtv_.Reset();
    swap_chain_.Reset();
    // Destroy the swap chain now, not at some later point, as a new one may be
    // created for the same window right away.
    context->Flush();
  }
  swap_chain_allows_tearing_ = false;
  swap_chain_width_ = 0;
  swap_chain_height_ = 0;
}

bool D3D11Presenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  assert_not_zero(frontbuffer_width);
  assert_not_zero(frontbuffer_height);
  GuestOutputTexture& guest_output = guest_output_textures_[mailbox_index];
  if (guest_output.texture &&
      (guest_output.width != frontbuffer_width || guest_output.height != frontbuffer_height)) {
    // Painting holds its own reference while using the old one.
    guest_output = GuestOutputTexture();
  }
  if (!guest_output.texture) {
    ID3D11Device* device = provider_.GetDevice();
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = frontbuffer_width;
    desc.Height = frontbuffer_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = kGuestOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (guest_output_uav_supported_) {
      desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &guest_output.texture)) ||
        FAILED(device->CreateShaderResourceView(guest_output.texture.Get(), nullptr,
                                                &guest_output.srv)) ||
        FAILED(device->CreateRenderTargetView(guest_output.texture.Get(), nullptr,
                                              &guest_output.rtv)) ||
        (guest_output_uav_supported_ &&
         FAILED(device->CreateUnorderedAccessView(guest_output.texture.Get(), nullptr,
                                                  &guest_output.uav)))) {
      REXLOG_ERROR("D3D11Presenter: Failed to create the guest output {}x{} texture",
                   frontbuffer_width, frontbuffer_height);
      guest_output = GuestOutputTexture();
      return false;
    }
    guest_output.width = frontbuffer_width;
    guest_output.height = frontbuffer_height;
  }
  D3D11GuestOutputRefreshContext context(is_8bpc_out_ref, guest_output.texture.Get(),
                                         guest_output.uav.Get(), guest_output.rtv.Get());
  return refresher(context);
}

Presenter::PaintResult D3D11Presenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  // Take the guest output under the mailbox lock first, then the context lock
  // (the guest output refresher takes them in the same order).
  GuestOutputProperties guest_output_properties;
  GuestOutputPaintConfig guest_output_paint_config;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> guest_output_srv;
  {
    uint32_t guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(ConsumeGuestOutput(
        guest_output_mailbox_index, &guest_output_properties, &guest_output_paint_config));
    if (guest_output_mailbox_index != UINT32_MAX) {
      guest_output_srv = guest_output_textures_[guest_output_mailbox_index].srv;
    }
  }

  ID3D11Device* device = provider_.GetDevice();
  std::lock_guard<ContextMutex> context_lock(provider_.context_mutex());
  ID3D11DeviceContext* context = provider_.GetImmediateContext();
  // Other users of the context (the command processor) bind their own state
  // again when the context mutex epoch changes.
  context->ClearState();

  const float kBackBufferClearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool back_buffer_clear_needed = true;

  if (guest_output_srv) {
    GuestOutputPaintFlow guest_output_flow = GetGuestOutputPaintFlow(
        guest_output_properties, swap_chain_width_, swap_chain_height_,
        D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        guest_output_paint_config);
    for (size_t i = 0; i < guest_output_flow.effect_count; ++i) {
      if (!guest_output_paint_ps_[size_t(guest_output_flow.effects[i])]) {
        guest_output_flow.effect_count = 0;
        break;
      }
    }

    // Intermediate textures of the needed sizes, the unneeded ones released.
    for (size_t i = 0; i < intermediate_textures_.size(); ++i) {
      IntermediateTexture& intermediate = intermediate_textures_[i];
      std::pair<uint32_t, uint32_t> needed_size(0, 0);
      if (i + 1 < guest_output_flow.effect_count) {
        needed_size = guest_output_flow.effect_output_sizes[i];
      }
      if (intermediate.width == needed_size.first && intermediate.height == needed_size.second) {
        continue;
      }
      intermediate = IntermediateTexture();
      if (!needed_size.first || !needed_size.second) {
        continue;
      }
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = needed_size.first;
      desc.Height = needed_size.second;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = kGuestOutputIntermediateFormat;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      if (FAILED(device->CreateTexture2D(&desc, nullptr, &intermediate.texture)) ||
          FAILED(device->CreateShaderResourceView(intermediate.texture.Get(), nullptr,
                                                  &intermediate.srv)) ||
          FAILED(device->CreateRenderTargetView(intermediate.texture.Get(), nullptr,
                                                &intermediate.rtv))) {
        REXLOG_ERROR(
            "D3D11Presenter: Failed to create a guest output presentation intermediate texture");
        intermediate = IntermediateTexture();
        guest_output_flow.effect_count = 0;
        continue;
      }
      intermediate.width = needed_size.first;
      intermediate.height = needed_size.second;
    }

    if (guest_output_flow.effect_count) {
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      context->VSSetShader(guest_output_paint_vs_.Get(), nullptr, 0);
      context->VSSetConstantBuffers(0, 1, guest_output_paint_rectangle_constants_.GetAddressOf());
      context->PSSetConstantBuffers(0, 1, guest_output_paint_effect_constants_.GetAddressOf());
      context->PSSetSamplers(0, 1, guest_output_paint_sampler_.GetAddressOf());
      context->RSSetState(guest_output_paint_rasterizer_state_.Get());
      context->OMSetBlendState(guest_output_paint_blend_state_.Get(), nullptr, UINT_MAX);
      context->OMSetDepthStencilState(guest_output_paint_depth_stencil_state_.Get(), 0);
    }

    for (size_t i = 0; i < guest_output_flow.effect_count; ++i) {
      bool is_final_effect = i + 1 >= guest_output_flow.effect_count;
      GuestOutputPaintEffect effect = guest_output_flow.effects[i];

      // Unbind the source of the previous effect before writing to it.
      ID3D11ShaderResourceView* null_srv = nullptr;
      context->PSSetShaderResources(0, 1, &null_srv);

      ID3D11RenderTargetView* dest_rtv;
      uint32_t dest_width, dest_height;
      int32_t effect_rect_x, effect_rect_y;
      if (is_final_effect) {
        dest_rtv = back_buffer_rtv_.Get();
        dest_width = swap_chain_width_;
        dest_height = swap_chain_height_;
        effect_rect_x = guest_output_flow.output_x;
        effect_rect_y = guest_output_flow.output_y;
      } else {
        const IntermediateTexture& intermediate = intermediate_textures_[i];
        dest_rtv = intermediate.rtv.Get();
        dest_width = intermediate.width;
        dest_height = intermediate.height;
        effect_rect_x = 0;
        effect_rect_y = 0;
      }
      context->OMSetRenderTargets(1, &dest_rtv, nullptr);
      if (is_final_effect) {
        // Clear the letterbox around the guest output, the rest is overwritten.
        if (guest_output_flow.letterbox_clear_rectangle_count) {
          ID3D11DeviceContext1* context_1 = provider_.GetImmediateContext1();
          if (context_1) {
            D3D11_RECT rects[GuestOutputPaintFlow::kMaxClearRectangles];
            for (size_t j = 0; j < guest_output_flow.letterbox_clear_rectangle_count; ++j) {
              const GuestOutputPaintFlow::ClearRectangle& clear =
                  guest_output_flow.letterbox_clear_rectangles[j];
              rects[j].left = LONG(clear.x);
              rects[j].top = LONG(clear.y);
              rects[j].right = LONG(clear.x + clear.width);
              rects[j].bottom = LONG(clear.y + clear.height);
            }
            context_1->ClearView(dest_rtv, kBackBufferClearColor, rects,
                                 UINT(guest_output_flow.letterbox_clear_rectangle_count));
          } else {
            context->ClearRenderTargetView(dest_rtv, kBackBufferClearColor);
          }
        }
        back_buffer_clear_needed = false;
      }

      D3D11_VIEWPORT viewport = {};
      viewport.Width = float(dest_width);
      viewport.Height = float(dest_height);
      viewport.MaxDepth = 1.0f;
      context->RSSetViewports(1, &viewport);

      GuestOutputPaintRectangleConstants rect_constants;
      float x_to_ndc = 2.0f / viewport.Width;
      float y_to_ndc = 2.0f / viewport.Height;
      rect_constants.x = -1.0f + float(effect_rect_x) * x_to_ndc;
      // +Y is -V.
      rect_constants.y = 1.0f - float(effect_rect_y) * y_to_ndc;
      rect_constants.width = float(guest_output_flow.effect_output_sizes[i].first) * x_to_ndc;
      rect_constants.height = -float(guest_output_flow.effect_output_sizes[i].second) * y_to_ndc;
      UpdateDynamicBuffer(context, guest_output_paint_rectangle_constants_.Get(), rect_constants);

      EffectConstants effect_constants = {};
      switch (effect) {
        case GuestOutputPaintEffect::kBilinear:
        case GuestOutputPaintEffect::kBilinearDither:
          effect_constants.bilinear.Initialize(guest_output_flow, i);
          break;
        case GuestOutputPaintEffect::kCasSharpen:
        case GuestOutputPaintEffect::kCasSharpenDither:
          effect_constants.cas_sharpen.Initialize(guest_output_flow, i, guest_output_paint_config);
          break;
        case GuestOutputPaintEffect::kCasResample:
        case GuestOutputPaintEffect::kCasResampleDither:
          effect_constants.cas_resample.Initialize(guest_output_flow, i, guest_output_paint_config);
          break;
        case GuestOutputPaintEffect::kFsrEasu:
          effect_constants.fsr_easu.Initialize(guest_output_flow, i);
          break;
        case GuestOutputPaintEffect::kFsrRcas:
        case GuestOutputPaintEffect::kFsrRcasDither:
          effect_constants.fsr_rcas.Initialize(guest_output_flow, i, guest_output_paint_config);
          break;
        default:
          break;
      }
      UpdateDynamicBuffer(context, guest_output_paint_effect_constants_.Get(), effect_constants);

      ID3D11ShaderResourceView* source_srv =
          i ? intermediate_textures_[i - 1].srv.Get() : guest_output_srv.Get();
      context->PSSetShaderResources(0, 1, &source_srv);
      context->PSSetShader(guest_output_paint_ps_[size_t(effect)].Get(), nullptr, 0);
      context->Draw(4, 0);
    }
    ID3D11ShaderResourceView* null_srv = nullptr;
    context->PSSetShaderResources(0, 1, &null_srv);
  }

  ID3D11RenderTargetView* back_buffer_rtv = back_buffer_rtv_.Get();
  context->OMSetRenderTargets(1, &back_buffer_rtv, nullptr);
  if (back_buffer_clear_needed) {
    context->ClearRenderTargetView(back_buffer_rtv, kBackBufferClearColor);
  }

  if (execute_ui_drawers) {
    D3D11UIDrawContext ui_draw_context(*this, swap_chain_width_, swap_chain_height_);
    ExecuteUIDrawersFromUIThread(ui_draw_context);
  }

  // Present as soon as possible, without waiting for vsync, allowing tearing
  // for variable refresh rate where possible; present_vsync waits for the host
  // display instead (see the D3D12 presenter).
  const bool present_vsync = REXCVAR_GET(present_vsync);
  HRESULT present_result = swap_chain_->Present(
      present_vsync ? 1 : 0,
      DXGI_PRESENT_RESTART |
          (!present_vsync && swap_chain_allows_tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0));
  // The flip model unbinds the back buffer on Present.
  context->OMSetRenderTargets(0, nullptr, nullptr);
  provider_.LogD3D11DebugMessages();
  if (present_result == DXGI_ERROR_DEVICE_REMOVED || present_result == DXGI_ERROR_DEVICE_RESET) {
    REXLOG_ERROR("D3D11 Present lost the device: result 0x{:08X}, removed reason 0x{:08X}",
                 uint32_t(present_result), uint32_t(device->GetDeviceRemovedReason()));
    rex::FlushLogging();
  }
  switch (present_result) {
    case DXGI_ERROR_DEVICE_REMOVED:
      return PaintResult::kGpuLostExternally;
    case DXGI_ERROR_DEVICE_RESET:
      return PaintResult::kGpuLostResponsible;
    default:
      return SUCCEEDED(present_result) ? PaintResult::kPresented : PaintResult::kNotPresented;
  }
}

bool D3D11Presenter::InitializeSurfaceIndependent() {
  // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING support.
  {
    Microsoft::WRL::ComPtr<IDXGIFactory5> dxgi_factory_5;
    if (SUCCEEDED(provider_.GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&dxgi_factory_5)))) {
      BOOL tearing_feature_data;
      dxgi_supports_tearing_ = SUCCEEDED(dxgi_factory_5->CheckFeatureSupport(
                                   DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing_feature_data,
                                   sizeof(tearing_feature_data))) &&
                               tearing_feature_data;
    }
  }

  ID3D11Device* device = provider_.GetDevice();

  // Typed UAV stores of the guest output format (required for R10G10B10A2 at
  // feature level 11.0, checked anyway).
  {
    UINT format_support = 0;
    guest_output_uav_supported_ =
        SUCCEEDED(device->CheckFormatSupport(kGuestOutputFormat, &format_support)) &&
        (format_support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW);
    if (!guest_output_uav_supported_) {
      REXLOG_WARN("D3D11Presenter: The guest output format can't be written through a UAV");
    }
  }

  if (FAILED(device->CreateVertexShader(shaders::guest_output_triangle_strip_rect_vs,
                                        sizeof(shaders::guest_output_triangle_strip_rect_vs),
                                        nullptr, &guest_output_paint_vs_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output painting vertex shader");
    return false;
  }
  for (size_t i = 0; i < size_t(GuestOutputPaintEffect::kCount); ++i) {
    const void* code;
    size_t code_size;
    switch (GuestOutputPaintEffect(i)) {
      case GuestOutputPaintEffect::kBilinear:
        code = shaders::guest_output_bilinear_ps;
        code_size = sizeof(shaders::guest_output_bilinear_ps);
        break;
      case GuestOutputPaintEffect::kBilinearDither:
        code = shaders::guest_output_bilinear_dither_ps;
        code_size = sizeof(shaders::guest_output_bilinear_dither_ps);
        break;
      case GuestOutputPaintEffect::kCasSharpen:
        code = shaders::guest_output_ffx_cas_sharpen_ps;
        code_size = sizeof(shaders::guest_output_ffx_cas_sharpen_ps);
        break;
      case GuestOutputPaintEffect::kCasSharpenDither:
        code = shaders::guest_output_ffx_cas_sharpen_dither_ps;
        code_size = sizeof(shaders::guest_output_ffx_cas_sharpen_dither_ps);
        break;
      case GuestOutputPaintEffect::kCasResample:
        code = shaders::guest_output_ffx_cas_resample_ps;
        code_size = sizeof(shaders::guest_output_ffx_cas_resample_ps);
        break;
      case GuestOutputPaintEffect::kCasResampleDither:
        code = shaders::guest_output_ffx_cas_resample_dither_ps;
        code_size = sizeof(shaders::guest_output_ffx_cas_resample_dither_ps);
        break;
      case GuestOutputPaintEffect::kFsrEasu:
        code = shaders::guest_output_ffx_fsr_easu_ps;
        code_size = sizeof(shaders::guest_output_ffx_fsr_easu_ps);
        break;
      case GuestOutputPaintEffect::kFsrRcas:
        code = shaders::guest_output_ffx_fsr_rcas_ps;
        code_size = sizeof(shaders::guest_output_ffx_fsr_rcas_ps);
        break;
      case GuestOutputPaintEffect::kFsrRcasDither:
        code = shaders::guest_output_ffx_fsr_rcas_dither_ps;
        code_size = sizeof(shaders::guest_output_ffx_fsr_rcas_dither_ps);
        break;
      default:
        // Not supported by this implementation.
        continue;
    }
    if (FAILED(device->CreatePixelShader(code, code_size, nullptr, &guest_output_paint_ps_[i]))) {
      REXLOG_ERROR("D3D11Presenter: Failed to create the guest output painting shader {}", i);
      return false;
    }
  }

  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.Usage = D3D11_USAGE_DYNAMIC;
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  constants_desc.ByteWidth = UINT(rex::round_up(sizeof(GuestOutputPaintRectangleConstants), 16));
  if (FAILED(device->CreateBuffer(&constants_desc, nullptr,
                                  &guest_output_paint_rectangle_constants_))) {
    return false;
  }
  constants_desc.ByteWidth = UINT(rex::round_up(sizeof(EffectConstants), 16));
  if (FAILED(
          device->CreateBuffer(&constants_desc, nullptr, &guest_output_paint_effect_constants_))) {
    return false;
  }

  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxAnisotropy = 1;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  D3D11_BLEND_DESC blend_desc = {};
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  D3D11_DEPTH_STENCIL_DESC depth_stencil_desc = {};
  if (FAILED(device->CreateSamplerState(&sampler_desc, &guest_output_paint_sampler_)) ||
      FAILED(device->CreateRasterizerState(&rasterizer_desc,
                                           &guest_output_paint_rasterizer_state_)) ||
      FAILED(device->CreateBlendState(&blend_desc, &guest_output_paint_blend_state_)) ||
      FAILED(device->CreateDepthStencilState(&depth_stencil_desc,
                                             &guest_output_paint_depth_stencil_state_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output painting states");
    return false;
  }

  return InitializeCommonSurfaceIndependent();
}

}  // namespace rex::ui::d3d11
