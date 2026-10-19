#pragma once

// Shared benchmark harness for db_bench and ycsb (spec §12):
//  * --name=value flags, and matched engine settings built from them;
//  * HdrHistogram latency recording, per thread, merged at the end, plus a
//    histogram per second for the latency time series;
//  * closed-loop runs (each thread issues its next operation as soon as the
//    last completes) and open-loop runs (operations are scheduled at a fixed
//    total arrival rate, and latency is measured from each operation's
//    *scheduled* start, so a stall's queueing delay is counted instead of
//    hidden: no coordinated omission);
//  * hardware counters per worker thread (Linux);
//  * JSON output.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "engine.h"
#include "perf_counters.h"

struct hdr_histogram;

namespace talus::bench {

class Flags {
 public:
  Flags(int argc, char** argv);

  std::string Str(const std::string& name, const std::string& fallback) const;
  int64_t Int(const std::string& name, int64_t fallback) const;
  double Double(const std::string& name, double fallback) const;
  bool Bool(const std::string& name, bool fallback) const;
  // Exits with an error listing any flag that was given but never read.
  void CheckAllUsed() const;
  std::string ToJson() const;

 private:
  std::map<std::string, std::string> values_;
  mutable std::set<std::string> used_;
};

// Engine settings from flags (shared by db_bench and ycsb).
EngineConfig EngineConfigFromFlags(const Flags& flags);

// A latency histogram in nanoseconds (HdrHistogram).
class Histogram {
 public:
  explicit Histogram(int significant_figures = 3);
  ~Histogram();
  Histogram(Histogram&& other) noexcept;
  Histogram& operator=(Histogram&& other) noexcept;
  Histogram(const Histogram&) = delete;
  Histogram& operator=(const Histogram&) = delete;

  void Record(uint64_t nanos);
  void Add(const Histogram& other);
  uint64_t Count() const;
  uint64_t Percentile(double p) const;
  uint64_t Max() const;
  double Mean() const;
  // {"count","mean","p50","p90","p99","p999","p9999","max"} and, if
  // with_cdf, "cdf": [[nanos, percentile], ...].
  std::string ToJson(bool with_cdf) const;

 private:
  hdr_histogram* h_ = nullptr;
};

struct RunSpec {
  int threads = 1;
  uint64_t ops_per_thread = 0;
  uint64_t extra_ops = 0;  // One additional operation on each of the first N threads.
  double target_rate = 0;  // Total ops/s across threads; 0 = closed loop.
  double max_seconds = 0;  // Stop early after this long (0 = no limit).
  uint64_t warmup_ops_per_thread = 0;
  bool pin_cpus = false;      // Pin worker t to CPU t (Linux).
  uint64_t bytes_per_op = 0;  // For MB/s.
  // Optional unmeasured thread (e.g. the writer in readwhilewriting); it
  // must return once `stop` becomes true.
  std::function<void(const std::atomic<bool>& stop)> background;
  // Optional: called once per second, its JSON kept as a time series (e.g.
  // engine stats, for write amplification over time).
  std::function<std::string()> stats_sampler;
};

// One operation: (thread, op index, thread's RNG) -> success.
using OpFn = std::function<bool(int thread, uint64_t index, std::mt19937_64& rng)>;

struct SecondStats {
  uint64_t ops = 0;
  Histogram latency{2};
};

struct RunResult {
  uint64_t ops = 0;
  uint64_t errors = 0;
  double seconds = 0;
  uint64_t bytes_per_op = 0;
  Histogram latency;
  std::vector<SecondStats> per_second;
  PerfTotals perf;
  std::string initial_stats = "{}";       // After warmup, before measured operations.
  std::vector<std::string> stats_series;  // JSON, one per second.

  double OpsPerSec() const { return seconds > 0 ? static_cast<double>(ops) / seconds : 0; }
  std::string ToJson() const;
};

std::vector<int> BenchmarkCpus();

RunResult RunWorkload(const RunSpec& spec, const OpFn& op);

// Median and standard deviation of throughput and latency percentiles
// across repetitions.
std::string SummarizeJson(const std::vector<RunResult>& runs);

// Timer source and calibration, CPU count and similar context. The Python
// scripts add full hardware details (model, SMT, RAM, disk, governor).
std::string EnvironmentJson();

// Writes `json` to `path`, or to stdout if path is empty or "-".
void WriteJson(const std::string& path, const std::string& json);

// A zero-padded decimal key of exactly `size` bytes (truncated on the left
// if the number has more digits).
std::string MakeKey(uint64_t n, int size);

// A pool of random value bytes; values are slices of it.
class ValueSource {
 public:
  explicit ValueSource(size_t value_size);
  std::string_view Next(std::mt19937_64& rng) const;

 private:
  size_t value_size_;
  std::string pool_;
};

}  // namespace talus::bench
