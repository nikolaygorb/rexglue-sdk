#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/sinks/sink.h>

namespace rex {

// One file writer shared by all category loggers. Producer locks never cover IO.
// flush() is asynchronous; crash/shutdown paths explicitly call Drain().
class AsyncFileSink final : public spdlog::sinks::sink {
 public:
  explicit AsyncFileSink(spdlog::sink_ptr inner, size_t capacity = 65536)
      : state_(std::make_shared<State>(std::move(inner), capacity)) {
    std::thread([state = state_] { Run(state); }).detach();
  }
  ~AsyncFileSink() override {
    state_->stopping.store(true, std::memory_order_release);
    state_->ready.notify_one();
  }

  void log(const spdlog::details::log_msg& message) override {
    {
      std::lock_guard lock(state_->mutex);
      const size_t limit =
          message.level < spdlog::level::warn ? state_->capacity : state_->capacity * 4;
      if (message.level < spdlog::level::err && state_->queue.size() >= limit) {
        ++state_->dropped;
        return;
      }
      state_->queue.emplace_back(message);
      ++state_->accepted;
    }
    state_->ready.notify_one();
    if (message.level == spdlog::level::critical) {
      Drain(std::chrono::seconds(1));
    }
  }
  void flush() override { state_->ready.notify_one(); }
  void set_pattern(const std::string& pattern) override { state_->inner->set_pattern(pattern); }
  void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override {
    state_->inner->set_formatter(std::move(formatter));
  }

  bool Drain(std::chrono::milliseconds timeout) {
    std::unique_lock lock(state_->mutex);
    const uint64_t target = state_->accepted;
    state_->ready.notify_one();
    const bool done = state_->drained.wait_for(
        lock, timeout, [&] { return state_->write_failed || state_->flushed >= target; });
    return done && !state_->write_failed;
  }

 private:
  struct State {
    State(spdlog::sink_ptr sink, size_t limit) : inner(std::move(sink)), capacity(limit) {}
    spdlog::sink_ptr inner;
    size_t capacity;
    std::mutex mutex;
    std::condition_variable ready, drained;
    std::deque<spdlog::details::log_msg_buffer> queue;
    uint64_t accepted = 0, flushed = 0, dropped = 0;
    std::atomic<bool> stopping{false};
    bool write_failed = false;
  };

  static void Run(const std::shared_ptr<State>& state) {
    for (;;) {
      std::deque<spdlog::details::log_msg_buffer> batch;
      uint64_t through, dropped;
      {
        std::unique_lock lock(state->mutex);
        state->ready.wait(lock, [&] { return state->stopping || !state->queue.empty(); });
        if (state->queue.empty() && state->stopping)
          return;
        batch.swap(state->queue);
        through = state->accepted;
        dropped = std::exchange(state->dropped, 0);
      }
      // An individual write failure must not terminate the detached worker.
      bool wrote = true;
      for (const auto& message : batch) {
        try {
          state->inner->log(message);
        } catch (...) {
          wrote = false;
        }
      }
      if (dropped) {
        try {
          const auto text = fmt::format("Async log queue dropped {} lines", dropped);
          state->inner->log(spdlog::details::log_msg("log", spdlog::level::warn, text));
        } catch (...) {}
      }
      bool flushed = false;
      try {
        state->inner->flush();
        flushed = true;
      } catch (...) {}
      {
        std::lock_guard lock(state->mutex);
        state->write_failed |= !wrote || !flushed;
        if (flushed)
          state->flushed = through;
      }
      state->drained.notify_all();
    }
  }

  std::shared_ptr<State> state_;
};

}  // namespace rex
