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

#pragma once

#include <filesystem>

namespace rex::graphics {

class GraphicsSystem;
class TraceReader;

// Replays a frame trace on the command processor of a graphics system set up
// without a title running.
class TracePlayer {
 public:
  explicit TracePlayer(GraphicsSystem* graphics_system) : graphics_system_(graphics_system) {}

  // Blocks until the frame is drawn. False if the trace couldn't be read or
  // ended before its swap.
  bool Play(const std::filesystem::path& path);

 private:
  bool PlayOnWorkerThread(TraceReader& reader);

  GraphicsSystem* graphics_system_;
};

}  // namespace rex::graphics
