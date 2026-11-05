/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Own single-frame format for ReXGlue
 */

#include <rex/graphics/trace_player.h>

#include <cstring>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/trace_reader.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/thread.h>

namespace rex::graphics {

bool TracePlayer::Play(const std::filesystem::path& path) {
  TraceReader reader;
  if (!reader.Open(path)) {
    return false;
  }
  CommandProcessor* command_processor = graphics_system_->command_processor();
  if (!command_processor) {
    return false;
  }
  // Pipelines compiled in the background would leave their draws out of the
  // frame, differently from run to run.
  REXCVAR_SET(async_shader_compilation, false);

  auto done = rex::thread::Event::CreateManualResetEvent(false);
  bool played = false;
  command_processor->CallInThread([&]() {
    played = PlayOnWorkerThread(reader);
    done->Set();
  });
  rex::thread::Wait(done.get(), false);
  return played;
}

bool TracePlayer::PlayOnWorkerThread(TraceReader& reader) {
  memory::Memory* memory = graphics_system_->memory();
  CommandProcessor* command_processor = graphics_system_->command_processor();
  command_processor->set_trace_playback(true);

  const uint8_t* packet = nullptr;
  uint32_t packet_dwords = 0;
  bool swapped = false;
  TraceCommand command;
  const uint8_t* data;
  while (!swapped && reader.Next(command, data)) {
    switch (command.type) {
      case TraceCommandType::kPacketStart:
        packet = data;
        packet_dwords = command.size / sizeof(uint32_t);
        break;
      case TraceCommandType::kPacketEnd:
        if (packet && packet_dwords) {
          command_processor->ExecutePacket(packet, packet_dwords);
        }
        packet = nullptr;
        break;
      case TraceCommandType::kMemoryRead:
        if (command.size && command.address < 0x20000000 &&
            command.size <= 0x20000000 - command.address) {
          std::memcpy(memory->TranslatePhysical(command.address), data, command.size);
          command_processor->TracePlaybackWroteMemory(command.address, command.size);
        }
        break;
      case TraceCommandType::kEdramSnapshot:
        command_processor->RestoreEdramSnapshot(data, command.size);
        break;
      case TraceCommandType::kRegisters:
        command_processor->RestoreRegisters(command.address,
                                            reinterpret_cast<const uint32_t*>(data),
                                            command.size / sizeof(uint32_t));
        break;
      case TraceCommandType::kGammaRamp: {
        constexpr uint32_t kTableSize = sizeof(reg::DC_LUT_30_COLOR) * 256;
        if (command.size == kTableSize + sizeof(reg::DC_LUT_PWL_DATA) * 3 * 128) {
          command_processor->RestoreGammaRamp(
              reinterpret_cast<const reg::DC_LUT_30_COLOR*>(data),
              reinterpret_cast<const reg::DC_LUT_PWL_DATA*>(data + kTableSize), command.param);
        }
      } break;
      case TraceCommandType::kShader:
        command_processor->RestoreShader(xenos::ShaderType(command.param),
                                         reinterpret_cast<const uint32_t*>(data),
                                         command.size / sizeof(uint32_t));
        break;
      case TraceCommandType::kSwap:
        swapped = true;
        break;
      default:
        REXGPU_WARN("GPU trace: unknown command {}", uint32_t(command.type));
        break;
    }
  }

  command_processor->set_trace_playback(false);
  if (reader.failed()) {
    REXGPU_ERROR("GPU trace: the file is truncated");
    return false;
  }
  if (!swapped) {
    REXGPU_ERROR("GPU trace: the file ends before the swap");
  }
  return swapped;
}

}  // namespace rex::graphics
