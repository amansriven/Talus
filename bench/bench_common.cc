#include "bench_common.h"

#include <hdr/hdr_histogram.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#endif

#include "timer.h"
#include "util/port.h"

namespace talus::bench {

// ---- Flags --------------------------------------------------------------------

Flags::Flags(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const size_t eq = arg.find('=');
    if (arg.rfind("--", 0) != 0 || eq == std::string::npos) {
      std::fprintf(stderr, "bad flag '%s' (expected --name=value)\n", arg.c_str());
      std::exit(2);
    }
    values_[arg.substr(2, eq - 2)] = arg.substr(eq + 1);
  }
}

std::string Flags::Str(const std::string& name, const std::string& fallback) const {
  used_.insert(name);
  auto it = values_.find(name);
  return it != values_.end() ? it->second : fallback;
}

int64_t Flags::Int(const std::string& name, int64_t fallback) const {
  const std::string v = Str(name, "");
  if (v.empty()) return fallback;
  char* end = nullptr;
  errno = 0;
  const auto result = std::strtoll(v.c_str(), &end, 10);
  if (errno == ERANGE || end == v.c_str() || *end != '\0') {
    std::fprintf(stderr, "invalid integer --%s=%s\n", name.c_str(), v.c_str());
    std::exit(2);
  }
  return result;
}

double Flags::Double(const std::string& name, double fallback) const {
  const std::string v = Str(name, "");
  if (v.empty()) return fallback;
  char* end = nullptr;
  errno = 0;
  const double result = std::strtod(v.c_str(), &end);
  if (errno == ERANGE || end == v.c_str() || *end != '\0' || !std::isfinite(result)) {
    std::fprintf(stderr, "invalid number --%s=%s\n", name.c_str(), v.c_str());
    std::exit(2);
  }
  return result;
}

bool Flags::Bool(const std::string& name, bool fallback) const {
  const std::string v = Str(name, "");
  if (v.empty()) return fallback;
  if (v == "1" || v == "true") return true;
  if (v == "0" || v == "false") return false;
  std::fprintf(stderr, "invalid boolean --%s=%s\n", name.c_str(), v.c_str());
  std::exit(2);
}

void Flags::CheckAllUsed() const {
  bool bad = false;
  for (const auto& [name, value] : values_) {
    if (used_.count(name) == 0) {
      std::fprintf(stderr, "unknown flag --%s\n", name.c_str());
      bad = true;
    }
  }
  if (bad) {
    std::exit(2);
  }
}

std::string Flags::ToJson() const {
  std::string json = "{";
  for (const auto& [name, value] : values_) {
    json += (json.size() > 1 ? "," : "") + JsonString(name) + ":" + JsonString(value);
  }
  return json + "}";
}

EngineConfig EngineConfigFromFlags(const Flags& f) {
  EngineConfig c;
  for (const char* name : {"memtable_size", "block_size", "cache_size", "target_file_size",
                           "l1_max_bytes", "rate_limit", "spin", "bloom_bits"}) {
    if (f.Int(name, 0) < 0) {
      std::fprintf(stderr, "--%s must be nonnegative\n", name);
      std::exit(2);
    }
  }
  c.path = f.Str("db", "/tmp/talus_bench");
  c.fresh = !f.Bool("use_existing_db", false);
  c.sync = f.Bool("sync", false);
  c.memtable_size = static_cast<uint64_t>(f.Int("memtable_size", 4 << 20));
  c.block_size = static_cast<uint64_t>(f.Int("block_size", 4 << 10));
  c.bloom_bits_per_key = static_cast<int>(f.Int("bloom_bits", 10));
  c.block_cache_size = static_cast<uint64_t>(f.Int("cache_size", 8 << 20));
  c.max_open_files = static_cast<int>(f.Int("max_open_files", 1000));
  c.target_file_size = static_cast<uint64_t>(f.Int("target_file_size", 2 << 20));
  c.l1_max_bytes = static_cast<uint64_t>(f.Int("l1_max_bytes", 10 << 20));
  c.max_background_compactions = static_cast<int>(f.Int("max_background_compactions", 1));
  c.blocked_bloom = f.Bool("blocked_bloom", true);
  c.mmap_reads = f.Bool("mmap_reads", false);
  c.huge_pages = f.Bool("huge_pages", false);
  c.group_commit = f.Bool("group_commit", true);
  c.concurrent_memtable = f.Bool("concurrent_memtable", true);
  c.tls_super_version = f.Bool("tls_super_version", true);
  c.compaction_rate_limit = static_cast<uint64_t>(f.Int("rate_limit", 0));
  c.write_spin_iterations = static_cast<int>(f.Int("spin", 256));
  const std::string cpus = f.Str("background_cpus", "");
  std::stringstream ss(cpus);
  for (std::string cpu; std::getline(ss, cpu, ',');) {
    if (!cpu.empty()) {
      c.background_cpus.push_back(std::atoi(cpu.c_str()));
    }
  }
  if (c.memtable_size == 0 || c.block_size == 0 || c.target_file_size == 0 || c.l1_max_bytes == 0 ||
      c.max_open_files <= 0 || c.max_background_compactions <= 0) {
    std::fprintf(stderr, "engine sizes and worker counts must be positive\n");
    std::exit(2);
  }
  return c;
}

