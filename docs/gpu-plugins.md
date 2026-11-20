# GPU plugins

A GPU plugin draws what the game sends to the console GPU. The SDK's `rexgpu-xenos`
emulates the Xenos GPU on Direct3D 12, Vulkan or Direct3D 11. A second kind of plugin, a
**native renderer**, draws the same commands directly with the host API, without
emulating the EDRAM, and keeps everything else the SDK does for the GPU: the PM4 ring,
the register file, fences, interrupts and vblanks, the window, the presenter (FSR, CAS,
letterbox, frame pacing), the ImGui overlays and GPU frame traces.

The SDK's native renderer is **`rexgpu-native`** (`src/graphics/native/`): one plugin for
every title, on **Vulkan** (Windows, Linux, macOS through MoltenVK, the Steam Deck). It is
early: it draws into host render targets, one per EDRAM surface (shaders translated by
the SDK's SPIR-V translator, vertices and indices fetched from a copy of the guest memory
on the GPU through the SDK's `SharedMemory` and `PrimitiveProcessor`, the pipeline state
from the registers as the SDK's Vulkan backend derives it, textures through the SDK's
`TextureCache` and texture load shaders, or the texture a resolve wrote to the fetched
address until the CPU writes there); surfaces sharing EDRAM get each other's contents
through per-tile ownership and transfers (single-sampled surfaces only so far); resolves copy render targets into host textures (colour with its exponent bias,
depth as its values, multisampled ones by the selected samples), write the guest's bytes into
the GPU's copy of guest memory as the emulation does (for textures read in other formats and
for the CPU), and do their clears, and
each swap presents the texture the front buffer was resolved to (or the front buffer in
guest memory when a frame resolved nothing large) through the guest's gamma ramp. Draws with memory
export and tessellated draws are skipped. The modules are being ported from
ArmyOfTwoRecompilation's renderer (MIT) one by one. This page describes what it and any other plugin are built on. It needs an SDK built
from source (`REXSDK_DIR`).

## Files and targets

| Target | File next to the game | What it is |
|---|---|---|
| `rexruntime` | `rexruntime.dll` | the runtime, as always |
| `rexgpu-xenos` | `rexgpu-xenos.dll` | GPU emulation: `plugin_main.cpp` linked with the whole of `rexgpu-core` |
| `rexgpu-native` | `rexgpu-native.dll` | the native renderer (built with `REXGLUE_USE_VULKAN`), linked with the whole of `rexgpu-core`; a title stages it with `GPU_PLUGINS xenos native` |
| `rexgpu-core` | none | static library with all of the SDK's GPU code, compiled into each plugin: `GraphicsSystem` and the Vulkan, Direct3D 12 and Direct3D 11 graphics systems, the base `CommandProcessor` (PM4 executor), the backends' command processors and caches, the shader front end (`ucode`, `Shader`, `ShaderTranslator`) and the SPIR-V and DXBC translators, `draw_util`, the texture, render target, primitive and shared memory caches. Not installed. |

Only one plugin is loaded in a process, chosen by the `gpu_plugin` setting (or
`RuntimeConfig::gpu_plugin`): `xenos` or `native`. A title ships both and lets players
fall back to the emulation. Title-specific fixes stay in the title's executable (hooks and
settings), not in the plugin.

## Building a plugin

The SDK's plugins are targets of `src/graphics/CMakeLists.txt`. A plugin outside the SDK
(an experiment, a fork) uses the helper:

```cmake
rexglue_add_gpu_plugin(native_test SOURCES
    src/gpu/plugin_main.cpp
    src/gpu/my_command_processor.cpp
)
rexglue_setup_target(mytitle GPU_PLUGINS xenos native_test)
```

- `rexglue_add_gpu_plugin(<name> SOURCES ...)` builds `rexgpu-<name>` with the SDK's
  debug postfixes (the loader looks for `rexgpu-<name>d.dll` in Debug, `rd` in
  RelWithDebInfo). It links `rexruntime` and the whole of `rexgpu-core`, so the graphics
  APIs (Vulkan headers and VMA, `d3d12`, `dxgi`) come with it.
- `GPU_PLUGINS` copies each plugin next to the executable.
- Never link `rexgpu-core` into the executable: the plugin defines the same cvars.
- On Windows the SDK builds Vulkan only with `REXGLUE_USE_VULKAN=ON`.

`plugin_main.cpp` defines the two exports of `rex/system/gpu_plugin.h`, like the SDK's
own `src/graphics/plugin_main.cpp`:

```cpp
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}
extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) return nullptr;
  return new MyGraphicsSystem();
}
```

The plugin and `rexruntime` share C++ objects and must come from the same SDK tree and
build.

## What the renderer derives from

The graphics system picks the command processor:

```cpp
class MyGraphicsSystem : public rex::graphics::vulkan::VulkanGraphicsSystem {
 public:
  std::string name() const override { return "Native (Vulkan)"; }
 protected:
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<MyCommandProcessor>(this, kernel_state_);
  }
};
```

`VulkanGraphicsSystem` makes the Vulkan provider (device, queues, VMA) and the presenter.
`D3D12GraphicsSystem` works the same way for Direct3D 12. Everything else comes from
`GraphicsSystem`: the GPU registers in guest memory, the vsync worker and vblank
interrupts, the interrupt callback, shader storage and frame traces.

