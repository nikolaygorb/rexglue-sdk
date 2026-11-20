/**
 * @file        graphics/null_command_processor.cpp
 * @brief       Command processor that services the ring without host GPU work
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Idea from EddieVanHelgen/rexglue-sdk adb783a (PerfectDarkZeroRestored, null
 * GPU backend); written for this tree.
 */

#include <rex/graphics/null_command_processor.h>

#include <xxhash.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>

namespace rex::graphics {

NullCommandProcessor::NullCommandProcessor(GraphicsSystem* graphics_system,
                                           system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

NullCommandProcessor::~NullCommandProcessor() = default;

std::string NullCommandProcessor::GetWindowTitleText() const {
  return "Null (no GPU emulation)";
}

bool NullCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    return false;
  }
  // No host query pool: the fast, strict and async modes queue reports that
  // only host query resolves retire (strict would make the guest wait on
  // them), so the fake counter walk is the only mode that answers.
  zpd_mode_ = ZPDMode::kFake;
  if (log_null_backend_) {
    REXGPU_INFO(
        "Null GPU backend: the ring and its synchronisation are serviced, draws, resolves and "
        "shaders do no host GPU work, occlusion queries are fake, nothing is presented");
  }
  return true;
}

void NullCommandProcessor::ShutdownContext() {
  for (auto& shaders : shaders_) {
    shaders.clear();
  }
  CommandProcessor::ShutdownContext();
}

Shader* NullCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                         uint32_t /*guest_address*/,
                                         const uint32_t* host_address, uint32_t dword_count) {
  const uint64_t hash = XXH3_64bits(host_address, size_t(dword_count) * sizeof(uint32_t));
  auto& shaders = shaders_[shader_type == xenos::ShaderType::kPixel ? 1 : 0];
  auto it = shaders.find(hash);
  if (it != shaders.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
  // Analysed so the base sees which shaders memexport (an unanalysed shader
  // counts as one that may) and frame traces get the real microcode.
  shader->AnalyzeUcode(ucode_disasm_buffer_);
  Shader* result = shader.get();
  shaders.emplace(hash, std::move(shader));
  return result;
}

bool NullCommandProcessor::IssueDraw(xenos::PrimitiveType /*prim_type*/,
                                     uint32_t /*index_count*/,
                                     IndexBufferInfo* /*index_buffer_info*/,
                                     bool /*major_mode_explicit*/) {
  const RegisterFile& regs = *register_file_;
  if (draw_util::IsVIZSurveyDraw(regs)) {
    // Not measured: the query falls back to visible.
    OnVIZSurveyDraw(false);
    return true;
  }
  if (regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
  ++frame_draws_;
  return true;
}

bool NullCommandProcessor::IssueCopy() {
  // No EDRAM: guest memory keeps what it had at the resolve destination.
  ++frame_copies_;
  return true;
}

void NullCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                     uint32_t frontbuffer_height) {
  EndZPDFrame();
  if (!swaps_) {
    REXGPU_INFO(
        "Null GPU backend: first swap (front buffer {:08X}, {}x{}), {} draws and {} resolves "
        "dropped",
        frontbuffer_ptr, frontbuffer_width, frontbuffer_height, frame_draws_, frame_copies_);
  }
  ++swaps_;
  frame_draws_ = 0;
  frame_copies_ = 0;
}

void NullCommandProcessor::TracePlaybackWroteMemory(uint32_t /*base_ptr*/,
                                                    uint32_t /*length*/) {
  // Nothing is cached from guest memory.
}

}  // namespace rex::graphics
