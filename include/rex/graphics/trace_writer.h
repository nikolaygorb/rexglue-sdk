/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Own single-frame format for ReXGlue
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <rex/graphics/registers.h>
#include <rex/graphics/trace_protocol.h>

namespace rex::graphics {

class TraceWriter {
 public:
  // physical_membase: guest physical memory, for reads given only an address.
  explicit TraceWriter(const uint8_t* physical_membase);
  ~TraceWriter();
  TraceWriter(const TraceWriter&) = delete;
  TraceWriter& operator=(const TraceWriter&) = delete;

  // The open trace, for the code reading guest memory for the GPU outside the
  // command processor. Only one trace is open at a time.
  static TraceWriter* active() { return active_; }

  bool Open(const std::filesystem::path& path, uint32_t title_id);
  void Close();
  bool is_open() const { return file_ != nullptr; }

  // dwords: the packet as in guest memory, big-endian.
  void WritePacketStart(const uint32_t* dwords, uint32_t count);
  void WritePacketEnd();
  // host_data: where to take the data from instead of guest memory, for data
  // the GPU wrote on the host.
  void WriteMemoryRead(uint32_t address, uint32_t length, const void* host_data = nullptr);
  void WriteEdramSnapshot(const void* snapshot, uint32_t size);
  void WriteRegisters(uint32_t first_register, const uint32_t* values, uint32_t count);
  void WriteGammaRamp(const reg::DC_LUT_30_COLOR* table_256_entry,
                      const reg::DC_LUT_PWL_DATA* pwl_rgb, uint32_t rw_component);
  // ucode: host-endian, written big-endian.
  void WriteShader(uint32_t shader_type, const uint32_t* ucode, uint32_t dword_count);
  void WriteSwap();

 private:
  void WriteCommand(TraceCommandType type, uint32_t address, uint32_t param, const void* data,
                    uint32_t size, const void* data_2 = nullptr, uint32_t size_2 = 0);

  inline static TraceWriter* active_ = nullptr;

  const uint8_t* physical_membase_;
  FILE* file_ = nullptr;
  std::vector<uint32_t> scratch_;
};

}  // namespace rex::graphics