// ---- Histogram ------------------------------------------------------------------

namespace {
// Keep long open-loop queues visible rather than clipping tails at 60 seconds.
// Half INT64_MAX leaves room for HdrHistogram's internal bucket rounding.
constexpr int64_t kMaxTrackableNanos = std::numeric_limits<int64_t>::max() / 2;
}  // namespace

Histogram::Histogram(int significant_figures) {
  if (hdr_init(1, kMaxTrackableNanos, significant_figures, &h_) != 0) {
    std::fprintf(stderr, "hdr_init failed\n");
    std::abort();
  }
}

Histogram::~Histogram() {
  if (h_ != nullptr) {
    hdr_close(h_);
  }
}

Histogram::Histogram(Histogram&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}

Histogram& Histogram::operator=(Histogram&& other) noexcept {
  if (this != &other) {
    if (h_ != nullptr) {
      hdr_close(h_);
    }
    h_ = std::exchange(other.h_, nullptr);
  }
  return *this;
}

void Histogram::Record(uint64_t nanos) {
  hdr_record_value(h_, static_cast<int64_t>(std::clamp<uint64_t>(nanos, 1, kMaxTrackableNanos)));
}

void Histogram::Add(const Histogram& other) { hdr_add(h_, other.h_); }
uint64_t Histogram::Count() const { return static_cast<uint64_t>(h_->total_count); }
uint64_t Histogram::Percentile(double p) const {
  return static_cast<uint64_t>(hdr_value_at_percentile(h_, p));
}
uint64_t Histogram::Max() const { return static_cast<uint64_t>(hdr_max(h_)); }
double Histogram::Mean() const { return hdr_mean(h_); }

std::string Histogram::ToJson(bool with_cdf) const {
  std::ostringstream o;
  o << "{\"count\":" << Count() << ",\"mean\":" << Mean() << ",\"p50\":" << Percentile(50)
    << ",\"p90\":" << Percentile(90) << ",\"p99\":" << Percentile(99)
    << ",\"p999\":" << Percentile(99.9) << ",\"p9999\":" << Percentile(99.99)
    << ",\"max\":" << Max();
  if (with_cdf && Count() > 0) {
    o << ",\"cdf\":[";
    hdr_iter iter;
    hdr_iter_percentile_init(&iter, h_, 5);  // Denser toward the tail.
    bool first = true;
    while (hdr_iter_next(&iter)) {
      o << (first ? "" : ",") << "[" << iter.highest_equivalent_value << ","
        << iter.specifics.percentiles.percentile << "]";
      first = false;
    }
    o << "]";
  }
  o << "}";
  return o.str();
}

// ---- Runner ---------------------------------------------------------------------

