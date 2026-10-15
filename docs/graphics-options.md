# Graphics options: resolution, upscaling, sharpening, frame generation

This page lists the image quality options of titles built with ReXGlue, and the driver-level tools that work with them without any support in the title. Options are settings (cvars) in the title's settings file, for example `settings/hardware.toml` (`key = value`, read at startup).

## Built-in options

| Setting | Values | What it does |
|---|---|---|
| `resolution_scale` (or `draw_resolution_scale_x` / `draw_resolution_scale_y`) | 1 (default), 2, 3, … | Renders the game at a multiple of its original resolution. The best image quality of all options, at a high GPU and memory cost. 2 or 3 is typical. |
| `present_effect` | `bilinear` (default), `cas`, `fsr` | How the game image is scaled to the window. `cas`: AMD FidelityFX CAS sharpening (with light upscaling up to 2x). `fsr`: AMD FidelityFX Super Resolution 1 (edge-preserving upscaling, then sharpening). Both work on any GPU. |
| `present_cas_additional_sharpness` | 0.0 (default) to 1.0 | Extra CAS sharpness. |
| `present_fsr_sharpness_reduction` | 0.2 (default) to 2.0 | FSR sharpening strength, in stops: higher is softer. |
| `present_fsr_max_upsampling_passes` | 1 to 4 (default 4) | Limits the chained FSR upscaling passes (each pass is up to 2x). |
| `present_dither` | `false` (default), `true` | Dithers the final image; removes banding in skies and gradients. |
| `swap_post_effect` | `none` (default), `fxaa`, `fxaa_extreme`, `smaa` | Anti-aliasing on the game image. `fxaa`: fast, slightly blurry. `smaa`: SMAA 1x (high preset), sharper edges and better on thin lines, a bit more GPU time. |
| `anisotropic_override` | -1 to 5 (default 3 = 4x) | Forces anisotropic texture filtering; 5 = 16x. |

`resolution_scale` and `present_effect` combine: render at 2x, then `fsr` or `cas` to scale to a 4K window.

Builds made with the optional AMD FidelityFX SDK (`REXGLUE_ENABLE_FIDELITYFX=ON`) also offer `present_effect = fsr2` / `fsr3` and `present_fsr_quality_mode`. They are experimental: the title does not yet give the upscaler depth, motion vectors or camera jitter, so it works on one frame at a time and is usually no better than `fsr`.

DLSS, XeSS, FSR 2 and later as real temporal upscalers, frame generation inside the title, and ambient occlusion are not available yet. They need per-title support.

## Direct3D 12 and Vulkan

Titles can render with Direct3D 12 (Windows only) or Vulkan (Windows, Linux). All built-in options above work the same on both.

### Choosing the backend

The backend is chosen by the title. Titles that let the player pick it usually have a setting such as `graphics_backend = "any" | "d3d12" | "vulkan"` (Perfect Dark Zero recomp does). With `any`, Direct3D 12 is tried first, then Vulkan. On Windows, Vulkan is only there if the title was built with `REXGLUE_USE_VULKAN=ON` (off by default).

Which one to use: Direct3D 12 is the default and usually the safer choice on Windows. Try Vulkan if a game shows a rendering bug or runs worse on Direct3D 12 on your GPU, or if a tool you want needs it. Report a bug that shows on only one backend together with the backend name.

### Backend settings

| Setting | Backend | Values | What it does |
|---|---|---|---|
| `render_target_path_d3d12` | D3D12 | `""` (auto), `rtv`, `rov` | How the console's EDRAM (its render target memory) is emulated. `rtv`: host render targets, fast; the default (auto picks `rov` only on Intel GPUs older than Arc, because of a driver bug). `rov`: rasterizer-ordered views, more accurate blending and formats, much slower. |
| `render_target_path_vulkan` | Vulkan | `""` (auto = `fbo`), `fsi` | Same choice for Vulkan. `fbo`: host render targets, fast. `fsi`: fragment shader interlock, more accurate, slower, needs GPU support (otherwise falls back). |
| `d3d12_adapter` | D3D12 | -1 (auto), 0, 1, … | Which GPU to use, by DXGI adapter index; -2 = WARP (software, for debugging). |
| `vulkan_device` | Vulkan | -1 (auto), 0, 1, … | Which GPU to use, by Vulkan device index. |
| `d3d12_pipeline_creation_threads` | D3D12 | -1 (auto) to 32 | Threads that create pipelines in the background. |
| `vulkan_async_skip_incomplete_frames` | Vulkan | `true` (default), `false` | With `async_shader_compilation`, does not show frames where some draws were skipped while their pipelines were being created. Avoids flashing. |
| `d3d12_allow_variable_refresh_rate_and_tearing` | D3D12 | `true` (default), `false` | Allows G-Sync/FreeSync and tearing when `vsync = false`. |
| `vulkan_allow_present_mode_immediate`, `vulkan_allow_present_mode_mailbox`, `vulkan_allow_present_mode_fifo_relaxed` | Vulkan | `true` (default), `false` | Which Vulkan present modes may be used: immediate (no vsync, tearing), mailbox (low latency without tearing), FIFO relaxed (vsync that tears when late). Turn one off if a driver handles it badly. |

### Settings for both backends

| Setting | Values | What it does |
|---|---|---|
| `async_shader_compilation` | `true` (default), `false` | Compiles shaders in background threads: less stutter, but objects can be missing for a moment the first time they appear. |
| `store_shaders` | `true` (default), `false` | Keeps compiled shaders on disk, so later runs stutter less. |
| `vsync` | `true` (default), `false` | Paces the game to the display. Many games tie their speed to vsync: with `false` they can run too fast. |
| `fullscreen`, `fullscreen_exclusive` | `true` / `false` | Start in fullscreen; exclusive switches the display mode instead of borderless. |
| `native_2x_msaa` | `true` (default), `false` | Uses the GPU's native 2x MSAA for games that use 2x MSAA. |

## Driver and external tools

These work on the final image of any title and need nothing from it.

| Tool | GPUs | Backends | What it does | Notes |
|---|---|---|---|---|
| AMD Fluid Motion Frames (AFMF) | AMD Radeon | D3D12; Vulkan with AFMF 2 | Frame generation in the driver | Enable in AMD Software. Adds latency; HUD can show artifacts. |
| NVIDIA Smooth Motion | NVIDIA RTX 40 and newer | D3D12, Vulkan | Frame generation in the driver | Enable in the NVIDIA app per program. Same trade-offs as AFMF. |
| Lossless Scaling | any | any | Frame generation and spatial upscaling in a separate window | Paid third-party app. |
| NVIDIA RTX HDR | NVIDIA RTX | D3D12, Vulkan | Converts the SDR image to HDR | Enable in the NVIDIA app. |
| Windows Auto HDR | any, HDR display | D3D12 only | Converts the SDR image to HDR | Windows 11 display settings. Use the D3D12 backend for it. |
| ReShade | any | D3D12, Vulkan | Post-processing shaders (sharpening, color, SMAA) | Install for the API the title uses. Effects that need depth (ambient occlusion, depth of field) are unreliable: the game's depth is not kept in a normal depth buffer. |

Driver features change between driver versions; check the vendor's current list if one does not appear for a title.

Frame generation happens after the game has drawn its frame, so it does not change game speed or timing. It works best when the game already runs at a stable frame rate (for example 30 or 60).
