/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/ui/renderdoc_api.h>

REXCVAR_DEFINE_BOOL(gpu_allow_invalid_fetch_constants, false, "GPU",
                    "Allow invalid fetch constants");
REXCVAR_DEFINE_BOOL(gpu_allow_invalid_upload_range, false, "GPU",
                    "Allows games to read data from pages that are marked as no access.");
REXCVAR_DEFINE_BOOL(native_2x_msaa, true, "GPU", "Enable native 2x MSAA");
REXCVAR_DEFINE_BOOL(precise_interpolation, true, "GPU",
                    "Interpolate pixel shader inputs manually from the per-vertex values (Vulkan "
                    "with VK_KHR_fragment_shader_barycentric) to avoid host interpolation "
                    "imprecision, which causes noise in games that compare interpolated values "
                    "for equality, such as Perfect Dark Zero and Tenchu Z on Nvidia.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(depth_float24_round, false, "GPU", "Round float24 depth values");
REXCVAR_DEFINE_BOOL(depth_float24_convert_in_pixel_shader, false, "GPU",
                    "Convert float24 depth in pixel shader");
REXCVAR_DEFINE_BOOL(depth_transfer_not_equal_test, true, "GPU",
                    "Use not-equal test for depth transfer");
REXCVAR_DEFINE_BOOL(gamma_render_target_as_unorm16, true, "GPU",
                    "Use R16G16B16A16_UNORM for gamma render targets (more accurate than sRGB)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "Path to dump shaders to");
REXCVAR_DEFINE_BOOL(mulsc_round_toward_zero, false, "GPU",
                    "Round mulsc products toward zero instead of to nearest even. This fixes bad "
                    "geometry in several Volition Engine titles and possibly others. This rounding "
                    "behavior hasn't been confirmed on real hardware, so it's disabled by default.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(use_fuzzy_alpha_epsilon, false, "GPU",
                    "Use approximate compare for alpha test values to prevent "
                    "flickering on NVIDIA graphics cards");
REXCVAR_DEFINE_BOOL(gpu_debug_markers, false, "GPU",
                    "Insert debug markers into GPU command streams for tools "
                    "like PIX and RenderDoc. Automatically enabled when "
                    "RenderDoc is detected.");
REXCVAR_DEFINE_BOOL(force_depth_clamp, false, "GPU",
                    "Use host depth clamping instead of near and far plane clipping when "
                    "guest clipping is enabled. X/Y/W clipping is unaffected. On Vulkan, "
                    "this requires depthClamp support.");
REXCVAR_DEFINE_INT32(occlusion_query_fake_lower_threshold, 80, "GPU",
                     "Lower end of the fake sample count value written on "
                     "EVENT_WRITE_ZPD when real occlusion queries are disabled.\n"
                     "-1 writes nothing, resulting in some games that sit and hang.\n"
                     "0 means the fake result stays fully occluded.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(occlusion_query_fake_upper_threshold, 100, "GPU",
                     "Upper end of the fake sample count value written on "
                     "EVENT_WRITE_ZPD when real occlusion queries are disabled.\n"
                     "Keep this higher than occlusion_query_fake_lower_threshold.\n"
                     "Ignored if occlusion_query_fake_lower_threshold is -1.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(xenos_sample_positions, false, "GPU",
                    "Rasterize 2x and 4x MSAA render targets at the Xenos sample positions, like "
                    "real hardware, if the GPU and driver support it.\n"
                    "Only applies to RTV/FBO - ROV/FSI render targets always use Xenos positions "
                    "since they safely map coverage and calculate depth in-shader.");

bool IsGpuDebugMarkersEnabled() {
  static bool cached = false;
  static bool result = false;
  if (!cached) {
    cached = true;
    if (REXCVAR_GET(gpu_debug_markers)) {
      result = true;
      REXLOG_INFO("GPU debug markers enabled via CVar");
    } else {
      auto renderdoc_api = rex::ui::RenderDocAPI::CreateIfConnected();
      if (renderdoc_api) {
        result = true;
        REXLOG_INFO("GPU debug markers auto-enabled (RenderDoc detected)");
      }
    }
  }
  return result;
}
