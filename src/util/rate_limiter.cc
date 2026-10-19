#include "util/rate_limiter.h"

#include <algorithm>
#include <thread>

namespace talus {

RateLimiter::RateLimiter(uint64_t bytes_per_second, std::chrono::microseconds refill_period)
    : rate_(bytes_per_second),
      burst_bytes_(static_cast<double>(bytes_per_second) *
                   std::chrono::duration<double>(refill_period).count()),
      tokens_(burst_bytes_),
      last_refill_(Clock::now()) {}

std::chrono::nanoseconds RateLimiter::Request(uint64_t bytes) {
  if (rate_ == 0 || bytes == 0) {
    return std::chrono::nanoseconds(0);
  }
  Clock::duration wait{};
  {
    std::lock_guard<std::mutex> lock(mu_);
    const Clock::time_point now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - last_refill_).count();
    tokens_ = std::min(burst_bytes_, tokens_ + elapsed * static_cast<double>(rate_));
    last_refill_ = now;
    tokens_ -= static_cast<double>(bytes);
    if (tokens_ < 0) {
      // Sleep until the refill brings the balance back to zero. Later callers
      // see the deeper debt and queue up behind us.
      wait = std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(-tokens_ / static_cast<double>(rate_)));
    }
  }
  if (wait > Clock::duration::zero()) {
    const Clock::time_point start = Clock::now();
    std::this_thread::sleep_for(wait);
    return Clock::now() - start;
  }
  return std::chrono::nanoseconds(0);
}

}  // namespace talus
