/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Ported for the ReXGlue fork: Direct3D 11 version of the
 *              D3D12 immediate drawer.
 */

#pragma once

#include <cstdint>
#include <memory>

#include <rex/ui/d3d11/d3d11_provider.h>
#include <rex/ui/immediate_drawer.h>

namespace rex::ui::d3d11 {

class D3D11ImmediateDrawer final : public ImmediateDrawer {
 public:
  static std::unique_ptr<D3D11ImmediateDrawer> Create(const D3D11Provider& provider) {
    auto immediate_drawer =
        std::unique_ptr<D3D11ImmediateDrawer>(new D3D11ImmediateDrawer(provider));
    if (!immediate_drawer->Initialize()) {
      return nullptr;
    }
    return immediate_drawer;
  }

  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                  ImmediateTextureFilter filter, bool is_repeated,
                                                  const uint8_t* data) override;

  // The UI draw context's thread holds the provider's context mutex (painting
  // does) for the whole Begin...End.
  void Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  enum class SamplerIndex {
    kNearestClamp,
    kLinearClamp,
    kNearestRepeat,
    kLinearRepeat,

    kCount,
  };

  // Direct3D 11 releases resources only after the GPU is done with them, so
  // textures need no lifetime tracking here.
  class D3D11ImmediateTexture final : public ImmediateTexture {
   public:
    static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11ImmediateTexture(uint32_t width, uint32_t height, ID3D11ShaderResourceView* srv,
                          SamplerIndex sampler_index)
        : ImmediateTexture(width, height), srv_(srv), sampler_index_(sampler_index) {}

    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    SamplerIndex sampler_index() const { return sampler_index_; }

   private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    SamplerIndex sampler_index_;
  };

  explicit D3D11ImmediateDrawer(const D3D11Provider& provider) : provider_(provider) {}
  bool Initialize();

  // Grows the buffer to at least the size if needed and maps it with discard.
  void* MapDynamicBuffer(Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer, UINT& buffer_size,
                         UINT size_needed, UINT bind_flags);

  const D3D11Provider& provider_;

  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_state_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> samplers_[size_t(SamplerIndex::kCount)];
  Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_;
  // Bound for drawing without a texture: white, so the vertex color is used.
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> white_texture_srv_;

  Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
  UINT vertex_buffer_size_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Buffer> index_buffer_;
  UINT index_buffer_size_ = 0;

  bool batch_open_ = false;
  bool batch_has_index_buffer_ = false;
  D3D11_RECT current_scissor_ = {};
  D3D_PRIMITIVE_TOPOLOGY current_primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11ShaderResourceView* current_srv_ = nullptr;
  ID3D11SamplerState* current_sampler_ = nullptr;
};

}  // namespace rex::ui::d3d11