namespace {

struct ThreadOutput {
  uint64_t ops = 0;
  uint64_t errors = 0;
  uint64_t end_ticks = 0;
  Histogram latency;
  std::vector<SecondStats> per_second;
  PerfTotals perf;
};

void RecordOp(ThreadOutput* out, uint64_t t0, uint64_t start_ticks, uint64_t end_ticks) {
  const uint64_t nanos = CycleClock::ToNanos(end_ticks > start_ticks ? end_ticks - start_ticks : 0);
  out->latency.Record(nanos);
  const auto second = static_cast<size_t>(CycleClock::ToNanos(end_ticks - t0) / 1'000'000'000ULL);
  if (out->per_second.size() <= second) {
    out->per_second.resize(second + 1);
  }
  out->per_second[second].ops++;
  out->per_second[second].latency.Record(nanos);
}

// Waits until the cycle counter reaches `target`: sleeps while far away,
// spins for the last stretch.
void WaitUntil(uint64_t target) {
  while (true) {
    const uint64_t now = CycleClock::Now();
    if (now >= target) {
      return;
    }
    const uint64_t remaining_ns = CycleClock::ToNanos(target - now);
    if (remaining_ns > 200'000) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(remaining_ns - 100'000));
    } else {
      CpuRelax();
    }
  }
}

}  // namespace

std::vector<int> BenchmarkCpus() {
  std::vector<int> cpus;
#if defined(__linux__)
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
    std::perror("sched_getaffinity");
    std::exit(1);
  }
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &allowed)) cpus.push_back(cpu);
  }
#endif
  return cpus;
}

RunResult RunWorkload(const RunSpec& spec, const OpFn& op) {
  const auto cpus = BenchmarkCpus();
  const double ticks_per_second = 1e9 / CycleClock::NanosPerTick();
  std::vector<ThreadOutput> outputs(static_cast<size_t>(spec.threads));
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> t0_shared{0};

  std::vector<std::thread> workers;
  for (int t = 0; t < spec.threads; ++t) {
    workers.emplace_back([&, t] {
      if (spec.pin_cpus) {
#if defined(__linux__)
        cpu_set_t selected;
        CPU_ZERO(&selected);
        CPU_SET(cpus[static_cast<size_t>(t) % cpus.size()], &selected);
        if (sched_setaffinity(0, sizeof(selected), &selected) != 0) {
          std::perror("benchmark CPU affinity");
          std::exit(1);
        }
#endif
      }
      std::mt19937_64 rng(0x9E3779B97F4A7C15ULL * static_cast<uint64_t>(t + 1));
      for (uint64_t i = 0; i < spec.warmup_ops_per_thread; ++i) {
        (void)op(t, i, rng);
      }
      ThreadPerfCounters perf;
      ThreadOutput& out = outputs[static_cast<size_t>(t)];
      // Release/acquire: the main thread reads `ready` to know all workers
      // have warmed up; workers acquire `go` to see the start time t0.
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
        CpuRelax();
      }
      const uint64_t t0 = t0_shared.load(std::memory_order_relaxed);  // Ordered by `go`.
      const auto max_ticks = static_cast<uint64_t>(spec.max_seconds * ticks_per_second);
      const double interval_ticks =
          spec.target_rate > 0 ? ticks_per_second * spec.threads / spec.target_rate : 0;
      const double phase_ticks = spec.target_rate > 0 ? ticks_per_second * t / spec.target_rate : 0;

      perf.Start();
      for (uint64_t i = 0;
           i < spec.ops_per_thread + (static_cast<uint64_t>(t) < spec.extra_ops ? 1 : 0); ++i) {
        uint64_t start = 0;
        if (spec.target_rate > 0) {
          // Open loop: latency runs from the scheduled start, so time spent
          // waiting behind a slow operation is counted.
          start = t0 + static_cast<uint64_t>(phase_ticks + interval_ticks * static_cast<double>(i));
          if (max_ticks > 0 && start - t0 >= max_ticks) break;
          WaitUntil(start);
        } else {
          start = CycleClock::Now();
        }
        if (!op(t, i, rng)) {
          ++out.errors;
        }
        const uint64_t end = CycleClock::Now();
        RecordOp(&out, t0, start, end);
        ++out.ops;
        if (max_ticks > 0 && (i & 63) == 0 && end - t0 > max_ticks) {
          break;
        }
      }
      out.perf = perf.Stop();
      out.end_ticks = CycleClock::Now();
    });
  }

  while (ready.load(std::memory_order_acquire) < spec.threads) {
    std::this_thread::yield();
  }
  const std::string initial_stats = spec.stats_sampler ? spec.stats_sampler() : "{}";
  std::thread background;
  if (spec.background) {
    background = std::thread([&] { spec.background(stop); });
  }
  std::vector<std::string> stats_series;
  std::thread sampler;
  const uint64_t t0 = CycleClock::Now();
  if (spec.stats_sampler) {
    sampler = std::thread([&] {
      for (int second = 1; !stop.load(std::memory_order_acquire); ++second) {
        // Sleep in small steps so the sampler notices `stop` promptly.
        const uint64_t target = t0 + static_cast<uint64_t>(ticks_per_second * second);
        while (CycleClock::Now() < target && !stop.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!stop.load(std::memory_order_acquire)) {
          stats_series.push_back(spec.stats_sampler());
        }
      }
    });
  }
  t0_shared.store(t0, std::memory_order_relaxed);
  go.store(true, std::memory_order_release);
  for (auto& w : workers) {
    w.join();
  }
  // Release: pairs with the background and sampler threads' acquire loads.
  stop.store(true, std::memory_order_release);
  if (background.joinable()) {
    background.join();
  }
  if (sampler.joinable()) {
    sampler.join();
  }

  RunResult result;
  result.initial_stats = initial_stats;
  result.bytes_per_op = spec.bytes_per_op;
  result.stats_series = std::move(stats_series);
  uint64_t end = t0;
  for (ThreadOutput& out : outputs) {
    result.ops += out.ops;
    result.errors += out.errors;
    result.latency.Add(out.latency);
    result.perf.Add(out.perf);
    end = std::max(end, out.end_ticks);
    if (result.per_second.size() < out.per_second.size()) {
      result.per_second.resize(out.per_second.size());
    }
    for (size_t s = 0; s < out.per_second.size(); ++s) {
      result.per_second[s].ops += out.per_second[s].ops;
      result.per_second[s].latency.Add(out.per_second[s].latency);
    }
  }
  result.seconds = static_cast<double>(CycleClock::ToNanos(end - t0)) / 1e9;
  return result;
}

