#include "timer.h"

#include <gtest/gtest.h>

TEST(BenchmarkTimer, MeasuresElapsedTimeAcrossSleep) {
  // Initialize calibration before measuring either interval.
  EXPECT_GT(talus::bench::CycleClock::NanosPerTick(), 0.0);
  const auto wall_start = talus::bench::CycleClock::MonotonicNanos();
  const auto start = talus::bench::CycleClock::Now();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const auto end = talus::bench::CycleClock::Now();
  const auto wall_end = talus::bench::CycleClock::MonotonicNanos();
  ASSERT_GT(end, start);
  const double elapsed = static_cast<double>(talus::bench::CycleClock::ToNanos(end - start));
  const double wall_elapsed = static_cast<double>(wall_end - wall_start);
  EXPECT_GT(elapsed, wall_elapsed * 0.8);
  EXPECT_LT(elapsed, wall_elapsed * 1.2);
}
