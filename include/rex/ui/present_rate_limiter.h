#pragma once

#include <chrono>
#include <cstdint>

namespace rex::ui {

// Called under the presenter's paint mutex, before acquiring swapchain images.
class PresentRateLimiter {
 public:
  using Clock = std::chrono::steady_clock;
  bool Allows(int32_t rate, Clock::time_point now) {
    if (rate != rate_) {
      rate_ = rate;
      next_ = now;
    }
    return rate <= 0 || now >= next_;
  }
  void NotePresent(int32_t rate, Clock::time_point now) {
    if (rate <= 0)
      return;
    const auto period =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate));
    auto next = next_ + period;
    // Keep a fixed grid under ordinary jitter; after a stall permit no burst.
    next_ = next > now && next <= now + period ? next : now + period;
  }

 private:
  int32_t rate_ = 0;
  Clock::time_point next_{};
};

}  // namespace rex::ui
