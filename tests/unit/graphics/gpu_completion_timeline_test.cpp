/**
 * @file        graphics/gpu_completion_timeline_test.cpp
 * @brief       Tests for the host GPU completion timeline submission bookkeeping
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/ui/gpu_completion_timeline.h>

namespace {

// A timeline where the "GPU" completes submissions only when told to, and an
// await completes everything up to the awaited submission (like a fence wait).
class FakeCompletionTimeline : public rex::ui::GPUCompletionTimeline {
 public:
  void Submit() { IncrementUpcomingSubmission(); }
  void CompleteOnGpu(uint64_t submission) { gpu_completed_ = submission; }

  void UpdateCompletedSubmission() override {
    if (gpu_completed_ > GetCompletedSubmissionFromLastUpdate()) {
      SetCompletedSubmission(gpu_completed_);
    }
  }

  std::vector<uint64_t> awaits;
  bool fail_awaits = false;

 protected:
  void AwaitSubmissionImpl(uint64_t awaited_submission) override {
    awaits.push_back(awaited_submission);
    if (!fail_awaits && gpu_completed_ < awaited_submission) {
      gpu_completed_ = awaited_submission;
    }
  }

 private:
  uint64_t gpu_completed_ = 0;
};

}  // namespace

// xenia-canary fe1fd3613.
TEST_CASE("Completion timeline starts with nothing submitted", "[graphics][timeline]") {
  FakeCompletionTimeline timeline;
  CHECK(timeline.GetUpcomingSubmission() == 1);
  CHECK(timeline.GetCompletedSubmissionFromLastUpdate() == 0);
  // Awaiting all with nothing submitted needs no wait.
  CHECK(timeline.AwaitAllSubmissions());
  CHECK(timeline.awaits.empty());
}

TEST_CASE("Completion timeline waits only when the submission is not complete",
          "[graphics][timeline]") {
  FakeCompletionTimeline timeline;
  timeline.Submit();
  timeline.Submit();
  timeline.Submit();
  REQUIRE(timeline.GetUpcomingSubmission() == 4);

  timeline.CompleteOnGpu(2);
  CHECK(timeline.AwaitSubmissionAndUpdateCompleted(2));
  CHECK(timeline.awaits.empty());
  CHECK(timeline.GetCompletedSubmissionFromLastUpdate() == 2);

  CHECK(timeline.AwaitAllSubmissions());
  REQUIRE(timeline.awaits.size() == 1);
  CHECK(timeline.awaits[0] == 3);
  CHECK(timeline.GetCompletedSubmissionFromLastUpdate() == 3);
}

TEST_CASE("Completion timeline limits the number of pending submissions",
          "[graphics][timeline]") {
  FakeCompletionTimeline timeline;
  // With fewer submissions than the limit, nothing is awaited.
  timeline.Submit();
  CHECK(timeline.AwaitMaxSubmissionsPendingAndUpdateCompleted(3));
  CHECK(timeline.awaits.empty());

  // Upcoming 6 with at most 3 pending: submissions up to 3 must be complete,
  // so that 4, 5 and the upcoming 6 can reuse the slots.
  for (int i = 0; i < 4; ++i) {
    timeline.Submit();
  }
  REQUIRE(timeline.GetUpcomingSubmission() == 6);
  CHECK(timeline.AwaitMaxSubmissionsPendingAndUpdateCompleted(3));
  REQUIRE(timeline.awaits.size() == 1);
  CHECK(timeline.awaits[0] == 3);
  CHECK(timeline.GetCompletedSubmissionFromLastUpdate() == 3);
}

TEST_CASE("Completion timeline reports a failed wait", "[graphics][timeline]") {
  FakeCompletionTimeline timeline;
  timeline.Submit();
  timeline.Submit();
  timeline.fail_awaits = true;
  CHECK_FALSE(timeline.AwaitAllSubmissions());
  CHECK(timeline.GetCompletedSubmissionFromLastUpdate() == 0);
  // The completed submission still advances once the GPU gets there.
  timeline.CompleteOnGpu(1);
  CHECK(timeline.UpdateAndGetCompletedSubmission() == 1);
}
