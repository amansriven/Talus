#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace talus {

// A token bucket limiting compaction write bandwidth.
//
// Tokens (bytes) accrue continuously at `bytes_per_second`, up to a bucket of
// one refill period's worth (1 ms by default). Request(n) takes n tokens,
// going into debt if needed, and a caller in debt sleeps until the debt is
// repaid. Because the bucket holds only a millisecond of tokens, writes are
// paced at block granularity instead of in bursts. Thread-safe; callers share
// the rate.
class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;

  // bytes_per_second == 0 means unlimited: Request() returns at once.
  explicit RateLimiter(uint64_t bytes_per_second,
                       std::chrono::microseconds refill_period = std::chrono::milliseconds(1));

  RateLimiter(const RateLimiter&) = delete;
  RateLimiter& operator=(const RateLimiter&) = delete;

  // Blocks until `bytes` may be written. Returns the time spent waiting.
  std::chrono::nanoseconds Request(uint64_t bytes);

  uint64_t bytes_per_second() const { return rate_; }

 private:
  const uint64_t rate_;
  const double burst_bytes_;

  std::mutex mu_;
  double tokens_;                  // Guarded by mu_; negative means debt.
  Clock::time_point last_refill_;  // Guarded by mu_.
};

}  // namespace talus
