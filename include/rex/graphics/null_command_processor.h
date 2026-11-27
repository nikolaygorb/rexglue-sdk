/**
 * @file        graphics/null_command_processor.h
 * @brief       Command processor that services the ring without host GPU work
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Idea from EddieVanHelgen/rexglue-sdk adb783a (PerfectDarkZeroRestored, null
 * GPU backend); written for this tree.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>

namespace rex::graphics {

// The base CommandProcessor - ring and indirect buffers, register file and the
// scratch write-back, COHER, WAIT_REG_MEM, INTERRUPT, EVENT_WRITE*, MEM_WRITE,
// the read-pointer write-back, the gamma ramp, XE_SWAP - with nothing done on
// the host GPU:
// - shaders are kept and analysed (the base reads their microcode for frame
//   traces and for the memexport check) but never translated;
// - draws and resolves are counted and dropped; VIZ surveys count as
//   unmeasured (visible);
// - occlusion queries are fake (occlusion_query is ignored: the other modes
//   queue reports only a host query pool retires);
// - the swap hands nothing to the presenter.
//
// Selected with graphics_backend "null" (vulkan/null_graphics_system.h, or
// d3d12/null_graphics_system.h without Vulkan). Use:
// what the title's CPU side costs without GPU emulation; and the smallest
// complete command processor, the starting point of a title's own renderer
// (docs/gpu-plugins.md).
class NullCommandProcessor : public CommandProcessor {
 public:
  NullCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~NullCommandProcessor() override;

  std::string GetWindowTitleText() const override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;
  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

  // A backend built on this one clears it and describes itself in the log.
  bool log_null_backend_ = true;

 private:
  // Per xenos::ShaderType (vertex, pixel), by the XXH3 of the guest
  // (big-endian) microcode; command processor thread only.
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_[2];
  string::StringBuffer ucode_disasm_buffer_;

  // Command processor thread only.
  uint64_t swaps_ = 0;
  uint64_t frame_draws_ = 0;
  uint64_t frame_copies_ = 0;
};

}  // namespace rex::graphics
