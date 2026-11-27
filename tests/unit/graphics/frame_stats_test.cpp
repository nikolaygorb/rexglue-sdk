/**
 * @file        frame_stats_test.cpp
 * @brief       Frame pacing summaries for frame_stats_interval
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <rex/graphics/frame_stats.h>

using Catch::Approx;
using rex::graphics::FrameStats;

TEST_CASE("Frame stats summarize steady 60 fps", "[graphics][frame-stats]") {
  FrameStats stats;
  for (int i = 0; i < 600; ++i) {
    stats.Add(1000.0 / 60.0);
  }
  CHECK(stats.window_seconds() == Approx(10.0));
  auto s = stats.Take();
  CHECK(s.frames == 600);
  CHECK(s.fps == Approx(60.0));
  CHECK(s.average_ms == Approx(16.667).epsilon(0.001));
  CHECK(s.p99_ms == Approx(16.667).epsilon(0.001));
  // Jitter within a millisecond of a refresh is not a late frame.
  CHECK(s.over_16_7_ms == 0);
  CHECK(s.over_33_3_ms == 0);
  CHECK(stats.empty());
  CHECK(stats.Take().frames == 0);
}

TEST_CASE("Frame stats count late frames and tail percentiles", "[graphics][frame-stats]") {
  FrameStats stats;
  for (int i = 0; i < 90; ++i) {
    stats.Add(16.7);
  }
  for (int i = 0; i < 9; ++i) {
    stats.Add(33.3);  // missed one refresh
  }
  stats.Add(100.0);  // a hitch
  auto s = stats.Take();
  CHECK(s.frames == 100);
  CHECK(s.over_16_7_ms == 10);
  CHECK(s.over_33_3_ms == 1);
  CHECK(s.max_ms == 100.0);
  CHECK(s.p95_ms == Approx(33.3));
  CHECK(s.p99_ms == Approx(33.3));
  CHECK(FrameStats::Format(s).find("100 frames") == 0);
}

TEST_CASE("Frame stats summarize the renderer's draws and counters", "[graphics][frame-stats]") {
  FrameStats stats;
  for (int i = 0; i < 99; ++i) {
    stats.Add(20.0, 2.0, 1000);
  }
  stats.Add(20.0, 12.0, 1100);
  auto s = stats.Take();
  CHECK(s.draws == Approx(1001.0));
  CHECK(s.draw_average_ms == Approx(2.1));
  CHECK(s.draw_p99_ms == Approx(2.0));
  // A renderer that reports nothing: the command processor's figures only.
  rex::graphics::RendererFrameCounters counters;
  CHECK(FrameStats::FormatRenderer(s, counters).find("1001 draws a frame") == 0);
  counters.reported = true;
  counters.gpu_timed = true;
  counters.gpu_ns = 100 * 5000000ull;  // 5 ms a frame
  counters.submissions = 300;
  counters.draw_render_passes = 4000;
  counters.draw_pipeline_binds = 25000;
  std::string line = FrameStats::FormatRenderer(s, counters);
  CHECK(line.find("GPU 5.00 ms a frame (25% of the frame time)") == 0);
  CHECK(line.find("250 pipeline binds, 40.0 render passes, 3.0 submissions a frame") !=
        std::string::npos);
  // The renderer's own figures go last.
  counters.detail = "IssueDraw ms a frame: shaders 1.00";
  line = FrameStats::FormatRenderer(s, counters);
  CHECK(line.size() > counters.detail.size());
  CHECK(line.compare(line.size() - counters.detail.size() - 2, std::string::npos,
                     "; " + counters.detail) == 0);
  CHECK(stats.Take().frames == 0);
}

TEST_CASE("Frame stats keep resolves apart from draws", "[graphics][frame-stats]") {
  FrameStats stats;
  for (int i = 0; i < 10; ++i) {
    stats.Add(20.0, 3.0, 500, 4.0, 30);
  }
  auto s = stats.Take();
  CHECK(s.draws == Approx(500.0));
  CHECK(s.draw_average_ms == Approx(3.0));
  CHECK(s.copies == Approx(30.0));
  CHECK(s.copy_average_ms == Approx(4.0));
  rex::graphics::RendererFrameCounters counters;
  CHECK(FrameStats::FormatRenderer(s, counters) ==
        "500 draws a frame, CPU in draws avg 3.00 ms, p99 3.00, resolves 30.0 a frame, CPU in "
        "them avg 4.00 ms; command processor waiting 0.00 ms a frame (ring empty 0.00, "
        "WAIT_REG_MEM 0.00)");
  // Draws only: no resolves.
  stats.Add(20.0, 3.0, 500);
  s = stats.Take();
  CHECK(s.copies == 0.0);
  CHECK(s.copy_average_ms == 0.0);
}

TEST_CASE("Frame stats sum the command processor's waits", "[graphics][frame-stats]") {
  FrameStats stats;
  for (int i = 0; i < 4; ++i) {
    stats.Add(20.0, 3.0, 500, 1.0, 30, 2.0 + i, 0.5);
  }
  auto s = stats.Take();
  CHECK(s.idle_average_ms == Approx(3.5));
  CHECK(s.wait_reg_average_ms == Approx(0.5));
  rex::graphics::RendererFrameCounters counters;
  CHECK(FrameStats::FormatRenderer(s, counters).find(
            "; command processor waiting 4.00 ms a frame (ring empty 3.50, WAIT_REG_MEM 0.50)") !=
        std::string::npos);
  // Taken: the next window starts at zero.
  stats.Add(20.0);
  s = stats.Take();
  CHECK(s.idle_average_ms == 0.0);
  CHECK(s.wait_reg_average_ms == 0.0);
}

TEST_CASE("Indirect buffer survey tells repeated contents from changed ones",
          "[graphics][frame-stats]") {
  rex::graphics::IndirectBufferSurvey survey;
  using Survey = rex::graphics::IndirectBufferSurvey;
  CHECK(survey.Classify(0x1000, 64, 0xAA) == Survey::kNew);
  CHECK(survey.Classify(0x1000, 64, 0xAA) == Survey::kSame);
  CHECK(survey.Classify(0x1000, 64, 0xBB) == Survey::kChanged);
  CHECK(survey.Classify(0x1000, 64, 0xBB) == Survey::kSame);
  // Another length at the same address is another buffer.
  CHECK(survey.Classify(0x1000, 32, 0xBB) == Survey::kNew);
  CHECK(survey.Classify(0x2000, 64, 0xBB) == Survey::kNew);
}

TEST_CASE("Indirect buffer survey reports a frame's share", "[graphics][frame-stats]") {
  rex::graphics::IndirectBufferSurvey survey;
  using Survey = rex::graphics::IndirectBufferSurvey;
  CHECK(survey.Format(2, 1000).empty());
  // Ticks at 1000 a second: 1 tick = 1 ms.
  survey.Add(Survey::kSame, 300, 8, 3);
  survey.Add(Survey::kSame, 300, 8, 3);
  survey.Add(Survey::kChanged, 100, 4, 2);
  survey.Add(Survey::kNew, 100, 2, 1);
  survey.AddHashTicks(2);
  const std::string line = survey.Format(2, 1000);
  CHECK(line.find("2.0 a frame (400 dwords)") != std::string::npos);
  CHECK(line.find("1.0 a frame, 75% of the dwords, CP 8.00 ms a frame, of it 3.00 ms outside") !=
        std::string::npos);
  CHECK(line.find("changed: 0.5, CP 2.00 ms, 1.00 outside") != std::string::npos);
  CHECK(line.find("new: 0.5, CP 1.00 ms, 0.50 outside") != std::string::npos);
  CHECK(line.find("hashing 1.00 ms a frame") != std::string::npos);
  survey.ResetCounts();
  CHECK(survey.Format(2, 1000).empty());
  // The hashes stay across windows.
  CHECK(survey.Classify(0x3000, 8, 1) == Survey::kNew);
  survey.ResetCounts();
  CHECK(survey.Classify(0x3000, 8, 1) == Survey::kSame);
}

TEST_CASE("PM4 packet survey ranks packet kinds by time outside draws", "[graphics][frame-stats]") {
  rex::graphics::Pm4PacketSurvey survey;
  using Survey = rex::graphics::Pm4PacketSurvey;
  CHECK(survey.Format(1, 1000).empty());
  // Ticks at 1000 a second: 1 tick = 1 ms; one frame.
  survey.Add(Survey::kType0, 10, 2);
  survey.Add(Survey::kType0, 10, 2);
  survey.Add(0x2d, 100, 6);
  survey.Add(0x46, 2, 1);
  survey.Add(0x77, 3, 1);
  const std::string line = survey.Format(1, 1000);
  CHECK(line.find("outside draws, resolves and waits 12.00 ms a frame (5 packets)") !=
        std::string::npos);
  // Largest first: SET_CONSTANT, then the register writes; unnamed opcodes in hex.
  CHECK(line.find("by kind: SET_CONSTANT 6.00 (1, 100 dwords), type 0 registers 4.00 (2, 20 "
                  "dwords), EVENT_WRITE 1.00 (1, 2 dwords), 0x77 1.00 (1, 3 dwords)") !=
        std::string::npos);
  CHECK(survey.Format(1, 1000, 1).find("type 0") == std::string::npos);
  survey.ResetCounts();
  CHECK(survey.Format(1, 1000).empty());
}
