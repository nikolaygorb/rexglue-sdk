#include <catch2/catch_test_macros.hpp>
#include <rex/ui/present_rate_limiter.h>

TEST_CASE("Host present cap follows a fixed grid and resets after stalls or rate changes", "[ui]") {
  using Limiter = rex::ui::PresentRateLimiter;
  using namespace std::chrono_literals;
  Limiter limit;
  const Limiter::Clock::time_point start{};
  REQUIRE(limit.Allows(100, start));
  limit.NotePresent(100, start);
  CHECK_FALSE(limit.Allows(100, start + 9ms));
  CHECK(limit.Allows(100, start + 11ms));
  limit.NotePresent(100, start + 11ms);
  CHECK(limit.Allows(100, start + 20ms));
  limit.NotePresent(100, start + 1s);
  CHECK_FALSE(limit.Allows(100, start + 1s));
  CHECK(limit.Allows(0, start + 1s));
  CHECK(limit.Allows(30, start + 1s));
}