The command processor derives from `rex::graphics::CommandProcessor`. The base class
consumes the ring and indirect buffers, keeps the register file, does every
synchronisation the guest waits on (scratch registers and their write-back, `COHER`,
`WAIT_REG_MEM`, `INTERRUPT`, `EVENT_WRITE*`, `MEM_WRITE`, the read-pointer write-back),
decodes the gamma ramp, handles occlusion and VIZ queries through the query interface
(fake counts when the backend has no query pool) and calls these for the work itself:

| Method | Called for | Notes |
|---|---|---|
| `SetupContext` / `ShutdownContext` | the command processor thread starting and ending | call the base version first |
| `LoadShader(type, guest_address, host_address, dword_count)` | `IM_LOAD` / `IM_LOAD_IMMEDIATE` | return a `Shader` (or a subclass) that keeps the microcode; the base reads it for traces and for the memexport check (`is_ucode_analyzed`, `memexport_eM_written`), so analyse it (`Shader::AnalyzeUcode`) |
| `IssueDraw(prim_type, index_count, index_buffer_info, major_mode_explicit)` | `DRAW_INDX`, `DRAW_INDX_2` | `RB_MODECONTROL.edram_mode == kCopy` means a resolve: call `IssueCopy` |
| `IssueCopy` | resolves | the `RB_COPY_*` registers describe it (`draw_util::GetResolveInfo`) |
| `IssueSwap(frontbuffer_ptr, width, height)` | `XE_SWAP` | call `EndZPDFrame()`, then hand the frame to `graphics_system_->presenter()->RefreshGuestOutput` as `VulkanCommandProcessor::IssueSwap` does |
| `TracePlaybackWroteMemory(base, length)` | trace replay writing guest memory | invalidate what the renderer cached from that range |

Optional overrides: `WriteRegister` (constant uploads), `OnPrimaryBufferEnd`,
`PrepareForWait` / `ReturnFromWait` (submit work before the guest waits), the query pool
interface (`IsQueryPoolReady`, `OpenQuery`, `CloseQuery`, `PumpQueryResolves`,
`AwaitQueryResolve`) for real occlusion queries (`VulkanQueryPool` with a command buffer of your
own: `src/graphics/native/command_processor.cpp`), `InitializeShaderStorage`,
`RestoreEdramSnapshot`, `GetWindowTitleText`.

A command processor without host occlusion queries should set `zpd_mode_ =
ZPDMode::kFake` after the base `SetupContext`: the other modes queue reports that only a
host query pool retires.

The smallest complete one is `NullCommandProcessor`
(`rex/graphics/null_command_processor.h`, `src/graphics/null_command_processor.cpp`): it
keeps and analyses shaders, drops draws and resolves, answers VIZ surveys and occlusion
queries with fake results and presents nothing. `VulkanNullGraphicsSystem`
(`rex/graphics/vulkan/null_graphics_system.h`) and `D3D12NullGraphicsSystem` are the
matching graphics systems, selected in `rexgpu-xenos` with the backend `"null"`
([graphics options](graphics-options.md)). A new renderer can start as a copy of
`NullCommandProcessor` and `VulkanNullGraphicsSystem` and fill in `IssueDraw`, `IssueCopy`
and `IssueSwap`; running a game on the null backend first shows that the game itself runs
without GPU emulation and what its CPU side costs.

The SDK's command processors are the reference for each of these:
`src/graphics/vulkan/command_processor.cpp` (the same API),
`src/graphics/d3d12/command_processor.cpp` (the most complete).

## Shaders

The translators in `rexgpu-core` take the analysed `Shader` and a modification
(`Shader::CreateTranslationInstance`, `ShaderTranslator::TranslateAnalyzedShader`):

- `SpirvShaderTranslator` (`rex/graphics/pipeline/shader/spirv_translator.h`) writes SPIR-V
  for Vulkan, the native renderer's translator. `src/graphics/vulkan/pipeline_cache.cpp`
  shows how modifications are chosen, how bindings are read from the translation, and the
  paths for devices without geometry shaders (MoltenVK): rectangle lists expanded in the
  vertex shader (`kRectangleListAsTriangleStrip`).
- `DxbcShaderTranslator` (`rex/graphics/pipeline/shader/dxbc_translator.h`) writes DXBC for
  Direct3D 12 (and, rewritten to Shader Model 5.0, Direct3D 11).

Both follow xenia-canary and get its fixes with every SDK update. A translator written for
one title (an HLSL emitter of its own, XenosRecomp) has to repeat them, and needs a
shader compiler at run time.

## Checking a renderer with GPU traces

[GPU frame traces](gpu-traces.md) replay through `GraphicsSystem`, so they work with any
plugin: the base command processor feeds the recorded frame to the renderer and the
replay saves what reached the presenter.

1. Record traces with the emulation (`gpu_plugin = "xenos"`, `resolution_scale` 1, F10 in
   the game).
2. Save the emulated frames as references:
   `python tools/gpu_traces/compare.py game.exe D:/traces --label xenos --update -- --gpu_plugin=xenos`.
3. Compare the native renderer with them:
   `python tools/gpu_traces/compare.py game.exe D:/traces --label xenos -- --gpu_plugin=native`.
   Raise `trace_replay_tolerance` / `trace_replay_max_different_percent` where the renderer
   is meant to differ (another resolve or filter path).

For this the renderer must implement `TracePlaybackWroteMemory` and present in `IssueSwap`.
The EDRAM snapshot of a trace is restored only by renderers that override
`RestoreEdramSnapshot`; frames that read render targets of an earlier frame need it.
