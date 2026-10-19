// YCSB core workloads A-F (spec §12), with the same load modes and JSON
// output as db_bench.
//
//   ycsb --workload=a --records=1000000 --operations=1000000 --distribution=zipfian --threads=8 --engine=talus --output=a.json
//
//   A  50% read, 50% update            (update heavy)
//   B  95% read,  5% update            (read mostly)
//   C 100% read                        (read only)
//   D  95% read,  5% insert, reads favor recent inserts ("latest")
//   E  95% scan,  5% insert            (short ranges, 1..--max_scan_length)
//   F  50% read, 50% read-modify-write
//
// Keys are "user" + a hash of the record number, as in YCSB, so inserts are
// not sequential. --distribution=zipfian (default, theta 0.99, scrambled so
// that popular keys are spread across the key space) or uniform; workload D
// always uses "latest".

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "bench_common.h"

namespace talus::bench {
namespace {

uint64_t Fnv64(uint64_t v) {
  uint64_t h = 0xCBF29CE484222325ULL;
  for (int i = 0; i < 8; ++i) {
    h ^= v & 0xff;
    h *= 0x100000001B3ULL;
    v >>= 8;
  }
  return h;
}

std::string RecordKey(uint64_t n) {
  return "user" + MakeKey(Fnv64(n) % 10'000'000'000'000'000'000ULL, 19);
}

// YCSB's ZipfianGenerator (Gray et al., "Quickly generating billion-record
// synthetic databases"): item i is drawn with probability proportional to
// 1 / (i+1)^theta.
class Zipfian {
 public:
  Zipfian(uint64_t items, double theta = 0.99) : items_(items), theta_(theta) {
    if (items <= 2) return;
    zeta_n_ = Zeta(items, theta);
    const double zeta2 = Zeta(2, theta);
    alpha_ = 1.0 / (1.0 - theta);
    eta_ = (1 - std::pow(2.0 / static_cast<double>(items), 1 - theta)) / (1 - zeta2 / zeta_n_);
  }

  uint64_t Next(std::mt19937_64& rng) const {
    if (items_ <= 2) return rng() % items_;
    const double u = std::uniform_real_distribution<double>(0, 1)(rng);
    const double uz = u * zeta_n_;
    if (uz < 1.0) {
      return 0;
    }
    if (uz < 1.0 + std::pow(0.5, theta_)) {
      return 1;
    }
    return static_cast<uint64_t>(static_cast<double>(items_) *
                                 std::pow(eta_ * u - eta_ + 1, alpha_));
  }

 private:
  static double Zeta(uint64_t n, double theta) {
    double sum = 0;
    for (uint64_t i = 0; i < n; ++i) {
      sum += 1.0 / std::pow(static_cast<double>(i + 1), theta);
    }
    return sum;
  }

