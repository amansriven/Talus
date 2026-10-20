#pragma once

// Hardware performance counters for one thread (spec §12), via Linux
// perf_event_open: cycles, instructions, L1d read misses, L2 load misses
// (Intel only), last-level-cache misses and branch misses. Each worker thread opens its own set, so
// the counts cover exactly the benchmarked operations and exclude background flush and compaction
// threads. Counters the CPU or VM does not provide are skipped. On other platforms nothing is
// available and the JSON says so.

#include <array>
#include <cstdint>
#include <string>

namespace talus::bench {

enum PerfEvent : int {
  kCycles,
  kInstructions,
  kL1dMisses,
  // Retired loads that missed L2 (MEM_LOAD_RETIRED.L2_MISS). A core event, so
  // VMs whose PMU hides L3 events (GCP's STANDARD level) still report it.
  kL2Misses,
  kLlcMisses,
  kBranchMisses,
  kNumPerfEvents,
};

// "cycles", "l1d_misses", ...: the JSON key prefix for each event.
const char* PerfEventName(int event);

struct PerfTotals {
  bool collected = false;
  std::array<uint64_t, kNumPerfEvents> values{};
  std::array<bool, kNumPerfEvents> available{};

  void Add(const PerfTotals& other);
  // {"available":..., "cycles_per_op":..., "ipc":..., ...}
  std::string ToJson(uint64_t ops) const;
};

class ThreadPerfCounters {
 public:
  ThreadPerfCounters();  // Opens the counters for the calling thread (disabled).
  ~ThreadPerfCounters();

  ThreadPerfCounters(const ThreadPerfCounters&) = delete;
  ThreadPerfCounters& operator=(const ThreadPerfCounters&) = delete;

  void Start();
  PerfTotals Stop();

 private:
  std::array<int, kNumPerfEvents> fds_;
};

}  // namespace talus::bench
