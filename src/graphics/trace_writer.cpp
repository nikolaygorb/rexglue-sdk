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

#include <rex/graphics/trace_writer.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/types.h>

namespace rex::graphics {

TraceWriter::TraceWriter(const uint8_t* physical_membase) : physical_membase_(physical_membase) {}

TraceWriter::~TraceWriter() {
  Close();
}

bool TraceWriter::Open(const std::filesystem::path& path, uint32_t title_id) {
  Close();
  auto absolute_path = std::filesystem::absolute(path);
  std::error_code error;
  std::filesystem::create_directories(absolute_path.parent_path(), error);
  file_ = rex::filesystem::OpenFile(absolute_path, "wb");
  if (!file_) {
    REXGPU_ERROR("GPU trace: failed to create {}", rex::path_to_utf8(absolute_path));
    return false;
  }
  TraceHeader header = {};
  header.magic = kTraceMagic;
  header.version = kTraceFormatVersion;
  header.title_id = title_id;
  fwrite(&header, sizeof(header), 1, file_);
  active_ = this;
  REXGPU_INFO("GPU trace: recording to {}", rex::path_to_utf8(absolute_path));
  return true;
}

void TraceWriter::Close() {
  if (!file_) {
    return;
  }
  if (active_ == this) {
    active_ = nullptr;
  }
  const bool failed = ferror(file_) != 0;
  fclose(file_);
  file_ = nullptr;
  if (failed) {
    REXGPU_ERROR("GPU trace: failed to write the file, it is incomplete");
  } else {
    REXGPU_INFO("GPU trace: done");
  }
}

void TraceWriter::WriteCommand(TraceCommandType type, uint32_t address, uint32_t param,
                               const void* data, uint32_t size, const void* data_2,
                               uint32_t size_2) {
  if (!file_) {
    return;
  }
  TraceCommand command = {type, address, param, size + size_2};
  fwrite(&command, sizeof(command), 1, file_);
  if (size) {
    fwrite(data, 1, size, file_);
  }
  if (size_2) {
    fwrite(data_2, 1, size_2, file_);
  }
  static constexpr uint8_t kPadding[4] = {};
  if (uint32_t padding = (4 - (command.size & 3)) & 3) {
    fwrite(kPadding, 1, padding, file_);
  }
}

void TraceWriter::WritePacketStart(const uint32_t* dwords, uint32_t count) {
  WriteCommand(TraceCommandType::kPacketStart, 0, 0, dwords, count * sizeof(uint32_t));
}

void TraceWriter::WritePacketEnd() {
  WriteCommand(TraceCommandType::kPacketEnd, 0, 0, nullptr, 0);
}

void TraceWriter::WriteMemoryRead(uint32_t address, uint32_t length, const void* host_data) {
  if (!file_ || !length) {
    return;
  }
  address &= 0x1FFFFFFF;
  if (length > 0x20000000 - address) {
    length = 0x20000000 - address;
  }
  WriteCommand(TraceCommandType::kMemoryRead, address, 0,
               host_data ? host_data : physical_membase_ + address, length);
}

void TraceWriter::WriteEdramSnapshot(const void* snapshot, uint32_t size) {
  WriteCommand(TraceCommandType::kEdramSnapshot, 0, 0, snapshot, size);
}

void TraceWriter::WriteRegisters(uint32_t first_register, const uint32_t* values, uint32_t count) {
  WriteCommand(TraceCommandType::kRegisters, first_register, 0, values, count * sizeof(uint32_t));
}

void TraceWriter::WriteGammaRamp(const reg::DC_LUT_30_COLOR* table_256_entry,
                                 const reg::DC_LUT_PWL_DATA* pwl_rgb, uint32_t rw_component) {
  WriteCommand(TraceCommandType::kGammaRamp, 0, rw_component, table_256_entry,
               sizeof(reg::DC_LUT_30_COLOR) * 256, pwl_rgb, sizeof(reg::DC_LUT_PWL_DATA) * 3 * 128);
}

void TraceWriter::WriteShader(uint32_t shader_type, const uint32_t* ucode, uint32_t dword_count) {
  if (!file_) {
    return;
  }
  scratch_.resize(dword_count);
  for (uint32_t i = 0; i < dword_count; ++i) {
    scratch_[i] = rex::byte_swap(ucode[i]);
  }
  WriteCommand(TraceCommandType::kShader, 0, shader_type, scratch_.data(),
               dword_count * sizeof(uint32_t));
}

void TraceWriter::WriteSwap() {
  WriteCommand(TraceCommandType::kSwap, 0, 0, nullptr, 0);
}

}  // namespace rex::graphics
