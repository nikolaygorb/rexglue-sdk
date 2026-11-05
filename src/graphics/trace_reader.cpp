/**
 * @file        graphics/trace_reader.cpp
 * @brief       Reads the commands of a GPU frame trace in order.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/graphics/trace_reader.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include <rex/logging.h>
#include <rex/filesystem.h>

namespace rex::graphics {

bool TraceReader::Open(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    REXGPU_ERROR("GPU trace: can't open {}", rex::path_to_utf8(path));
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
  return OpenData(std::move(data));
}

bool TraceReader::OpenData(std::vector<uint8_t> data) {
  data_ = std::move(data);
  position_ = sizeof(TraceHeader);
  failed_ = false;
  if (data_.size() < sizeof(TraceHeader)) {
    REXGPU_ERROR("GPU trace: not a trace file");
    return false;
  }
  std::memcpy(&header_, data_.data(), sizeof(header_));
  if (header_.magic != kTraceMagic) {
    REXGPU_ERROR("GPU trace: not a trace file");
    return false;
  }
  if (header_.version != kTraceFormatVersion) {
    REXGPU_ERROR("GPU trace: format version {}, this build reads {}", header_.version,
                 kTraceFormatVersion);
    return false;
  }
  return true;
}

bool TraceReader::Next(TraceCommand& command_out, const uint8_t*& data_out) {
  if (failed_ || position_ >= data_.size()) {
    return false;
  }
  if (data_.size() - position_ < sizeof(TraceCommand)) {
    failed_ = true;
    return false;
  }
  std::memcpy(&command_out, data_.data() + position_, sizeof(command_out));
  position_ += sizeof(TraceCommand);
  const size_t padded_size = (size_t(command_out.size) + 3) & ~size_t(3);
  if (data_.size() - position_ < command_out.size) {
    failed_ = true;
    return false;
  }
  data_out = data_.data() + position_;
  position_ = std::min(data_.size(), position_ + padded_size);
  return true;
}

}  // namespace rex::graphics
