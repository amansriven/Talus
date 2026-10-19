// db_bench: LevelDB-style microbenchmarks over a key-value engine (spec §12).
//
//   db_bench --benchmarks=fillseq,readrandom --num=1000000 --threads=4 --engine=talus --repetitions=3 --output=result.json
//
// Workloads:
//   fillseq          write keys 0..num-1 in order (fresh DB)
//   fillrandom       write random keys (fresh DB)
//   overwrite        write random existing keys
//   readrandom       read random keys (all present after fillseq)
//   readmissing      read keys that do not exist but sort inside the key range
//   seekrandom       seek to a random key and read the entry there
//   readseq          scan the whole DB in order, one entry per operation
//   readwhilewriting readrandom on --threads readers while one extra,
//                    unmeasured thread overwrites random keys
//
// Load: closed loop by default; --rate=N runs open loop at N total ops/s,
// with latency measured from each operation's scheduled start.
//
// Flags: --num (key space; also the write count), --reads (read count,
// default --num), --key_size (16), --value_size (100), --threads,
// --sync, --engine=talus|leveldb|rocksdb, --warmup_ops (per thread),
// --repetitions, --pin_cpus, --rate, --duration (seconds cap), --db,
// --output, plus the engine settings in bench_common.cc.

#include <algorithm>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "bench_common.h"

namespace talus::bench {
namespace {

struct Config {
  uint64_t num = 1'000'000;  // Key space (and the number of write operations).
  uint64_t reads = 0;        // Read operations; 0 = num.
  int key_size = 16;
  size_t value_size = 100;
  int threads = 1;
  uint64_t warmup_ops = 0;
  int repetitions = 1;
  bool pin_cpus = false;
  double rate = 0;
  double duration = 0;
  std::string engine = "talus";
};

class Bench {
 public:
  Bench(const Config& c, const EngineConfig& ec) : c_(c), ec_(ec), values_(c.value_size) {}

  // Runs one workload `repetitions` times; returns its JSON.
  std::string Run(const std::string& name) {
    const bool fresh_each_rep = name == "fillseq" || name == "fillrandom";
    std::vector<RunResult> runs;
    std::string reps_json;
    for (int rep = 0; rep < c_.repetitions; ++rep) {
      if (fresh_each_rep || engine_ == nullptr) {
        EngineConfig ec = ec_;
        ec.fresh = fresh_each_rep ? true : ec_.fresh;
        Reopen(ec);
      }
      RunResult r = RunOnce(name);
      had_errors_ = had_errors_ || r.errors != 0;
      engine_->WaitForCompactions();  // Each repetition starts from a settled DB.
      reps_json += (rep > 0 ? "," : "") +
                   r.ToJson().insert(1, "\"engine_stats\":" + engine_->StatsJson() + ",");
      std::fprintf(stderr,
                   "%-16s rep %d: %10.0f ops/s  p50 %6.2f us  p99 %7.2f us  p99.9 %8.2f us\n",
                   name.c_str(), rep + 1, r.OpsPerSec(), r.latency.Percentile(50) / 1e3,
                   r.latency.Percentile(99) / 1e3, r.latency.Percentile(99.9) / 1e3);
      runs.push_back(std::move(r));
    }
    return "{\"benchmark\":" + JsonString(name) + ",\"summary\":" + SummarizeJson(runs) +
           ",\"repetitions\":[" + reps_json + "]}";
  }

  void Close() { engine_.reset(); }
  bool had_errors() const { return had_errors_; }

 private:
  void Reopen(const EngineConfig& ec) {
    engine_.reset();
    std::string error;
    engine_ = OpenEngine(c_.engine, ec, &error);
    if (engine_ == nullptr) {
      std::fprintf(stderr, "cannot open %s: %s\n", c_.engine.c_str(), error.c_str());
      std::exit(1);
    }
  }

  std::string Key(uint64_t n) const { return MakeKey(n, c_.key_size); }

