/**
 * @file        rex/graphics/trace_reader.h
 * @brief       Reads the commands of a GPU frame trace in order.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <rex/graphics/trace_protocol.h>

namespace rex::graphics {

class TraceReader {
 public:
  // Reads the whole file. False if it can't be read or isn't a trace of this
  // format version.
  bool Open(const std::filesystem::path& path);
  // For tests: the file contents.
  bool OpenData(std::vector<uint8_t> data);

  const TraceHeader& header() const { return header_; }

  // The next command and its data, valid while the reader lives. False at the
  // end of the file, or at a truncated command (then failed() is true).
  bool Next(TraceCommand& command_out, const uint8_t*& data_out);
  bool failed() const { return failed_; }

 private:
  std::vector<uint8_t> data_;
  size_t position_ = 0;
  TraceHeader header_ = {};
  bool failed_ = false;
};

}  // namespace rex::graphics
