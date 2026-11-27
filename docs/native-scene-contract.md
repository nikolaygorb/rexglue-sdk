# Native scene contract (temporal inputs)

Native Vulkan only. Gives a title the inputs a temporal upscaler / frame generator needs
(FSR 3.1, XeSS SR, DLSS): owned scene color and depth history, jitter, motion vectors
and masks. The SDK holds no title knowledge; the title annotates its own command stream
and the SDK verifies every claim against the draw it actually executed.

Reference producer: PerfectDarkZeroRecomp `src/render/engine/scene_probe.cpp`.

## Split of work

| Title (producer) | SDK (consumer, `src/graphics/native/scene_*`) |
|---|---|
| Finds the world shader, camera constants, scene viewport, end-of-scene and before-HUD points | Verifies the camera against the executed VS (`camera_register`), viewport and `P x V` consistency |
| Writes markers (`rex/graphics/title_scene.h`) into its PM4 stream | Retains color/depth per frame (6 GPU slots, fence-retired) |
| Sends simulated `delta_time`, history resets (cuts, teleports, control changes) | Applies Halton jitter to scene draws only; motion excludes jitter; object motion from the scene draws themselves |
| Picks `locked_distance` (first-person weapon), `kLockedVolume` (followed third-person character), `kSceneAutoOpaque` | Camera motion from depth, sky/far and camera-locked classes (distance or volume), reactive mask from the opaque snapshot |

## Markers (protocol 4, opcode 0x66, 64 words; 3 still accepted)

| Point | When | SDK action |
|---|---|---|
| `kView` + `kSceneCamera` | right after the first draw that uses the camera | verify, start the frame's view; a later accepted one of the frame replaces it, a refused one keeps it |
| `kOpaqueEnd` (optional) | opaque/translucent boundary | opaque snapshot (or use `kSceneAutoOpaque`) |
| `kSceneEnd` | scene color/depth final, before post | retain + compute; jitter stops |
| `kPostEnd` | before the HUD; the bound target is the frame without the HUD | debug view output; frame generation's hudless snapshot |
| `kFrameEnd` | before Swap | reports; arm next frame's jitter |
| `kLockedVolume` (4) | after `kView`, before `kSceneEnd`; up to 4 | view-space box (`locked_box` min/max) moving with the camera: a third-person character the camera follows; its pixels get zero motion |

A producer may annotate several draws of a frame (Fuse: the 1st, 8th and 32nd camera draw), so a draw the
backend skipped (`view-last-draw`, e.g. its pipeline still building) costs no view. Without `camera_register`
the view needs a recorded draw of this frame with the same targets, not that draw's shader. Refusals by reason:
`frame-consumer.json` `reject_reasons`.

The scene viewport may sit anywhere in its target on whole pixels (letterboxed scenes); the
marker's `width`/`height` are its size, the SDK takes its origin from the verified draw.

Camera block: `view` 3x4, `projection` 4x4, `view_projection` 4x4; row-major, column vectors,
Direct3D clip (z/w in [0,1]). Frames must be consecutive; a gap resets history.

Protocol 4 (any marker of a frame): `frame_x/y/width/height` (words 18-19) where the scene viewport
appears in the front buffer, guest pixels (0: at the origin 1:1); flag `kSceneNoFrameGeneration` (16):
no generated frame before this one. A producer checks `GetTitleSceneProtocolVersion() >= ` its own.

## Porting a title

1. Find the world VS and where its camera constants are (capture: `native_scene_probe_*`).
2. Producer: on the camera draw emit `kView` with the matrices, `camera_register`, viewport;
   emit `kSceneEnd`/`kPostEnd` around composition and `kFrameEnd` before Swap. Append inside
   already-reserved ring space only; never reorder or re-run title work. A marker that finds the
   segment full may be placed once the title's own call made room: `kSceneEnd` right after its
   resolve (the bound target is unchanged until the next draw; a resolve through an alias format at
   the same EDRAM base retains the view's surface), `kFrameEnd` after the swap (the presented frame
   is then taken at the present from its complete scene and HUD-less image, and the next frame's
   1x scene interval, opened at the swap, stays open).