  RunResult RunOnce(const std::string& name) {
    RunSpec spec;
    spec.threads = c_.threads;
    const bool is_read = name.rfind("read", 0) == 0 || name == "seekrandom";
    const uint64_t total_ops = is_read && c_.reads > 0 ? c_.reads : c_.num;
    spec.ops_per_thread = total_ops / static_cast<uint64_t>(c_.threads);
    spec.extra_ops = total_ops % static_cast<uint64_t>(c_.threads);
    spec.target_rate = c_.rate;
    spec.max_seconds = c_.duration;
    spec.warmup_ops_per_thread = c_.warmup_ops;
    spec.pin_cpus = c_.pin_cpus;
    spec.bytes_per_op = static_cast<uint64_t>(c_.key_size) + c_.value_size;
    spec.stats_sampler = [this] { return engine_->StatsJson(); };
    Engine* e = engine_.get();
    const uint64_t num = c_.num;
    const uint64_t per_thread = spec.ops_per_thread;

    // Keys are built in a per-thread buffer, so the harness itself does not
    // allocate per operation.
    if (name == "fillseq") {
      return RunWorkload(spec, [=, this](int t, uint64_t i, std::mt19937_64& rng) {
        thread_local std::string key;
        key = Key(static_cast<uint64_t>(t) * per_thread +
                  std::min(static_cast<uint64_t>(t), spec.extra_ops) + i);
        return e->Put(key, values_.Next(rng));
      });
    }
    if (name == "fillrandom" || name == "overwrite") {
      return RunWorkload(spec, [=, this](int, uint64_t, std::mt19937_64& rng) {
        thread_local std::string key;
        key = Key(rng() % num);
        return e->Put(key, values_.Next(rng));
      });
    }
    if (name == "readrandom" || name == "readwhilewriting") {
      std::atomic<uint64_t> background_errors{0};
      if (name == "readwhilewriting") {
        spec.background = [=, this, &background_errors](const std::atomic<bool>& stop) {
          std::mt19937_64 rng(42);
          std::string key;
          while (!stop.load(std::memory_order_acquire)) {  // Pairs with the runner's release.
            key = Key(rng() % num);
            if (!e->Put(key, values_.Next(rng))) {
              // Relaxed: the worker join orders the final read.
              background_errors.fetch_add(1, std::memory_order_relaxed);
            }
          }
        };
      }
      RunResult r = RunWorkload(spec, [=, this](int, uint64_t, std::mt19937_64& rng) {
        thread_local std::string key;
        key = Key(rng() % num);
        return e->Get(key) >= 0;  // A miss is not an error.
      });
      r.errors += background_errors.load(std::memory_order_relaxed);  // Ordered by join.
      return r;
    }
    if (name == "readmissing") {
      return RunWorkload(spec, [=, this](int, uint64_t, std::mt19937_64& rng) {
        thread_local std::string key;
        key = Key(rng() % num);
        key += '.';  // Sorts right after a real key: inside the range, never present.
        return e->Get(key) == 0;
      });
    }
    if (name == "seekrandom") {
      return RunWorkload(spec, [=, this](int, uint64_t, std::mt19937_64& rng) {
        thread_local std::unique_ptr<EngineIterator> iter;
        thread_local Engine* owner = nullptr;
        if (owner != e) {  // A new iterator per run (the DB may have changed).
          iter = e->NewIterator();
          owner = e;
        }
        thread_local std::string key;
        key = Key(rng() % num);
        iter->Seek(key);
        if (iter->Valid()) {
          volatile size_t sink = iter->value().size();
          (void)sink;
        }
        return iter->ok();
      });
    }
    if (name == "readseq") {
      std::vector<std::unique_ptr<EngineIterator>> iters(static_cast<size_t>(c_.threads));
      for (auto& it : iters) {
        it = e->NewIterator();
        it->SeekToFirst();
      }
      RunResult r = RunWorkload(spec, [&](int t, uint64_t, std::mt19937_64&) {
        EngineIterator* it = iters[static_cast<size_t>(t)].get();
        if (!it->Valid()) {
          it->SeekToFirst();  // Wrap around if the DB holds fewer entries than ops.
        }
        if (!it->Valid()) {
          return false;
        }
        volatile size_t sink = it->key().size() + it->value().size();
        (void)sink;
        it->Next();
        return it->ok();
      });
      return r;
    }
    std::fprintf(stderr, "unknown benchmark %s\n", name.c_str());
    std::exit(2);
  }

  bool had_errors_ = false;
  const Config c_;
  const EngineConfig ec_;
  const ValueSource values_;
  std::unique_ptr<Engine> engine_;
};

int Main(int argc, char** argv) {
  const Flags flags(argc, argv);
  Config c;
  const std::string benchmarks = flags.Str("benchmarks", "fillseq,readrandom");
  c.num = static_cast<uint64_t>(flags.Int("num", 1'000'000));
  c.reads = static_cast<uint64_t>(flags.Int("reads", 0));
  c.key_size = static_cast<int>(flags.Int("key_size", 16));
  c.value_size = static_cast<size_t>(flags.Int("value_size", 100));
  c.threads = static_cast<int>(flags.Int("threads", 1));
  c.warmup_ops = static_cast<uint64_t>(flags.Int("warmup_ops", 0));
  c.repetitions = static_cast<int>(flags.Int("repetitions", 1));
  c.pin_cpus = flags.Bool("pin_cpus", false);
  c.rate = flags.Double("rate", 0);
  c.duration = flags.Double("duration", 0);
  c.engine = flags.Str("engine", "talus");
  const std::string output = flags.Str("output", "");
  EngineConfig ec = EngineConfigFromFlags(flags);
  flags.CheckAllUsed();

  std::fprintf(
      stderr, "db_bench: engine=%s num=%llu threads=%d key=%dB value=%zuB %s\n", c.engine.c_str(),
      static_cast<unsigned long long>(c.num), c.threads, c.key_size, c.value_size,
      c.rate > 0 ? ("open loop @ " + std::to_string(c.rate) + " ops/s").c_str() : "closed loop");
  if (c.threads <= 0 || c.num == 0 || c.num > INT64_MAX || c.reads > INT64_MAX ||
      c.num < static_cast<uint64_t>(c.threads) || c.key_size <= 0 || c.value_size > (1ULL << 30) ||
      c.repetitions <= 0 || c.rate < 0 || c.duration < 0 || c.warmup_ops > INT64_MAX ||
      benchmarks.empty()) {
    std::fprintf(stderr, "invalid benchmark counts, sizes, rate or duration\n");
    return 2;
  }
  Bench bench(c, ec);
  std::string results;
  std::stringstream list(benchmarks);
  for (std::string name; std::getline(list, name, ',');) {
    if (!name.empty()) {
      results += (results.empty() ? "" : ",") + bench.Run(name);
    }
  }
  bench.Close();
  WriteJson(output, "{\"tool\":\"db_bench\",\"engine\":" + JsonString(c.engine) +
                        ",\"load\":" + (c.rate > 0 ? "\"open\"" : "\"closed\"") +
                        ",\"flags\":" + flags.ToJson() + ",\"engine_config\":" + ec.ToJson() +
                        ",\"environment\":" + EnvironmentJson() + ",\"results\":[" + results +
                        "]}");
  return bench.had_errors() ? 1 : 0;
}

}  // namespace
}  // namespace talus::bench

int main(int argc, char** argv) { return talus::bench::Main(argc, argv); }
