#pragma once

// A cycle-counter clock for per-operation latency (spec §12):
//  * x86-64: RDTSCP. It waits for earlier instructions to finish, so the
//    measured operation cannot leak past the end timestamp. Modern x86 has an
//    invariant TSC (constant rate, synchronized across cores).
//  * ARM64: CNTVCT_EL0, the virtual counter, preceded by ISB so it is not
//    read early. Its rate is fixed (24 MHz on Apple Silicon, so ~42 ns
//    resolution; typically higher on server ARM).
//  * Elsewhere: std::chrono::steady_clock.
// Ticks are converted to nanoseconds with a ratio calibrated once at startup
// against clock_gettime(CLOCK_MONOTONIC).

#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace talus::bench {

class CycleClock {
 public:
  static uint64_t Now() {
#if defined(__x86_64__)
    unsigned aux = 0;
    return __rdtscp(&aux);
#elif defined(__aarch64__)
    uint64_t ticks = 0;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(ticks)::"memory");
    return ticks;
#else
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
  }

  static const char* Source() {
#if defined(__x86_64__)
    return "rdtscp";
#elif defined(__aarch64__)
    return "cntvct_el0";
#else
    return "steady_clock";
#endif
  }

  // Nanoseconds per tick, measured over ~100 ms against CLOCK_MONOTONIC.
  static double NanosPerTick() {
    static const double kRatio = Calibrate();
    return kRatio;
  }

  static uint64_t ToNanos(uint64_t ticks) {
    return static_cast<uint64_t>(static_cast<double>(ticks) * NanosPerTick());
  }

  static uint64_t MonotonicNanos() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
  }

 private:
  static double Calibrate() {
    const uint64_t t0 = MonotonicNanos();
    const uint64_t c0 = Now();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint64_t t1 = MonotonicNanos();
    const uint64_t c1 = Now();
    return static_cast<double>(t1 - t0) / static_cast<double>(c1 - c0);
  }
};

}  // namespace talus::bench
