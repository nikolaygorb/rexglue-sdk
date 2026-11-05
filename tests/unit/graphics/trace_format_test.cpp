/**
 * @file        trace_format_test.cpp
 * @brief       GPU frame trace files: writing and reading back.
 *
 * @license     BSD 3-Clause License
 */

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/trace_reader.h>
#include <rex/graphics/trace_writer.h>

using namespace rex::graphics;

namespace {

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("Trace commands read back as written", "[gpu_trace]") {
  std::vector<uint8_t> memory(0x1000);
  for (size_t i = 0; i < memory.size(); ++i) {
    memory[i] = uint8_t(i * 7);
  }
  const auto path = std::filesystem::temp_directory_path() / "rex_trace_format_test.xtr";

  {
    TraceWriter writer(memory.data());
    REQUIRE(writer.Open(path, 0x4D5307E6));
    CHECK(TraceWriter::active() == &writer);
    const uint32_t packet[] = {0xC0012D00, 0x11223344, 0x55667788};
    writer.WritePacketStart(packet, 3);
    // An odd length gets padded, the next command must still be found.
    writer.WriteMemoryRead(0x103, 5);
    writer.WriteMemoryRead(0xA0000200, 4);
    writer.WritePacketEnd();
    const uint32_t ucode[] = {0x01020304, 0x05060708};
    writer.WriteShader(1, ucode, 2);
    writer.WriteSwap();
    writer.Close();
    CHECK(TraceWriter::active() == nullptr);
  }

  TraceReader reader;
  REQUIRE(reader.OpenData(ReadFile(path)));
  std::filesystem::remove(path);
  CHECK(reader.header().title_id == 0x4D5307E6);

  TraceCommand command;
  const uint8_t* data;
  REQUIRE(reader.Next(command, data));
  CHECK(command.type == TraceCommandType::kPacketStart);
  REQUIRE(command.size == 12);
  uint32_t second_dword;
  std::memcpy(&second_dword, data + 4, 4);
  CHECK(second_dword == 0x11223344);

  REQUIRE(reader.Next(command, data));
  CHECK(command.type == TraceCommandType::kMemoryRead);
  CHECK(command.address == 0x103);
  REQUIRE(command.size == 5);
  CHECK(std::memcmp(data, memory.data() + 0x103, 5) == 0);

  // Physical addresses are masked to the 512 MB of guest memory.
  REQUIRE(reader.Next(command, data));
  CHECK(command.address == 0x200);
  CHECK(std::memcmp(data, memory.data() + 0x200, 4) == 0);

  REQUIRE(reader.Next(command, data));
  CHECK(command.type == TraceCommandType::kPacketEnd);

  REQUIRE(reader.Next(command, data));
  CHECK(command.type == TraceCommandType::kShader);
  CHECK(command.param == 1);
  CHECK(data[0] == 0x01);
  CHECK(data[3] == 0x04);

  REQUIRE(reader.Next(command, data));
  CHECK(command.type == TraceCommandType::kSwap);
  CHECK_FALSE(reader.Next(command, data));
  CHECK_FALSE(reader.failed());
}

TEST_CASE("Damaged trace files are rejected", "[gpu_trace]") {
  TraceReader reader;
  CHECK_FALSE(reader.OpenData({1, 2, 3}));

  TraceHeader header = {kTraceMagic, kTraceFormatVersion + 1, 0, 0};
  std::vector<uint8_t> data(sizeof(header));
  std::memcpy(data.data(), &header, sizeof(header));
  CHECK_FALSE(reader.OpenData(data));

  header.version = kTraceFormatVersion;
  std::memcpy(data.data(), &header, sizeof(header));
  TraceCommand command = {TraceCommandType::kMemoryRead, 0, 0, 64};
  data.resize(sizeof(header) + sizeof(command) + 8);
  std::memcpy(data.data() + sizeof(header), &command, sizeof(command));
  REQUIRE(reader.OpenData(data));
  const uint8_t* payload;
  CHECK_FALSE(reader.Next(command, payload));
  CHECK(reader.failed());
}
