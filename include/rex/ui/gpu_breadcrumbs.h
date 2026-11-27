#pragma once

#include <cinttypes>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

namespace rex::ui {

// Commands recorded since the previous draw or resolve in the main command
// stream (hoisted uploads run before the render pass and are not counted). For
// a draw: before the draw command itself; for a resolve: with its own commands
// (FH1-ReImagined ReIGlue 1100).
struct GpuBreadcrumbWork {
  uint16_t render_passes = 0;  // render pass or dynamic rendering begins
  uint16_t draws = 0;          // host draws (EDRAM transfers, clears drawn, effects)
  uint16_t dispatches = 0;     // compute (texture loads, resolves, effects)
  uint16_t barriers = 0;
  uint16_t copies = 0;       // buffer and image copies, blits, resolves, fills, image clears
  uint16_t clears = 0;       // attachment clears in the render pass
  uint16_t queries = 0;      // occlusion query begins
  uint16_t conditional = 0;  // conditional rendering begins
  bool Any() const {
    return render_passes || draws || dispatches || barriers || copies || clears || queries ||
           conditional;
  }
};

// What `GpuBreadcrumbDetails::render_pass` holds.
enum class GpuRenderPassKind : uint8_t {
  kNone,
  // VulkanRenderTargetCache::RenderPassKey::key.
  kXenosVulkan,
  // PackNativeRenderPass: sample count, depth and colour VkFormats (low 8 bits).
  kNative,
};

// What a draw was drawn with besides its shaders, and the work recorded for it.
struct GpuBreadcrumbDetails {
  uint64_t vertex_shader_modification = 0;
  uint64_t pixel_shader_modification = 0;
  uint64_t render_pass = 0;
  GpuRenderPassKind render_pass_kind = GpuRenderPassKind::kNone;
  GpuBreadcrumbWork work;
  // A marker written once `work` finished, before the draw (second marker).
  bool setup_marked = false;
  // Recorded with these (entries without them print only the first lines).
  bool filled = false;
};

struct GpuBreadcrumb {
  uint32_t sequence = 0;
  uint64_t submission = 0;
  uint64_t frame = 0;
  uint64_t vertex_shader = 0;
  uint64_t pixel_shader = 0;
  uint32_t vertices = 0;
  bool resolve = false;
  GpuBreadcrumbDetails details;
};

// Record on the CP thread, inspect from whichever thread first sees device loss.
class GpuBreadcrumbTrail {
 public:
  static constexpr size_t kCapacity = 16384;
  GpuBreadcrumbTrail() : entries_(kCapacity) {}
  uint32_t Record(GpuBreadcrumb entry) {
    std::lock_guard lock(mutex_);
    if (!++sequence_)
      ++sequence_;
    entry.sequence = sequence_;
    entries_[sequence_ % kCapacity] = entry;
    return sequence_;
  }
  std::optional<GpuBreadcrumb> Find(uint32_t sequence) const {
    std::lock_guard lock(mutex_);
    const auto& entry = entries_[sequence % kCapacity];
    return sequence && entry.sequence == sequence ? std::optional(entry) : std::nullopt;
  }
  // The number the next Record gives (one recording thread: the CP's), for
  // the marker written before a draw is recorded.
  uint32_t Next() const {
    std::lock_guard lock(mutex_);
    return sequence_ + 1 ? sequence_ + 1 : 1;
  }
  uint32_t Last() const {
    std::lock_guard lock(mutex_);
    return sequence_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<GpuBreadcrumb> entries_;
  uint32_t sequence_ = 0;
};

// The native renderer's render pass (Pipelines::RenderPassKey) in 64 bits:
// samples in bits 0-7, depth VkFormat 8-15, colour 0-3 VkFormats 16-47 (core
// formats, all below 256).
inline uint64_t PackNativeRenderPass(uint32_t samples, uint32_t depth_format,
                                     const uint32_t color_formats[4]) {
  uint64_t key = (samples & 0xFF) | (uint64_t(depth_format & 0xFF) << 8);
  for (uint32_t i = 0; i < 4; ++i) {
    key |= uint64_t(color_formats[i] & 0xFF) << (16 + 8 * i);
  }
  return key;
}

inline std::string DescribeGpuRenderPass(GpuRenderPassKind kind, uint64_t key) {
  if (kind == GpuRenderPassKind::kXenosVulkan) {
    static const char* const kSamples[] = {"1x", "2x", "4x", "?x"};
    static const char* const kColor[16] = {"8_8_8_8",
                                           "8_8_8_8_GAMMA",
                                           "2_10_10_10",
                                           "2_10_10_10_FLOAT",
                                           "16_16",
                                           "16_16_16_16",
                                           "16_16_FLOAT",
                                           "16_16_16_16_FLOAT",
                                           "8",
                                           "9",
                                           "2_10_10_10_AS_10_10_10_10",
                                           "11",
                                           "2_10_10_10_FLOAT_AS_16_16_16_16",
                                           "13",
                                           "32_FLOAT",
                                           "32_32_FLOAT"};
    const uint32_t used = uint32_t(key >> 2) & 0x1F;
    std::string out = fmt::format("render pass 0x{:08X} ({}", uint32_t(key), kSamples[key & 3]);
    if (used & 1) {
      out += (key >> 7) & 1 ? ", depth D24FS8" : ", depth D24S8";
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (used & (2u << i)) {
        out += fmt::format(", color {} {}", i, kColor[(key >> (8 + 4 * i)) & 0xF]);
      }
    }
    if (!used) {
      out += ", no attachments";
    }
    return out + ")";
  }
  if (kind == GpuRenderPassKind::kNative) {
    std::string out = fmt::format("render pass ({}x", uint32_t(key & 0xFF));
    if ((key >> 8) & 0xFF) {
      out += fmt::format(", depth VkFormat {}", uint32_t((key >> 8) & 0xFF));
    }
    bool any_color = false;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t format = uint32_t(key >> (16 + 8 * i)) & 0xFF;
      if (format) {
        out += fmt::format(", color {} VkFormat {}", i, format);
        any_color = true;
      }
    }
    if (!any_color && !((key >> 8) & 0xFF)) {
      out += ", no attachments";
    }
    return out + ")";
  }
  return "render pass unknown";
}

inline std::string DescribeGpuBreadcrumbWork(const GpuBreadcrumbWork& w) {
  if (!w.Any()) {
    return "nothing";
  }
  std::string out;
  const auto part = [&out](uint32_t n, const char* one, const char* many) {
    if (n) {
      out += fmt::format("{}{} {}", out.empty() ? "" : ", ", n, n == 1 ? one : many);
    }
  };
  part(w.render_passes, "render pass", "render passes");
  part(w.draws, "draw", "draws");
  part(w.dispatches, "dispatch", "dispatches");
  part(w.barriers, "barrier", "barriers");
  part(w.copies, "copy", "copies");
  part(w.clears, "attachment clear", "attachment clears");
  part(w.queries, "query", "queries");
  part(w.conditional, "conditional rendering begin", "conditional rendering begins");
  return out;
}

// The line after the first unfinished breadcrumb: what that draw was drawn
// with, the work recorded for it before the draw command, and whether the GPU
// finished that work. finished: the last breadcrumb the GPU finished;
// setup_finished: the last draw whose work before it the GPU finished (the
// second marker; 0: none, or no such marker, as with NV checkpoints). Empty
// when nothing is unfinished or the entry has no details.
inline std::string DescribeGpuBreadcrumbDetails(const GpuBreadcrumbTrail& trail, uint32_t finished,
                                                uint32_t setup_finished) {
  if (finished == trail.Last()) {
    return std::string();
  }
  const uint32_t next = finished + 1 ? finished + 1 : 1;
  const std::optional<GpuBreadcrumb> entry = trail.Find(next);
  if (!entry || !entry->details.filled) {
    return std::string();
  }
  const GpuBreadcrumbDetails& d = entry->details;
  std::string out = fmt::format("GPU breadcrumbs, unfinished #{} ({}): ", next,
                                entry->resolve ? "resolve" : "draw");
  if (!entry->resolve) {
    out += fmt::format("VS modification {:016X}, PS modification {:016X}, ",
                       d.vertex_shader_modification, d.pixel_shader_modification);
    out += DescribeGpuRenderPass(d.render_pass_kind, d.render_pass);
    out += "; recorded before the draw command: " + DescribeGpuBreadcrumbWork(d.work);
  } else {
    out += "recorded with it: " + DescribeGpuBreadcrumbWork(d.work);
  }
  if (d.setup_marked) {
    out += setup_finished == next
               ? "; the GPU finished that work: it hung in the draw itself"
               : "; the GPU did not finish that work: it hung in it, not in the draw";
  } else if (!entry->resolve && !d.work.Any()) {
    out += fmt::format("; nothing else was recorded after #{}: it hung in the draw itself",
                       finished);
  } else if (!entry->resolve) {
    out += "; in that work or in the draw: gpu_device_lost_setup_breadcrumbs tells them apart";
  }
  if (const std::optional<GpuBreadcrumb> last = trail.Find(finished);
      last && last->details.filled && !last->resolve &&
      (last->details.render_pass != d.render_pass ||
       last->details.render_pass_kind != d.render_pass_kind)) {
    out += "; last finished in " +
           DescribeGpuRenderPass(last->details.render_pass_kind, last->details.render_pass);
  }
  return out;
}

}  // namespace rex::ui
