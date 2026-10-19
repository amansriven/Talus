#include "util/rate_limiter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

namespace talus {
namespace {

using Clock = std::chrono::steady_clock;

TEST(RateLimiterTest, UnlimitedNeverWaits) {
  RateLimiter limiter(0);
  const auto start = Clock::now();
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(limiter.Request(1 << 20).count(), 0);
  }
  EXPECT_LT(Clock::now() - start, std::chrono::milliseconds(50));
}

// 1 MiB at 4 MiB/s in 4 KiB requests takes about 250 ms in total, and the
// bytes are spread evenly rather than written in bursts.
TEST(RateLimiterTest, PacesWritesSmoothly) {
  constexpr uint64_t kRate = 4 << 20;
  constexpr uint64_t kChunk = 4096;
  constexpr int kChunks = 256;
  RateLimiter limiter(kRate);
  std::vector<Clock::time_point> done;
  const auto start = Clock::now();
  for (int i = 0; i < kChunks; ++i) {
    limiter.Request(kChunk);
    done.push_back(Clock::now());
  }
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  EXPECT_GT(elapsed, 0.22);
  EXPECT_LT(elapsed, 0.40);

  // Smoothness: in any 25 ms window, no more than that window's share of the
  // rate plus a few chunks of slack (bucket refill and sleep jitter).
  const auto window = std::chrono::milliseconds(25);
  const uint64_t allowed = kRate / 40 + 4 * kChunk;
  size_t lo = 0;
  for (size_t hi = 0; hi < done.size(); ++hi) {
    while (done[hi] - done[lo] > window) {
      ++lo;
    }
    EXPECT_LE((hi - lo + 1) * kChunk, allowed) << "burst ending at chunk " << hi;
  }
}

TEST(RateLimiterTest, ThreadsShareTheRate) {
  constexpr uint64_t kRate = 2 << 20;
  RateLimiter limiter(kRate);
  const auto start = Clock::now();
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&limiter] {
      for (int i = 0; i < 32; ++i) {
        limiter.Request(4096);  // 4 threads x 128 KiB = 512 KiB total.
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  EXPECT_GT(elapsed, 0.22);  // 512 KiB / 2 MiB/s = 250 ms.
  EXPECT_LT(elapsed, 0.45);
}

}  // namespace
}  // namespace talus
