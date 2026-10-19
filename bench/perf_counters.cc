#include "perf_counters.h"

#include <cstdio>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace talus::bench {

void PerfTotals::Add(const PerfTotals& other) {
  if (!other.collected) return;
  if (!collected) {
    *this = other;
    return;
  }
  for (int i = 0; i < kNumPerfEvents; ++i) {
    values[i] += other.values[i];
    // A per-operation total is valid only if every worker counted the event.
    available[i] = available[i] && other.available[i];
  }
}

std::string PerfTotals::ToJson(uint64_t ops) const {
  static const char* const kNames[kNumPerfEvents] = {"cycles", "instructions", "l1d_misses",
                                                     "llc_misses", "branch_misses"};
  bool any = false;
  std::string json = "{";
  char buf[96];
  for (int i = 0; i < kNumPerfEvents; ++i) {
    if (!available[i]) {
      continue;
    }
    any = true;
    std::snprintf(buf, sizeof(buf), "\"%s_per_op\":%.3f,", kNames[i],
                  ops == 0 ? 0.0 : static_cast<double>(values[i]) / static_cast<double>(ops));
    json += buf;
  }
  if (available[kCycles] && available[kInstructions] && values[kCycles] > 0) {
    std::snprintf(
        buf, sizeof(buf), "\"ipc\":%.3f,",
        static_cast<double>(values[kInstructions]) / static_cast<double>(values[kCycles]));
    json += buf;
  }
  json += any ? "\"available\":true}" : "\"available\":false}";
  return json;
}

#if defined(__linux__)

namespace {

int OpenCounter(uint32_t type, uint64_t config) {
  perf_event_attr attr{};
  attr.size = sizeof(attr);
  attr.type = type;
  attr.config = config;
  attr.disabled = 1;
  attr.exclude_kernel = 1;  // User-space work only: works with perf_event_paranoid=2.
  attr.exclude_hv = 1;
  attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
  // pid 0, cpu -1: this thread, on whatever CPU it runs.
  return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
}

constexpr uint64_t CacheEvent(uint64_t cache, uint64_t op, uint64_t result) {
  return cache | (op << 8) | (result << 16);
}

}  // namespace

ThreadPerfCounters::ThreadPerfCounters() {
  fds_[kCycles] = OpenCounter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
  fds_[kInstructions] = OpenCounter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
  fds_[kL1dMisses] = OpenCounter(PERF_TYPE_HW_CACHE,
                                 CacheEvent(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ,
                                            PERF_COUNT_HW_CACHE_RESULT_MISS));
  fds_[kLlcMisses] = OpenCounter(PERF_TYPE_HW_CACHE,
                                 CacheEvent(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ,
                                            PERF_COUNT_HW_CACHE_RESULT_MISS));
  fds_[kBranchMisses] = OpenCounter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES);
}

ThreadPerfCounters::~ThreadPerfCounters() {
  for (int fd : fds_) {
    if (fd >= 0) {
      close(fd);
    }
  }
}

void ThreadPerfCounters::Start() {
  for (int& fd : fds_) {
    if (fd >= 0 &&
        (ioctl(fd, PERF_EVENT_IOC_RESET, 0) != 0 || ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0)) {
      close(fd);
      fd = -1;
    }
  }
}

PerfTotals ThreadPerfCounters::Stop() {
  PerfTotals totals;
  totals.collected = true;
  for (int i = 0; i < kNumPerfEvents; ++i) {
    if (fds_[i] < 0) {
      continue;
    }
    if (ioctl(fds_[i], PERF_EVENT_IOC_DISABLE, 0) != 0) continue;
    struct {
      uint64_t value;
      uint64_t enabled;
      uint64_t running;
    } sample{};
    if (read(fds_[i], &sample, sizeof(sample)) == static_cast<ssize_t>(sizeof(sample)) &&
        sample.running > 0 && sample.enabled >= sample.running) {
      // The PMU multiplexes when more events are requested than fit at once.
      // Scale each thread's value by its actual scheduled fraction.
      totals.values[i] = static_cast<uint64_t>(static_cast<double>(sample.value) *
                                               static_cast<double>(sample.enabled) /
                                               static_cast<double>(sample.running));
      totals.available[i] = true;
    }
  }
  return totals;
}

#else  // Not Linux: no perf_event_open.

ThreadPerfCounters::ThreadPerfCounters() { fds_.fill(-1); }
ThreadPerfCounters::~ThreadPerfCounters() = default;
void ThreadPerfCounters::Start() {}
PerfTotals ThreadPerfCounters::Stop() { return {}; }

#endif

}  // namespace talus::bench