  uint64_t items_;
  double theta_;
  double zeta_n_ = 0;
  double alpha_ = 0;
  double eta_ = 0;
};

struct Mix {
  double read = 0;
  double update = 0;
  double insert = 0;
  double scan = 0;
  double rmw = 0;  // Read-modify-write.
};

Mix MixFor(char workload) {
  switch (workload) {
    case 'a':
      return {.read = 0.5, .update = 0.5};
    case 'b':
      return {.read = 0.95, .update = 0.05};
    case 'c':
      return {.read = 1.0};
    case 'd':
      return {.read = 0.95, .insert = 0.05};
    case 'e':
      return {.insert = 0.05, .scan = 0.95};
    case 'f':
      return {.read = 0.5, .rmw = 0.5};
    default:
      std::fprintf(stderr, "unknown workload '%c' (use a-f)\n", workload);
      std::exit(2);
  }
}

int Main(int argc, char** argv) {
  const Flags flags(argc, argv);
  const std::string workload_name = flags.Str("workload", "a");
  const char workload =
      static_cast<char>(std::tolower(workload_name.empty() ? 'a' : workload_name[0]));
  const auto records = static_cast<uint64_t>(flags.Int("records", 1'000'000));
  const auto operations = static_cast<uint64_t>(flags.Int("operations", 1'000'000));
  const std::string distribution = flags.Str("distribution", "zipfian");
  const int threads = static_cast<int>(flags.Int("threads", 1));
  const auto value_size = static_cast<size_t>(flags.Int("value_size", 1000));
  const auto max_scan = static_cast<uint64_t>(flags.Int("max_scan_length", 100));
  const double rate = flags.Double("rate", 0);
  const double duration = flags.Double("duration", 0);
  const auto warmup = static_cast<uint64_t>(flags.Int("warmup_ops", 0));
  const int repetitions = static_cast<int>(flags.Int("repetitions", 1));
  const bool pin = flags.Bool("pin_cpus", false);
  const bool skip_load = flags.Bool("skip_load", false);
  const std::string engine_name = flags.Str("engine", "talus");
  const std::string output = flags.Str("output", "");
  EngineConfig ec = EngineConfigFromFlags(flags);
  flags.CheckAllUsed();
  if (distribution != "zipfian" && distribution != "uniform") {
    std::fprintf(stderr, "--distribution must be zipfian or uniform\n");
    return 2;
  }
  if (threads <= 0 || records < static_cast<uint64_t>(threads) || records > INT64_MAX ||
      operations == 0 || operations > INT64_MAX || max_scan == 0 || max_scan > INT64_MAX ||
      value_size > (1ULL << 30) || repetitions <= 0 || rate < 0 || duration < 0 ||
      warmup > INT64_MAX) {
    std::fprintf(stderr, "invalid YCSB counts, sizes, rate or duration\n");
    return 2;
  }
  const Mix mix = MixFor(workload);
  const ValueSource values(value_size);

  std::string error;
  ec.fresh = !skip_load;
  std::unique_ptr<Engine> engine = OpenEngine(engine_name, ec, &error);
  if (engine == nullptr) {
    std::fprintf(stderr, "cannot open %s: %s\n", engine_name.c_str(), error.c_str());
    return 1;
  }
  Engine* e = engine.get();

  // Load phase: insert every record (measured too, as "load").
  bool had_errors = false;
  std::string results;
  if (!skip_load) {
    RunSpec load;
    load.threads = threads;
    load.ops_per_thread = records / static_cast<uint64_t>(threads);
    load.extra_ops = records % static_cast<uint64_t>(threads);
    load.pin_cpus = pin;
    load.bytes_per_op = 23 + value_size;
    const uint64_t per_thread = load.ops_per_thread;
    RunResult r = RunWorkload(load, [&](int t, uint64_t i, std::mt19937_64& rng) {
      return e->Put(RecordKey(static_cast<uint64_t>(t) * per_thread +
                              std::min(static_cast<uint64_t>(t), load.extra_ops) + i),
                    values.Next(rng));
    });
    had_errors = had_errors || r.errors != 0;
    e->WaitForCompactions();
    std::fprintf(stderr, "load: %llu records, %.0f ops/s\n", static_cast<unsigned long long>(r.ops),
                 r.OpsPerSec());
    std::vector<RunResult> one;
    one.push_back(std::move(r));
    results = "{\"benchmark\":\"load\",\"summary\":" + SummarizeJson(one) + ",\"repetitions\":[" +
              one[0].ToJson() + "]}";
  }

  // Run phase.
  const Zipfian zipf(records);
  std::atomic<uint64_t> inserted{records};  // Next record number to insert.
  auto pick_existing = [&](std::mt19937_64& rng) -> uint64_t {
    // Relaxed: only an approximate bound on the key space is needed.
    const uint64_t count = inserted.load(std::memory_order_relaxed);
    if (workload == 'd') {  // "Latest": the newest records are the hottest.
      return count - 1 - std::min(count - 1, zipf.Next(rng));
    }
    if (distribution == "uniform") {
      return rng() % count;
    }
    return Fnv64(zipf.Next(rng)) % count;  // Scrambled zipfian.
  };

  std::vector<RunResult> runs;
  std::string reps_json;
  for (int rep = 0; rep < repetitions; ++rep) {
    RunSpec spec;
    spec.threads = threads;
    spec.ops_per_thread = operations / static_cast<uint64_t>(threads);
    spec.extra_ops = operations % static_cast<uint64_t>(threads);
    spec.target_rate = rate;
    spec.max_seconds = duration;
    spec.warmup_ops_per_thread = warmup;
    spec.pin_cpus = pin;
    spec.bytes_per_op = 23 + value_size;
    spec.stats_sampler = [e] { return e->StatsJson(); };
    RunResult r = RunWorkload(spec, [&](int, uint64_t, std::mt19937_64& rng) {
      const double dice = std::uniform_real_distribution<double>(0, 1)(rng);
      double edge = mix.read;
      if (dice < edge) {
        return e->Get(RecordKey(pick_existing(rng))) >= 0;
      }
      if (dice < (edge += mix.update)) {
        return e->Put(RecordKey(pick_existing(rng)), values.Next(rng));
      }
      if (dice < (edge += mix.insert)) {
        // Relaxed: only uniqueness of the new record number matters.
        return e->Put(RecordKey(inserted.fetch_add(1, std::memory_order_relaxed)),
                      values.Next(rng));
      }
      if (dice < (edge += mix.scan)) {
        auto iter = e->NewIterator();
        const uint64_t length = 1 + rng() % max_scan;
        iter->Seek(RecordKey(pick_existing(rng)));
        for (uint64_t i = 0; i < length && iter->Valid(); ++i, iter->Next()) {
          volatile size_t sink = iter->value().size();
          (void)sink;
        }
        return iter->ok();
      }
      // Read-modify-write.
      const std::string key = RecordKey(pick_existing(rng));
      std::string value;
      if (e->GetCopy(key, &value) < 0) {
        return false;
      }
      return e->Put(key, values.Next(rng));
    });
    had_errors = had_errors || r.errors != 0;
    e->WaitForCompactions();
    std::fprintf(stderr,
                 "workload %c rep %d: %.0f ops/s  p50 %.2f us  p99 %.2f us  p99.9 %.2f us\n",
                 workload, rep + 1, r.OpsPerSec(), r.latency.Percentile(50) / 1e3,
                 r.latency.Percentile(99) / 1e3, r.latency.Percentile(99.9) / 1e3);
    reps_json +=
        (rep > 0 ? "," : "") + r.ToJson().insert(1, "\"engine_stats\":" + e->StatsJson() + ",");
    runs.push_back(std::move(r));
  }
  results += (results.empty() ? "" : ",") + std::string("{\"benchmark\":\"workload_") + workload +
             "\",\"summary\":" + SummarizeJson(runs) + ",\"repetitions\":[" + reps_json + "]}";
  engine.reset();

  WriteJson(output,
            "{\"tool\":\"ycsb\",\"engine\":" + JsonString(engine_name) +
                ",\"workload\":" + JsonString(std::string(1, workload)) +
                ",\"distribution\":" + JsonString(workload == 'd' ? "latest" : distribution) +
                ",\"load\":" + (rate > 0 ? "\"open\"" : "\"closed\"") +
                ",\"flags\":" + flags.ToJson() + ",\"engine_config\":" + ec.ToJson() +
                ",\"environment\":" + EnvironmentJson() + ",\"results\":[" + results + "]}");
  return had_errors ? 1 : 0;
}

}  // namespace
}  // namespace talus::bench

int main(int argc, char** argv) { return talus::bench::Main(argc, argv); }
