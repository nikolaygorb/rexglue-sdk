/**
 * @file        graphics/native/shaders.h
 * @brief       Native renderer: guest shaders translated to SPIR-V
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              The modifications follow the SDK's VulkanPipelineCache
 *              (GetCurrentVertexShaderModification, GetCurrentPixelShaderModification;
 *              Xenia, Copyright 2022 Ben Vanik, BSD license) for host render targets.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/string/buffer.h>

namespace rex::graphics::native {

// The SDK's SpirvShaderTranslator in its host render target mode (no fragment
// shader interlock, no EDRAM buffer), at the draw resolution scale given to
// Initialize. Shaders are the SDK's VulkanShader: the translated bindings
// are the translator's (set 0 shared memory, set 1 the constant buffers, sets
// 2 and 3 the vertex and pixel textures with their samplers after them).
// Translation happens on the first draw of each modification: on a
// background thread with its own translator when the caller allows it (the
// draw is skipped until it is done, as pipelines), else on the command
// processor thread. Stored translations are made in parallel at start
// (TranslateBatch).
class Shaders {
 public:
  Shaders() = default;
  Shaders(const Shaders&) = delete;
  Shaders& operator=(const Shaders&) = delete;
  ~Shaders();

  // scale4: the draw resolution scale the translations are for, in quarters
  // (4 = 1x, 6 = 1.5x), along both axes.
  bool Initialize(const ui::vulkan::VulkanDevice* vulkan_device, uint32_t scale4 = 4);
  // Before Initialize: vertex shaders capture their positions for object
  // motion (SpirvShaderTranslator::SetMotionCapture).
  void SetMotionCapture(bool enabled) { motion_capture_ = enabled; }
  // After Initialize: capture is translated in (the device can store from
  // vertex shaders).
  bool motion_capture() const { return translator_ && translator_->motion_capture(); }
  void Shutdown();

  const SpirvShaderTranslator& translator() const { return *translator_; }

  // The shader with this microcode (host_address: big-endian, as in guest
  // memory), created and analysed on first use.
  vulkan::VulkanShader* Load(xenos::ShaderType shader_type, const uint32_t* host_address,
                             uint32_t dword_count);
  // A shader loaded before, by the hash of its microcode; nullptr if none.
  vulkan::VulkanShader* Find(xenos::ShaderType shader_type, uint64_t ucode_data_hash) const;

  SpirvShaderTranslator::Modification GetVertexModification(
      const RegisterFile& regs, const Shader& shader,
      Shader::HostVertexShaderType host_vertex_shader_type, uint32_t interpolator_mask,
      bool ps_param_gen_used) const;
  // depth_float24_convert: depth_float24_convert_in_pixel_shader where the
  // device can (sampleRateShading), for a D24FS8 depth test.
  SpirvShaderTranslator::Modification GetPixelModification(
      const RegisterFile& regs, const Shader& shader, uint32_t interpolator_mask,
      uint32_t param_gen_pos, reg::RB_DEPTHCONTROL normalized_depth_control,
      bool polygon_offset_in_shader, bool depth_float24_convert = false) const;
  // For draws without a pixel shader under depth_float24_convert_in_pixel_shader:
  // the translator's depth-only shader converting to float24 (truncating or
  // rounding), so the depth buffer holds one encoding. Made on first use;
  // VK_NULL_HANDLE on failure.
  VkShaderModule GetDepthOnlyFloat24Shader(bool round);

  // The translation with the module created; nullptr if it failed (logged
  // once per translation, never retried). allow_async: one not made yet goes
  // to the background thread, waited for up to wait_ms, then nullptr with
  // pending_out set.
  vulkan::VulkanShader::VulkanTranslation* Translate(vulkan::VulkanShader& shader,
                                                    uint64_t modification,
                                                    bool allow_async = false,
                                                    uint32_t wait_ms = 0,
                                                    bool* pending_out = nullptr);
  // Translations made in parallel now, one thread per shader at a time (each
  // with its own translator), as the SDK's backends do with stored ones.
  void TranslateBatch(const std::vector<std::pair<vulkan::VulkanShader*, uint64_t>>& translations);
  // Until the background thread has nothing to translate.
  void AwaitIdle();

 private:
  std::unique_ptr<SpirvShaderTranslator> CreateTranslator() const;
  // Translates and creates the module, with the given translator.
  static bool TranslateNow(SpirvShaderTranslator& translator,
                           vulkan::VulkanShader::VulkanTranslation& translation);
  void WorkerMain();
  void StopWorker();

  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;
  bool depth_only_float24_attempted_[2] = {};
  VkShaderModule depth_only_float24_[2] = {};
  uint32_t scale4_ = 4;
  bool motion_capture_ = false;
  std::unique_ptr<SpirvShaderTranslator> translator_;

  // Background translation. The command processor thread creates the
  // translation objects (the shader's map) and reads one only once its done
  // flag is set; the worker writes only the translations queued, one at a
  // time (a shader's first translation sets its bindings).
  struct AsyncTranslation {
    std::atomic<bool> done{false};
    bool succeeded = false;
  };
  struct TranslationJob {
    vulkan::VulkanShader::VulkanTranslation* translation;
    AsyncTranslation* state;
  };
  std::unordered_map<const void*, std::unique_ptr<AsyncTranslation>> async_translations_;
  std::thread worker_;
  std::mutex jobs_mutex_;
  std::condition_variable jobs_cv_;
  std::deque<TranslationJob> jobs_;
  bool worker_busy_ = false;
  bool worker_exit_ = false;
  std::condition_variable done_cv_;
  // Per xenos::ShaderType (vertex, pixel), by the XXH3 of the microcode.
  std::unordered_map<uint64_t, std::unique_ptr<vulkan::VulkanShader>> shaders_[2];
  string::StringBuffer ucode_disasm_buffer_;
};

}  // namespace rex::graphics::native
