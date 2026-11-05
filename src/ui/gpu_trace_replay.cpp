/**
 * @file        ui/gpu_trace_replay.cpp
 * @brief       Draws a GPU frame trace instead of running the title, saves
 *              the frame as PNG and compares it with a reference image.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/ui/gpu_trace_replay.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_STRING(trace_replay, "", "GPU/Trace",
                      "A GPU frame trace (.xtr) to draw instead of running the title; the "
                      "process exits after it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(trace_replay_output, "", "GPU/Trace",
                      "PNG to save the replayed frame to (empty: beside the trace)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(trace_replay_reference, "", "GPU/Trace",
                      "Reference PNG to compare the replayed frame with; a difference makes the "
                      "exit code 1 and writes a .diff.png")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(trace_replay_tolerance, 2, "GPU/Trace",
                     "How much a color channel may differ from the reference before the pixel "
                     "counts as different")
    .range(0, 255)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(trace_replay_max_different_percent, 0.0, "GPU/Trace",
                      "Share of different pixels, in percent, still accepted as matching")
    .range(0.0, 100.0)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::ui {

namespace {

bool WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  return bool(file);
}

}  // namespace

bool IsGpuTraceReplay() {
  return !REXCVAR_GET(trace_replay).empty();
}

int RunGpuTraceReplay(system::IGraphicsSystem& graphics_system) {
  const auto trace_path = rex::to_path(REXCVAR_GET(trace_replay));
  REXLOG_INFO("GPU trace replay: {}", rex::path_to_utf8(trace_path));
  if (!graphics_system.PlayFrameTrace(trace_path)) {
    REXLOG_ERROR("GPU trace replay: the frame wasn't drawn");
    return 2;
  }

  // The swap may reach the presenter a little after the command processor is
  // done with it.
  Presenter* presenter = graphics_system.presenter();
  RawImage image;
  bool captured = false;
  for (int attempt = 0; presenter && attempt < 500 && !captured; ++attempt) {
    captured = presenter->CaptureGuestOutput(image);
    if (!captured) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  if (!captured) {
    REXLOG_ERROR("GPU trace replay: the frame didn't reach the presenter");
    return 2;
  }
  const std::vector<uint8_t> frame =
      PackOpaqueRGBA(image.data.data(), image.width, image.height, image.stride);

  auto output_path = rex::to_path(REXCVAR_GET(trace_replay_output));
  if (output_path.empty()) {
    output_path = trace_path;
    output_path.replace_extension(".png");
  }
  if (!WriteFile(output_path, EncodeImagePNG(frame.data(), image.width, image.height))) {
    REXLOG_ERROR("GPU trace replay: can't write {}", rex::path_to_utf8(output_path));
    return 2;
  }
  REXLOG_INFO("GPU trace replay: {}x{} frame saved to {}", image.width, image.height,
              rex::path_to_utf8(output_path));

  const auto reference_path = rex::to_path(REXCVAR_GET(trace_replay_reference));
  if (reference_path.empty()) {
    return 0;
  }
  std::ifstream reference_file(reference_path, std::ios::binary);
  const std::vector<uint8_t> reference_png((std::istreambuf_iterator<char>(reference_file)),
                                           std::istreambuf_iterator<char>());
  int reference_width = 0, reference_height = 0;
  const std::vector<uint8_t> reference = DecodeImageRGBA(reference_png.data(), reference_png.size(),
                                                         reference_width, reference_height);
  if (reference.empty()) {
    REXLOG_ERROR("GPU trace replay: can't read the reference {}",
                 rex::path_to_utf8(reference_path));
    return 1;
  }
  if (uint32_t(reference_width) != image.width || uint32_t(reference_height) != image.height) {
    REXLOG_ERROR("GPU trace replay: the frame is {}x{}, the reference {}x{}", image.width,
                 image.height, reference_width, reference_height);
    return 1;
  }
  std::vector<uint8_t> diff;
  const uint64_t different =
      CompareImagesRGBA(frame.data(), reference.data(), image.width, image.height,
                        uint32_t(REXCVAR_GET(trace_replay_tolerance)), &diff);
  const double percent = 100.0 * double(different) / double(uint64_t(image.width) * image.height);
  if (percent <= REXCVAR_GET(trace_replay_max_different_percent)) {
    REXLOG_INFO("GPU trace replay: matches the reference ({} pixels differ)", different);
    return 0;
  }
  auto diff_path = output_path;
  diff_path.replace_extension(".diff.png");
  WriteFile(diff_path, EncodeImagePNG(diff.data(), image.width, image.height));
  REXLOG_ERROR("GPU trace replay: {} pixels ({:.3f}%) differ from the reference, see {}", different,
               percent, rex::path_to_utf8(diff_path));
  return 1;
}

}  // namespace rex::ui
