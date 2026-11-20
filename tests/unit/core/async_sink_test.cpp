#include <catch2/catch_test_macros.hpp>
#include <rex/logging/async_sink.h>
#include <spdlog/sinks/base_sink.h>

#include <future>
#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("Async file logging owns messages and drains in order", "[logging]") {
  class Sink final : public spdlog::sinks::base_sink<std::mutex> {
   public:
    std::vector<std::string> messages;
    bool flushed = false;

   protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
      messages.emplace_back(message.payload.data(), message.payload.size());
    }
    void flush_() override { flushed = true; }
  };
  auto sink = std::make_shared<Sink>();
  rex::AsyncFileSink async(sink);
  for (int i = 0; i < 64; ++i) {
    std::string line = std::to_string(i);
    async.log(spdlog::details::log_msg("test", spdlog::level::info, line));
  }
  REQUIRE(async.Drain(std::chrono::seconds(2)));
  REQUIRE(sink->messages.size() == 64);
  for (int i = 0; i < 64; ++i)
    CHECK(sink->messages[i] == std::to_string(i));
  CHECK(sink->flushed);
}

TEST_CASE("An async producer does not wait for a blocked file writer", "[logging]") {
  class Sink final : public spdlog::sinks::base_sink<std::mutex> {
   public:
    std::promise<void> entered, release;
    std::shared_future<void> released = release.get_future().share();

   protected:
    void sink_it_(const spdlog::details::log_msg&) override {
      if (!started_) {
        started_ = true;
        entered.set_value();
      }
      released.wait();
    }
    void flush_() override {}
    bool started_ = false;
  };
  auto sink = std::make_shared<Sink>();
  rex::AsyncFileSink async(sink);
  async.log(spdlog::details::log_msg("test", spdlog::level::info, "first"));
  const auto entered = sink->entered.get_future().wait_for(std::chrono::seconds(2));
  async.log(spdlog::details::log_msg("test", spdlog::level::info, "second"));
  const bool early = async.Drain(std::chrono::milliseconds(1));
  sink->release.set_value();
  REQUIRE(entered == std::future_status::ready);
  CHECK_FALSE(early);
  CHECK(async.Drain(std::chrono::seconds(2)));
}

TEST_CASE("An async drain reports a failed writer", "[logging]") {
  class Sink final : public spdlog::sinks::base_sink<std::mutex> {
   protected:
    void sink_it_(const spdlog::details::log_msg&) override {
      throw std::runtime_error("unwritable log");
    }
    void flush_() override {}
  };
  rex::AsyncFileSink async(std::make_shared<Sink>());
  async.log(spdlog::details::log_msg("test", spdlog::level::info, "message"));
  CHECK_FALSE(async.Drain(std::chrono::seconds(2)));
}