std::string RunResult::ToJson() const {
  std::ostringstream o;
  o << "{\"initial_engine_stats\":" << initial_stats << ",\"ops\":" << ops
    << ",\"errors\":" << errors << ",\"seconds\":" << seconds << ",\"ops_per_sec\":" << OpsPerSec()
    << ",\"mb_per_sec\":" << OpsPerSec() * static_cast<double>(bytes_per_op) / 1048576.0
    << ",\"latency_ns\":" << latency.ToJson(/*with_cdf=*/true) << ",\"timeseries\":[";
  for (size_t s = 0; s < per_second.size(); ++s) {
    const Histogram& h = per_second[s].latency;
    o << (s > 0 ? "," : "") << "{\"second\":" << s << ",\"ops\":" << per_second[s].ops
      << ",\"p50\":" << h.Percentile(50) << ",\"p99\":" << h.Percentile(99)
      << ",\"p90\":" << h.Percentile(90) << ",\"p999\":" << h.Percentile(99.9)
      << ",\"p9999\":" << h.Percentile(99.99) << ",\"max\":" << h.Max() << "}";
  }
  o << "],\"perf\":" << perf.ToJson(ops) << ",\"stats_series\":[";
  for (size_t s = 0; s < stats_series.size(); ++s) {
    o << (s > 0 ? "," : "") << "{\"second\":" << s + 1 << ",\"stats\":" << stats_series[s] << "}";
  }
  o << "]}";
  return o.str();
}

