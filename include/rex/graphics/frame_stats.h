/**
 * @file        rex/graphics/frame_stats.h
 * @brief       Frame pacing statistics between guest swaps, for the log
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

namespace rex::graphics {

// What the renderer did over a frame_stats_interval window, summed; the
// command processor divides it by the window's frames
// (CollectRendererFrameCounters).
struct RendererFrameCounters {
  // The renderer fills these in (otherwise only the command processor's own
  // figures are logged).
  bool reported = false;
  // Graphics card time of the command buffers that completed in the window,
  // from timestamps at their start and end, when gpu_timed.
  bool gpu_timed = false;
  uint64_t gpu_ns = 0;
  uint64_t submissions = 0;
  // Render passes begun for the guest's draws.
  uint64_t draw_render_passes = 0;
  // Graphics pipelines bound for the guest's draws.
  uint64_t draw_pipeline_binds = 0;
  // The window's frames, set by the command processor before collecting (0
  // when the counts are dropped), for the renderer's own per-frame figures.
  uint64_t frames = 0;
  // The renderer's own figures, appended to its line (empty: none).
  std::string detail;
};

// Collects the time between guest frame swaps and summarizes a window of them
// (frame_stats_interval). Not thread-safe: the command processor thread owns
// it.
class FrameStats {
 public:
  struct Summary {
    size_t frames = 0;
    double seconds = 0;
    double fps = 0;
    double average_ms = 0;
    double p95_ms = 0;
    double p99_ms = 0;
    double max_ms = 0;
    // Frames that took longer than one or two 60 Hz refreshes.
    size_t over_16_7_ms = 0;
    size_t over_33_3_ms = 0;
    // The command processor thread's time in the renderer's IssueDraw a
    // frame, and the draws a frame (resolves not counted).
    double draw_average_ms = 0;
    double draw_p99_ms = 0;
    double draws = 0;
    // The same for the resolves (draws in the EDRAM copy mode).
    double copy_average_ms = 0;
    double copies = 0;
    // The command processor thread waiting a frame: on an empty ring, and in
    // WAIT_REG_MEM. Their sum near zero while frames are late: that thread
    // is what the frame waits for.
    double idle_average_ms = 0;
    double wait_reg_average_ms = 0;
  };

  // draw_ms, draws: the frame's time in IssueDraw and its draws; copy_ms,
  // copies: in its resolves.
  void Add(double frame_ms, double draw_ms = 0, uint32_t draws = 0, double copy_ms = 0,
           uint32_t copies = 0, double idle_ms = 0, double wait_reg_ms = 0) {
    frame_ms_.push_back(frame_ms);
    total_ms_ += frame_ms;
    draw_ms_.push_back(draw_ms);
    draw_total_ms_ += draw_ms;
    draws_ += draws;
    copy_total_ms_ += copy_ms;
    copies_ += copies;
    idle_total_ms_ += idle_ms;
    wait_reg_total_ms_ += wait_reg_ms;
  }

  double window_seconds() const { return total_ms_ / 1000.0; }
  bool empty() const { return frame_ms_.empty(); }

  // Summarizes the frames since the last call and starts a new window.
  Summary Take() {
    Summary summary;
    summary.frames = frame_ms_.size();
    if (!summary.frames) {
      return summary;
    }
    summary.seconds = total_ms_ / 1000.0;
    summary.average_ms = total_ms_ / double(summary.frames);
    summary.fps = summary.seconds > 0 ? double(summary.frames) / summary.seconds : 0;
    for (double ms : frame_ms_) {
      summary.over_16_7_ms += ms > 1000.0 / 60.0 + kSlackMs;
      summary.over_33_3_ms += ms > 2000.0 / 60.0 + kSlackMs;
    }
    std::sort(frame_ms_.begin(), frame_ms_.end());
    auto percentile = [this](double p) {
      size_t index = size_t(p * double(frame_ms_.size() - 1) + 0.5);
      return frame_ms_[std::min(index, frame_ms_.size() - 1)];
    };
    summary.p95_ms = percentile(0.95);
    summary.p99_ms = percentile(0.99);
    summary.max_ms = frame_ms_.back();
    summary.draw_average_ms = draw_total_ms_ / double(summary.frames);
    summary.draws = double(draws_) / double(summary.frames);
    summary.copy_average_ms = copy_total_ms_ / double(summary.frames);
    summary.copies = double(copies_) / double(summary.frames);
    summary.idle_average_ms = idle_total_ms_ / double(summary.frames);
    summary.wait_reg_average_ms = wait_reg_total_ms_ / double(summary.frames);
    std::sort(draw_ms_.begin(), draw_ms_.end());
    size_t draw_index = size_t(0.99 * double(draw_ms_.size() - 1) + 0.5);
    summary.draw_p99_ms = draw_ms_[std::min(draw_index, draw_ms_.size() - 1)];
    frame_ms_.clear();
    total_ms_ = 0;
    draw_ms_.clear();
    draw_total_ms_ = 0;
    draws_ = 0;
    copy_total_ms_ = 0;
    copies_ = 0;
    idle_total_ms_ = 0;
    wait_reg_total_ms_ = 0;
    return summary;
  }

  static std::string Format(const Summary& s) {
    return fmt::format(
        "{} frames in {:.1f} s, {:.1f} fps; frame time avg {:.2f} ms, p95 {:.2f}, p99 {:.2f}, "
        "max {:.2f}; {} over 16.7 ms, {} over 33.3 ms",
        s.frames, s.seconds, s.fps, s.average_ms, s.p95_ms, s.p99_ms, s.max_ms, s.over_16_7_ms,
        s.over_33_3_ms);
  }

  // The renderer's line: per frame, its graphics card time (and its share of
  // the frame time), the CPU time in its draws, and its counters.
  static std::string FormatRenderer(const Summary& s, const RendererFrameCounters& c) {
    std::string line = fmt::format(
        "{:.0f} draws a frame, CPU in draws avg {:.2f} ms, p99 {:.2f}, resolves {:.1f} a frame, "
        "CPU in them avg {:.2f} ms; command processor waiting {:.2f} ms a frame (ring empty "
        "{:.2f}, WAIT_REG_MEM {:.2f})",
        s.draws, s.draw_average_ms, s.draw_p99_ms, s.copies, s.copy_average_ms,
        s.idle_average_ms + s.wait_reg_average_ms, s.idle_average_ms, s.wait_reg_average_ms);
    if (!c.reported || !s.frames) {
      return line;
    }
    double frames = double(s.frames);
    if (c.gpu_timed) {
      double gpu_ms = double(c.gpu_ns) / 1.0e6 / frames;
      line = fmt::format("GPU {:.2f} ms a frame ({:.0f}% of the frame time); ", gpu_ms,
                         s.average_ms > 0 ? 100.0 * gpu_ms / s.average_ms : 0.0) +
             line;
    } else {
      line = "GPU not timed; " + line;
    }
    line += fmt::format("; {:.0f} pipeline binds, {:.1f} render passes, {:.1f} submissions a frame",
                        double(c.draw_pipeline_binds) / frames,
                        double(c.draw_render_passes) / frames, double(c.submissions) / frames);
    if (!c.detail.empty()) {
      line += "; " + c.detail;
    }
    return line;
  }

 private:
  // Swap timing jitters by a fraction of a millisecond; a frame is only late
  // when it clearly misses a refresh.
  static constexpr double kSlackMs = 1.0;

  std::vector<double> frame_ms_;
  double total_ms_ = 0;
  std::vector<double> draw_ms_;
  double draw_total_ms_ = 0;
  uint64_t draws_ = 0;
  double copy_total_ms_ = 0;
  uint64_t copies_ = 0;
  double idle_total_ms_ = 0;
  double wait_reg_total_ms_ = 0;
};

// gpu_indirect_buffer_survey (with frame_stats_interval): how much of the
// command processor thread's time goes to indirect buffers (the D3D command
// buffers titles replay every frame) whose contents are the same as at their
// last run at that address, and how much of that is outside the draws,
// resolves and waits: the most a decoder that remembered such buffers (a
// static-list memo, ReIGlue 0650's idea, doc 21 Q11) could save. Command
// processor thread only.
class IndirectBufferSurvey {
 public:
  enum Kind : uint32_t { kNew, kChanged, kSame, kKindCount };
  struct Counts {
    uint64_t buffers = 0;
    uint64_t dwords = 0;
    // Host ticks executing them, and of those outside IssueDraw and waits.
    uint64_t ticks = 0;
    uint64_t outside_ticks = 0;
  };

  // A run of the buffer at `address`, `dwords` long, whose contents hash to
  // `hash`, against its last run there; remembered for the next one.
  Kind Classify(uint32_t address, uint32_t dwords, uint64_t hash) {
    if (last_hash_.size() >= kMaxRemembered) {
      last_hash_.clear();
    }
    const uint64_t key = (uint64_t(address) << 32) | dwords;
    auto [it, inserted] = last_hash_.try_emplace(key, hash);
    if (inserted) {
      return kNew;
    }
    const bool same = it->second == hash;
    it->second = hash;
    return same ? kSame : kChanged;
  }

  void Add(Kind kind, uint32_t dwords, uint64_t ticks, uint64_t outside_ticks) {
    Counts& c = counts_[kind];
    ++c.buffers;
    c.dwords += dwords;
    c.ticks += ticks;
    c.outside_ticks += outside_ticks;
  }
  void AddHashTicks(uint64_t ticks) { hash_ticks_ += ticks; }
  const Counts& counts(Kind kind) const { return counts_[kind]; }

  // The window's figures a frame; empty without frames or buffers.
  std::string Format(uint64_t frames, uint64_t tick_frequency) const {
    uint64_t buffers = 0;
    uint64_t dwords = 0;
    for (const Counts& c : counts_) {
      buffers += c.buffers;
      dwords += c.dwords;
    }
    if (!frames || !buffers || !tick_frequency) {
      return std::string();
    }
    const double f = double(frames);
    const auto ms = [&](uint64_t ticks) {
      return double(ticks) * 1000.0 / double(tick_frequency) / f;
    };
    const Counts& same = counts_[kSame];
    const Counts& changed = counts_[kChanged];
    const Counts& fresh = counts_[kNew];
    return fmt::format(
        "{:.1f} a frame ({:.0f} dwords); same content as at their last run at that address: "
        "{:.1f} a frame, {:.0f}% of the dwords, CP {:.2f} ms a frame, of it {:.2f} ms outside "
        "draws, resolves and waits; changed: {:.1f}, CP {:.2f} ms, {:.2f} outside; new: {:.1f}, "
        "CP {:.2f} ms, {:.2f} outside; hashing {:.2f} ms a frame",
        double(buffers) / f, double(dwords) / f, double(same.buffers) / f,
        dwords ? 100.0 * double(same.dwords) / double(dwords) : 0.0, ms(same.ticks),
        ms(same.outside_ticks), double(changed.buffers) / f, ms(changed.ticks),
        ms(changed.outside_ticks), double(fresh.buffers) / f, ms(fresh.ticks),
        ms(fresh.outside_ticks), ms(hash_ticks_));
  }

  // Starts a new window (the remembered hashes stay).
  void ResetCounts() {
    for (Counts& c : counts_) {
      c = Counts();
    }
    hash_ticks_ = 0;
  }

 private:
  static constexpr size_t kMaxRemembered = 65536;
  std::unordered_map<uint64_t, uint64_t> last_hash_;
  Counts counts_[kKindCount];
  uint64_t hash_ticks_ = 0;
};

// gpu_pm4_survey (doc 21 NV1): the command processor's time by packet kind,
// outside draws, resolves and waits: what decoding the packets on a thread of
// their own (NV1) or making one kind cheaper could save at most. Indirect
// buffers are not counted themselves: the packets in them are. Each counted
// packet also carries about one host clock read of its own timing.
class Pm4PacketSurvey {
 public:
  // Type-3 packets by opcode (0-127), then type 0 (register writes) and 1.
  static constexpr uint32_t kType0 = 128;
  static constexpr uint32_t kType1 = 129;
  static constexpr uint32_t kSlots = 130;

  void Add(uint32_t slot, uint32_t dwords, uint64_t outside_ticks) {
    Counts& c = counts_[std::min(slot, kSlots - 1)];
    ++c.packets;
    c.dwords += dwords;
    c.outside_ticks += outside_ticks;
  }

  static const char* SlotName(uint32_t slot) {
    switch (slot) {
      case kType0:
        return "type 0 registers";
      case kType1:
        return "type 1 registers";
      case 0x10:
        return "NOP";
      case 0x21:
        return "REG_RMW";
      case 0x22:
        return "DRAW_INDX";
      case 0x23:
        return "VIZ_QUERY";
      case 0x25:
        return "SET_STATE";
      case 0x26:
        return "WAIT_FOR_IDLE";
      case 0x27:
        return "IM_LOAD";
      case 0x2b:
        return "IM_LOAD_IMMEDIATE";
      case 0x2d:
        return "SET_CONSTANT";
      case 0x2e:
        return "LOAD_CONSTANT_CONTEXT";
      case 0x2f:
        return "LOAD_ALU_CONSTANT";
      case 0x36:
        return "DRAW_INDX_2";
      case 0x3b:
        return "INVALIDATE_STATE";
      case 0x3c:
        return "WAIT_REG_MEM";
      case 0x3d:
        return "MEM_WRITE";
      case 0x3e:
        return "REG_TO_MEM";
      case 0x44:
        return "COND_EXEC";
      case 0x45:
        return "COND_WRITE";
      case 0x46:
        return "EVENT_WRITE";
      case 0x4a:
        return "SET_SHADER_BASES";
      case 0x50:
        return "SET_BIN_MASK";
      case 0x51:
        return "SET_BIN_SELECT";
      case 0x54:
        return "INTERRUPT";
      case 0x55:
        return "SET_CONSTANT2";
      case 0x56:
        return "SET_SHADER_CONSTANTS";
      case 0x58:
        return "EVENT_WRITE_SHD";
      case 0x59:
        return "EVENT_WRITE_CFL";
      case 0x5a:
        return "EVENT_WRITE_EXT";
      case 0x5b:
        return "EVENT_WRITE_ZPD";
      case 0x5e:
        return "CONTEXT_UPDATE";
      case 0x64:
        return "XE_SWAP";
      default:
        return nullptr;
    }
  }

  // The window's total and its `top` largest kinds, ms a frame (packets a
  // frame); empty without frames or packets.
  std::string Format(uint64_t frames, uint64_t tick_frequency, size_t top = 10) const {
    std::vector<uint32_t> slots;
    uint64_t total_ticks = 0;
    uint64_t total_packets = 0;
    for (uint32_t i = 0; i < kSlots; ++i) {
      if (counts_[i].packets) {
        slots.push_back(i);
        total_ticks += counts_[i].outside_ticks;
        total_packets += counts_[i].packets;
      }
    }
    if (!frames || !total_packets || !tick_frequency) {
      return std::string();
    }
    const double f = double(frames);
    const auto ms = [&](uint64_t ticks) {
      return double(ticks) * 1000.0 / double(tick_frequency) / f;
    };
    std::stable_sort(slots.begin(), slots.end(), [&](uint32_t a, uint32_t b) {
      return counts_[a].outside_ticks > counts_[b].outside_ticks;
    });
    std::string line = fmt::format(
        "outside draws, resolves and waits {:.2f} ms a frame ({:.0f} packets); by kind:",
        ms(total_ticks), double(total_packets) / f);
    for (size_t i = 0; i < slots.size() && i < top; ++i) {
      const Counts& c = counts_[slots[i]];
      const char* name = SlotName(slots[i]);
      line += fmt::format("{} {} {:.2f} ({:.0f}, {:.0f} dwords)", i ? "," : "",
                          name ? std::string(name) : fmt::format("0x{:02X}", slots[i]),
                          ms(c.outside_ticks), double(c.packets) / f, double(c.dwords) / f);
    }
    return line;
  }

  void ResetCounts() {
    for (Counts& c : counts_) {
      c = Counts();
    }
  }

 private:
  struct Counts {
    uint64_t packets = 0;
    uint64_t dwords = 0;
    uint64_t outside_ticks = 0;
  };
  Counts counts_[kSlots];
};

}  // namespace rex::graphics