3. Resets: set `kSceneReset` on camera cuts, teleports, control changes; give `delta_time`.
   Shader variants (per level): `QueryTitleSceneStats(stats, 5)` gives accepted views and the last
   accepted `source_hash`, so a producer can offer a candidate shader as `kView` and keep it once
   the SDK verified it (PDZ: after a second without views).
4. Produce while the consumer is on: read `native_temporal_upscaler` / `native_scene_frames` by name
   (`rex::cvar::GetFlagByName`; the plugin's flags are registered before the title loads), again on
   change (`RegisterChangeCallback`), switching at a Swap. Track shader identities from the start.
5. Run with `native_scene_frames=true`, check `native_scene_debug` 1-4 and `frame-consumer.json`
   (`rejected`/`last_reject_reason`, `resets`, `jitter_draws`, `opaque_snapshots`).
6. Enable `native_scene_jitter=8`: debug 3 (motion) must stay gray on a still camera.

## Cvars

`native_scene_frames` (restart), `native_scene_debug` 0-5 (5: pixels with object motion), `native_scene_object_motion` (restart), `native_scene_jitter` phases (0 off),
`native_scene_frame_images` (diagnostics).

## Outputs per slot

| Image | Format | Content |
|---|---|---|
| color | RGBA16F | scene color (source range, LDR if the title is LDR) |
| depth | R32F | raw host depth |
| motion | RGBA16F | xy current -> previous image pixels without jitter (the view in the image's corner; the previous view may have another size), z expected previous depth, w coverage |
| masks | RGBA8 | reactive, camera-locked, sky/far, coverage |

Camera facts for providers: `scene::DeriveCamera` (near, far, fov, depth inversion).

## Object motion (no title work)

`native_scene_object_motion` (default on, restart; needs `vertexPipelineStoresAndAtomics`). From the frame after a
retained view, every draw into the scene depth over the scene viewport that writes depth (triangles, host vertex
shader, no memexport or shader depth) stores its final host clip position per vertex (the translator's
`SetMotionCapture`: binding 3 of set 0, `motion_capture_*` system constants; 20 bytes per vertex, two frames,
`native_scene_object_motion_vertices`). At `kSceneEnd` each draw is matched to the previous retained frame's
(shaders, index and vertex buffers, vertex count, index offset; ties by the closest float constants: the title's
identity registers `native_scene_object_motion_identity`, e.g. the world transform, when the shader reads them,
else its first used ones, which may include camera-dependent matrices that swap copies of a mesh while the camera
turns), drawn again
from the stored positions with the previous ones (`ObjectMotion`), and where it is the visible surface (its depth at
the retained depth's sample 0) gives the previous uv and depth; elsewhere camera motion. Camera-locked classes keep
zero motion. Report: `frame-consumer.json` `object_motion` (`matched_draws`, `budget_skips`, `peak_vertices`).
An extent-only reset (letterbox bars moving) keeps camera/object motion and frame generation history
(`extent_continues`); temporal upscalers still start anew. Scene images only grow (a smaller view uses their corner),
so viewport steps cost no GPU wait; the scene pass fills them to the largest view so far (`extent_guest_*`, the view's
edge repeated) and the upscalers take that constant size (one context, history kept across letterbox steps), the
view's part copied back; `image_growths`, `retain_slow`, `retain_max_ms` in the report.

## Temporal upscaler (AMD FSR, Intel XeSS SR, NVIDIA DLSS SR)

`native_temporal_upscaler=fsr3|fsr4|xess|dlss` (Windows; `fsr` = `fsr3`), switchable while running, provider included.
AMD FSR: SDK 2.4 signed Vulkan DLLs, downloaded and staged by `cmake/rexglue_fsr_sdk.cmake`
(`REXGLUE_ENABLE_FSR_SDK`, pinned commit + SHA-256, license copied as `amd_fsr_sdk_license.md`). fsr4: FSR 4 only
where the GPU offers it (`rex/ui/vulkan/fsr_versions.h`), else FSR 3.1, also while running.
Intel XeSS SR: `libxess.dll` of SDK 3.0.2 (SR 2.0.2), `cmake/rexglue_xess_sdk.cmake` (`REXGLUE_ENABLE_XESS_SDK`,
~78 MB, license and notices copied as `intel_xess_*.txt`). Hardware (Intel's guide): Intel Iris Xe or later (Intel implementation; UHD 770 reported the cross-vendor one), other vendors with `shaderStorageImageWriteWithoutFormat` + `mutableDescriptorType` (DP4a recommended); Windows 10/11 x64. XeSS's instance/device extensions and features are
enabled whenever the DLL is there (`rex/ui/vulkan/xess.h`; a device that fails with them is created without), so
switching needs no restart. Pipelines build in the background on first use, cached per GPU in
`<cache_root>/shaders/local/xess_vk_<vendor>_<device>.bin` (beside the renderer's driver cache); the preset follows the render/output ratio (Native AA 1x .. Ultra Performance
3x); the output size re-initialises XeSS after it settles (4 frames). A provider's ratio limit (`MaxUpscaleRatio`: XeSS 3x, FSR none) keeps the scene at the scale above it (native-resolution AA, logged and in `logs/upscaler.log`). Inputs: the same color, `ndc_depth_`, RGBA16F
motion (RG used), jitter; no sharpening; `native_xess_responsive_mask` (off) gives the reactive mask.
NVIDIA DLSS SR / DLAA: DLSS SDK 310.9.16, `cmake/rexglue_dlss_sdk.cmake` (`REXGLUE_ENABLE_DLSS_SDK`; off with
fuzzers/sanitizers): NGX headers, its static library linked into rexruntime only (one NGX per process),
`nvngx_dlss.dll` / `nvngx_dlssg.dll` and the license copied as `nvidia_dlss_sdk_license.txt`. `rex/ui/vulkan/ngx.h`
(`vulkan_ngx.cpp`) is NGX's only user, under one lock (NGX is not thread-safe); the project ID is derived from the
executable's name (no title input); NGX logs go to `logs/ngx`, warnings and errors to our log (`NGX: ...`). Its
instance and device requirements are added at start when NGX reports them (not NVIDIA, old driver: none; a device
that fails with them is created without). Mode by the output/input ratio (1: DLAA, else the NGX mode whose range
holds the input: 2x Performance), up to 3x (`MaxUpscaleRatio`). A new output size gets a new feature at once, the
old one released after its submissions (`Submitter::DestroyAfter`); a frame in another format (LDR/HDR) goes through
the current feature, re-created after 30 such frames. Flags: low-resolution motion, depth inversion from the
projection, HDR + auto exposure for a float scene; jitter as FSR's. Create/evaluate run in `CmdExternal` on the
submission thread. `native_dlss_preset` (default/j/k/l/m).
Any failure (no DLL, platform, device, context, dispatch) keeps the original image and logs why.
Native-resolution AA: at `kSceneEnd` the output replaces the scene color (RGB), so place
`kSceneEnd` before the title resolves the scene for post/composition. While it runs,
`swap_post_effect` is skipped (logged). Jitter phases come from the provider unless `native_scene_jitter`
is set. Cvars: `native_fsr_sharpness`, `native_fsr_color_space` (auto by guest format / srgb / linear / hdr; XeSS:
srgb/linear = LDR input), `native_fsr_reactive`, `native_fsr_debug_checking`, `native_xess_responsive_mask`.
Report: `frame-consumer.json` `upscaler` / `upscaled` / `upscaler_skips`; one line for players in
`logs/upscaler.log` (version and GPU, or why not), shown by the launcher. Launcher rows: `docs/launcher.md`
(the title lists them in `[common] enable`).

**Real upscale** (`native_scene_upscale`, default off; needs a resolution scale above 1 and an active
upscaler; off = native-resolution AA). `native_temporal_upscaler` alone turns `native_scene_frames` on.
The scene interval opens at the SDK's swap while views arrive (within 30 swaps of the last
accepted `kView`) and the upscaler runs, and closes at `kSceneEnd` (also `kPostEnd`, `kFrameEnd`
if it was lost). In it the scene's surfaces (the view's color/depth and every target drawn with
them) get their own 1x images (`RenderTargets` scene policy); those draws use
the 1x pixel shader variant, their resolves become ordinary 1x textures. At `kSceneEnd` the upscaler writes
the scene at the scale into the surface's scaled image, so post, composition and HUD stay at the
scale. Occlusion counts are normalized per scale segment. Report: `upscaled_width/height`,
`scene_interval`. The title only has to place `kSceneEnd` before the scene's resolve.

## Frame generation (AMD FSR, NVIDIA DLSS)

`native_frame_generation=fsr3|fsr4|dlss` (presenter, Windows, restart; fsr4 as the upscaler's, FSR 4
generation and swapchain together or FSR 3 for both): AMD's proxy swapchain
(`amd_fidelityfx_framegeneration_vk.dll`) on extra queues `VulkanDevice` creates then (present,
acquire, and with `native_frame_generation_async` a compute queue: generation overlaps the next
frame, the hudless image is double-buffered); turns
`native_scene_frames` on. Per real frame the native renderer writes, at the swap, into the presenter's
images of the guest output: depth (guest clip z/w) and motion placed by the frame rectangle (outside
it still and far), and the `kPostEnd` snapshot through the same gamma pass (the frame without the HUD).
The presenter paints that hudless image like the frame, records Prepare (camera from the view) and
configures generation; overlays, repaints, NIS and `kSceneNoFrameGeneration` frames are shown as they
are. Real frames are held at ~half the display's refresh rate (detected). Diagnostics
`native_frame_generation_debug` 1-4 (4: inputs over the hudless frame: the rectangle must fit).
Report: `frame-consumer.json` `frame_generation`; log every 10 s (real / shown FPS);
`logs/frame_generation.log` for the launcher.

NVIDIA DLSS Frame Generation (`dlss`, GeForce RTX 40 and newer; `vulkan_dlss_frame_generation.cpp`): the same inputs
plus the camera (`FrameGenerationInputs`: `view_to_clip`, `clip_to_previous_clip` in float64, the scene moved into the
frame's clip space by both frames' rectangles; jitter in clip space). No proxy: the regular swapchain with two more
images (usage TRANSFER). After the presenter's own pass (UI drawn) it copies the real frame, evaluates NGX into the
second image and shows the generated frame at once, the real one from a thread of its own (FIFO: at the next vblank,
else after half a real frame). History follows the frame the motion refers to (`previous_frame`), so frames
without a scene in between (cutscenes give the scene every other frame) don't break it. Linearized depth scale:
`native_dlss_frame_generation_depth_scale` (0 = near in meters / 0.2 mm); `native_dlss_frame_generation_hudless`
(off: the final frame only). The same real frame limit as AMD's. Not yet: Reflex, HDR output.

## Limits

One main view (`view_id` 0). Object motion: depth-writing triangle draws only (no particles, translucency, memexport geometry); a draw drawn several times per frame is matched by its constants.
Scene scale 1x only (other ratios need scaled-texture sampling across scales). Surfaces aliased
by the scene and other passes before `kSceneEnd` render at 1x. Frame generation: no UI resource
(hudless only). DLSS: no Ray Reconstruction or multi-frame generation.
Not available on D3D12/D3D11 or the xenos plugin.
