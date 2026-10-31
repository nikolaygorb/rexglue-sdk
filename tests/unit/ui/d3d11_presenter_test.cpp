/**
 * @file        ui/d3d11_presenter_test.cpp
 * @brief       Tests for the Direct3D 11 provider, immediate drawer and
 *              presenter on WARP at feature level 11.0 (no window needed)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/ui/d3d11/d3d11_presenter.h>
#include <rex/ui/d3d11/d3d11_provider.h>

REXCVAR_DECLARE(int32_t, d3d11_adapter);
REXCVAR_DECLARE(std::string, d3d11_feature_level);

using rex::ui::d3d11::D3D11Presenter;
using rex::ui::d3d11::D3D11Provider;

namespace {

std::unique_ptr<D3D11Provider> CreateWarpProvider() {
  int32_t adapter = REXCVAR_GET(d3d11_adapter);
  std::string feature_level = REXCVAR_GET(d3d11_feature_level);
  REXCVAR_SET(d3d11_adapter, -2);
  REXCVAR_SET(d3d11_feature_level, std::string("11_0"));
  std::unique_ptr<D3D11Provider> provider = D3D11Provider::Create();
  REXCVAR_SET(d3d11_adapter, adapter);
  REXCVAR_SET(d3d11_feature_level, feature_level);
  return provider;
}

}  // namespace

TEST_CASE("D3D11 provider on WARP capped to feature level 11.0", "[ui][d3d11]") {
  std::unique_ptr<D3D11Provider> provider = CreateWarpProvider();
  if (!provider) {
    SKIP("WARP is not available");
  }
  CHECK(provider->GetFeatureLevel() == D3D_FEATURE_LEVEL_11_0);
  CHECK(provider->IsSoftwareAdapter());
  CHECK_FALSE(provider->AreUAVsAtEveryStageSupported());
  CHECK(provider->GetUAVSlotCount() == 8);
  CHECK(provider->CreateImmediateDrawer() != nullptr);
}

TEST_CASE("D3D11 presenter refreshes and captures the guest output", "[ui][d3d11]") {
  std::unique_ptr<D3D11Provider> provider = CreateWarpProvider();
  if (!provider) {
    SKIP("WARP is not available");
  }
  // Creates every guest output painting shader and state.
  std::unique_ptr<rex::ui::Presenter> presenter = provider->CreatePresenter();
  REQUIRE(presenter);

  bool refresher_called = false;
  bool uav_available = false;
  REQUIRE(presenter->RefreshGuestOutput(
      64, 32, 16, 9, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& d3d11_context =
            static_cast<D3D11Presenter::D3D11GuestOutputRefreshContext&>(context);
        refresher_called = true;
        uav_available = d3d11_context.uav() != nullptr;
        std::lock_guard<rex::ui::d3d11::ContextMutex> lock(provider->context_mutex());
        const float red[] = {1.0f, 0.0f, 0.0f, 1.0f};
        provider->GetImmediateContext()->ClearRenderTargetView(d3d11_context.rtv(), red);
        return true;
      }));
  CHECK(refresher_called);
  // R10G10B10A2_UNORM typed UAV stores are required at feature level 11.0.
  CHECK(uav_available);

  rex::ui::RawImage image;
  REQUIRE(presenter->CaptureGuestOutput(image));
  CHECK(image.width == 64);
  CHECK(image.height == 32);
  REQUIRE(image.data.size() == size_t(64) * 32 * 4);
  CHECK(image.data[0] == 255);
  CHECK(image.data[1] == 0);
  CHECK(image.data[2] == 0);
}

// D3D11CommandProcessor ends its occlusion query when another thread (the
// presenter, the UI) takes the context lock, so their draws aren't counted.
TEST_CASE("The context mutex reports a change of the owning thread", "[ui][d3d11]") {
  rex::ui::d3d11::ContextMutex mutex;
  uint32_t calls = 0;
  mutex.SetOwnerChangeCallback([&calls]() { ++calls; });
  {
    std::lock_guard<rex::ui::d3d11::ContextMutex> lock(mutex);
    // Recursive locking by the owner isn't a change.
    std::lock_guard<rex::ui::d3d11::ContextMutex> nested_lock(mutex);
  }
  CHECK(calls == 1);
  {
    std::lock_guard<rex::ui::d3d11::ContextMutex> lock(mutex);
  }
  CHECK(calls == 1);
  std::thread other([&mutex]() { std::lock_guard<rex::ui::d3d11::ContextMutex> lock(mutex); });
  other.join();
  CHECK(calls == 2);
  {
    std::lock_guard<rex::ui::d3d11::ContextMutex> lock(mutex);
  }
  CHECK(calls == 3);
  mutex.SetOwnerChangeCallback(nullptr);
  std::thread again([&mutex]() { std::lock_guard<rex::ui::d3d11::ContextMutex> lock(mutex); });
  again.join();
  CHECK(calls == 3);
}
