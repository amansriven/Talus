#pragma once

// A cycle-counter clock for per-operation latency (spec §12):
//  * x86-64: RDTSCP. It waits for earlier instructions to finish, so the
//    measured operation cannot leak past the end timestamp. Modern x86 has an
//    invariant TSC (constant rate, synchronized across cores).
//  * macOS ARM64: mach_absolute_time (the supported monotonic clock).
//  * Other ARM64: CNTVCT_EL0, the virtual counter, preceded by ISB so it is not
//    read early. Its rate is fixed and platform-dependent.
//  * Elsewhere: std::chrono::steady_clock.
// Ticks are converted to nanoseconds with a ratio calibrated once at startup
// against clock_gettime(CLOCK_MONOTONIC).

#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>

#if defined(__APPLE__) && defined(__aarch64__)
#include <mach/mach_time.h>
#endif

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace talus::bench {

class CycleClock {
 public:
  static uint64_t Now() { return Config().fallback ? SteadyNow() : RawNow(); }

 private:
  static uint64_t RawNow() {
#if defined(__x86_64__)
    unsigned aux = 0;
    return __rdtscp(&aux);
#elif defined(__APPLE__) && defined(__aarch64__)
    return mach_absolute_time();
#elif defined(__aarch64__)
    uint64_t ticks = 0;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(ticks)::"memory");
    return ticks;
#else
    return SteadyNow();
#endif
  }

 public:
  static const char* Source() {
    if (Config().fallback) {
      return "steady_clock";
    }
#if defined(__x86_64__)
    return "rdtscp";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "mach_absolute_time";
#elif defined(__aarch64__)
    return "cntvct_el0";
#else
    return "steady_clock";
#endif
  }

  // Nanoseconds per tick, measured over ~100 ms against CLOCK_MONOTONIC.
  static double NanosPerTick() { return Config().ratio; }

  static uint64_t ToNanos(uint64_t ticks) {
    return static_cast<uint64_t>(static_cast<double>(ticks) * NanosPerTick());
  }

  static uint64_t MonotonicNanos() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
  }

 private:
  struct ClockConfig {
    double ratio;
    bool fallback;
  };

  static uint64_t SteadyNow() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
  }

  static ClockConfig Calibrate() {
    const uint64_t t0 = MonotonicNanos();
    const uint64_t c0 = RawNow();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint64_t c1 = RawNow();
    const uint64_t t1 = MonotonicNanos();
    // Reject backward/stopped counters before unsigned subtraction. A broad
    // plausible range also catches wraps and unusable virtualized counters.
    if (t1 <= t0 || c1 <= c0) {
      return {1.0, true};
    }
    const double ratio = static_cast<double>(t1 - t0) / static_cast<double>(c1 - c0);
    if (!(ratio >= 0.01 && ratio <= 1000.0)) {
      return {1.0, true};
    }
    return {ratio, false};
  }

  static const ClockConfig& Config() {
    // Select once, before timestamps are taken: all threads share one clock
    // domain and conversion ratio, including when calibration falls back.
    static const ClockConfig config = Calibrate();
    return config;
  }
};

}  // namespace talus::bench