std::string SummarizeJson(const std::vector<RunResult>& runs) {
  auto stats = [&](auto&& metric) {
    std::vector<double> v;
    for (const RunResult& r : runs) {
      v.push_back(metric(r));
    }
    std::sort(v.begin(), v.end());
    const double median =
        v.empty()
            ? 0
            : (v.size() % 2 == 1 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2);
    double mean = 0;
    for (double x : v) {
      mean += x;
    }
    mean = v.empty() ? 0 : mean / static_cast<double>(v.size());
    double var = 0;
    for (double x : v) {
      var += (x - mean) * (x - mean);
    }
    const double stddev = v.size() > 1 ? std::sqrt(var / static_cast<double>(v.size() - 1)) : 0;
    std::ostringstream o;
    o << "{\"median\":" << median << ",\"stddev\":" << stddev << "}";
    return o.str();
  };
  std::ostringstream o;
  o << "{\"repetitions\":" << runs.size()
    << ",\"ops_per_sec\":" << stats([](const RunResult& r) { return r.OpsPerSec(); })
    << ",\"mb_per_sec\":" << stats([](const RunResult& r) {
         return r.OpsPerSec() * static_cast<double>(r.bytes_per_op) / 1048576.0;
       })
    << ",\"p90_ns\":" << stats([](const RunResult& r) { return double(r.latency.Percentile(90)); })
    << ",\"p50_ns\":" << stats([](const RunResult& r) { return double(r.latency.Percentile(50)); })
    << ",\"p99_ns\":" << stats([](const RunResult& r) { return double(r.latency.Percentile(99)); })
    << ",\"p999_ns\":"
    << stats([](const RunResult& r) { return double(r.latency.Percentile(99.9)); })
    << ",\"p9999_ns\":"
    << stats([](const RunResult& r) { return double(r.latency.Percentile(99.99)); })
    << ",\"max_ns\":" << stats([](const RunResult& r) { return double(r.latency.Max()); }) << "}";
  return o.str();
}

std::string EnvironmentJson() {
  std::ostringstream o;
  o << "{\"timer\":" << JsonString(CycleClock::Source())
    << ",\"timer_ns_per_tick\":" << CycleClock::NanosPerTick()
    << ",\"hardware_concurrency\":" << std::thread::hardware_concurrency() << ",\"engines\":[";
  const auto engines = AvailableEngines();
  for (size_t i = 0; i < engines.size(); ++i) {
    o << (i > 0 ? "," : "") << JsonString(engines[i]);
  }
  o << "],\"affinity_supported\":";
#if defined(__linux__)
  o << "true";
#else
  o << "false";
#endif
  o << ",\"allowed_cpus\":[";
  const auto cpus = BenchmarkCpus();
  for (size_t i = 0; i < cpus.size(); ++i) o << (i ? "," : "") << cpus[i];
  o << "]}";
  return o.str();
}

void WriteJson(const std::string& path, const std::string& json) {
  if (path.empty() || path == "-") {
    std::cout << json << "\n";
    return;
  }
  std::ofstream out(path);
  out << json << "\n";
  out.close();
  if (!out) {
    std::fprintf(stderr, "cannot write JSON to %s\n", path.c_str());
    std::exit(1);
  }
  std::fprintf(stderr, "wrote %s\n", path.c_str());
}

std::string MakeKey(uint64_t n, int size) {
  std::string digits = std::to_string(n);
  if (static_cast<int>(digits.size()) >= size) {
    return digits.substr(digits.size() - static_cast<size_t>(size));
  }
  return std::string(static_cast<size_t>(size) - digits.size(), '0') + digits;
}

ValueSource::ValueSource(size_t value_size) : value_size_(value_size) {
  std::mt19937_64 rng(301);
  pool_.resize(std::max<size_t>(1 << 20, value_size * 4));
  for (char& c : pool_) {
    c = static_cast<char>(' ' + rng() % 95);
  }
}

std::string_view ValueSource::Next(std::mt19937_64& rng) const {
  const size_t offset = rng() % (pool_.size() - value_size_);
  return std::string_view(pool_).substr(offset, value_size_);
}

}  // namespace talus::bench
