// Google Benchmark microbenchmarks (spec §12). Each isolates one design
// choice:
//   SkipList*       insert/lookup, single-writer vs concurrent, prefetch on/off
//   Crc32c*         hardware vs software
//   BloomProbe*     cache-line-blocked vs standard filter, sized past the LLC
//   BlockSeek       restart-point binary search + scan in one data block
//   CacheLookup     sharded LRU hit path, across threads
//   Arena/Malloc    bump allocation vs the system allocator, across threads
//   Counter*        false sharing: per-thread counters, padded vs unpadded
//
// Run with --benchmark_format=json for the experiment scripts. On Linux the
// benchmarks the experiments rely on also report hardware counters per
// iteration (cycles_per_op, l1d_misses_per_op, ...), counted over the timed
// loop only.

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "cache/lru_cache.h"
#include "db/dbformat.h"
#include "memtable/skiplist.h"
#include "perf_counters.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "table/filter_policy.h"
#include "table/table.h"
#include "util/arena.h"
#include "util/crc32c.h"
#include "util/hash.h"
#include "util/port.h"

namespace talus {
namespace {

// Counts hardware events from construction until destruction (put it just
// before the timed loop) and reports each available one per iteration.
class PerfScope {
 public:
  explicit PerfScope(benchmark::State& state) : state_(state) { counters_.Start(); }
  ~PerfScope() {
    const bench::PerfTotals totals = counters_.Stop();
    for (int i = 0; i < bench::kNumPerfEvents; ++i) {
      if (totals.available[i]) {
        state_.counters[std::string(bench::PerfEventName(i)) + "_per_op"] = benchmark::Counter(
            static_cast<double>(totals.values[i]), benchmark::Counter::kAvgIterations);
      }
    }
  }
  PerfScope(const PerfScope&) = delete;
  PerfScope& operator=(const PerfScope&) = delete;

 private:
  benchmark::State& state_;
  bench::ThreadPerfCounters counters_;
};

struct Bytewise {
  int operator()(std::string_view a, std::string_view b) const { return a.compare(b); }
};

std::string Key8(uint64_t v) {  // Big-endian: bytewise order = numeric order.
  std::string s(8, '\0');
  for (int i = 0; i < 8; ++i) {
    s[static_cast<size_t>(i)] = static_cast<char>(v >> (56 - 8 * i));
  }
  return s;
}

uint64_t Fnv(uint64_t v) { return Hash64(reinterpret_cast<const char*>(&v), sizeof(v)); }

std::string MakeUserKey(int i) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016d", i * 7);
  return buf;
}

// ---- Skiplist -------------------------------------------------------------------

template <bool kPrefetch>
void BM_SkipListInsert(benchmark::State& state) {
  const bool concurrent = state.range(0) != 0;
  for (auto _ : state) {
    state.PauseTiming();
    Arena arena;
    SkipList<Bytewise, kPrefetch> list(Bytewise(), &arena);
    std::mt19937_64 rng(301);
    std::vector<std::string> keys(100'000);
    for (auto& k : keys) {
      k = Key8(rng());
    }
    state.ResumeTiming();
    for (const auto& k : keys) {
      auto fill = [&](char* kd, char*) { std::memcpy(kd, k.data(), 8); };
      if (concurrent) {
        list.InsertConcurrently(8, 0, fill);
      } else {
        list.Insert(8, 0, fill);
      }
    }
  }
  state.SetItemsProcessed(state.iterations() * 100'000);
}
BENCHMARK(BM_SkipListInsert<true>)
    ->Arg(0)
    ->Arg(1)
    ->ArgNames({"concurrent"})
    ->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SkipListInsert<false>)
    ->Arg(0)
    ->Arg(1)
    ->ArgNames({"concurrent"})
    ->Unit(benchmark::kMillisecond);

// Many threads inserting into one skiplist with CAS (the concurrent path's
// scalability), vs the single-writer path driven by one thread.
template <bool kPrefetch>
void BM_SkipListConcurrentInsert(benchmark::State& state) {
  static std::unique_ptr<Arena> arena;
  static std::unique_ptr<SkipList<Bytewise, kPrefetch>> list;
  static std::atomic<uint64_t> next{0};
  if (state.thread_index() == 0) {
    arena = std::make_unique<Arena>();
    list = std::make_unique<SkipList<Bytewise, kPrefetch>>(Bytewise(), arena.get());
  }
  for (auto _ : state) {
    // Relaxed: only uniqueness of the key matters.
    const std::string k = Key8(Fnv(next.fetch_add(1, std::memory_order_relaxed)));
    list->InsertConcurrently(8, 0, [&](char* kd, char*) { std::memcpy(kd, k.data(), 8); });
  }
  state.SetItemsProcessed(state.iterations());
  if (state.thread_index() == 0) {
    list.reset();
    arena.reset();
  }
}
BENCHMARK(BM_SkipListConcurrentInsert<true>)->ThreadRange(1, 8)->UseRealTime();

template <bool kPrefetch>
void BM_SkipListLookup(benchmark::State& state) {
  const auto n = static_cast<uint64_t>(state.range(0));
  Arena arena;
  SkipList<Bytewise, kPrefetch> list(Bytewise(), &arena);
  for (uint64_t i = 0; i < n; ++i) {
    const std::string k = Key8(i * 2);
    list.Insert(8, 64, [&](char* kd, char*) { std::memcpy(kd, k.data(), 8); });
  }
  typename SkipList<Bytewise, kPrefetch>::Iterator iter(&list);
  std::mt19937_64 rng(7);
  {
    const PerfScope perf(state);
    for (auto _ : state) {
      iter.Seek(Key8((rng() % n) * 2));
      benchmark::DoNotOptimize(iter.Valid());
    }
  }
  state.SetItemsProcessed(state.iterations());
}
// 1M entries with 64-byte values: far bigger than the L2, so search misses
// cache, which is what prefetching targets (experiment 7).
BENCHMARK(BM_SkipListLookup<true>)->Arg(10'000)->Arg(1'000'000);
BENCHMARK(BM_SkipListLookup<false>)->Arg(10'000)->Arg(1'000'000);

// ---- CRC32C ---------------------------------------------------------------------

void BM_Crc32cHardware(benchmark::State& state) {
  const auto fn = crc32c::internal::HardwareExtend();
  if (fn == nullptr) {
    state.SkipWithError("no hardware CRC32C on this CPU");
    return;
  }
  const std::string data(static_cast<size_t>(state.range(0)), 'x');
  for (auto _ : state) {
    benchmark::DoNotOptimize(fn(0, data.data(), data.size()));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
void BM_Crc32cSoftware(benchmark::State& state) {
  const std::string data(static_cast<size_t>(state.range(0)), 'x');
  for (auto _ : state) {
    benchmark::DoNotOptimize(crc32c::internal::ExtendSoftware(0, data.data(), data.size()));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Crc32cHardware)->Range(64, 64 << 10);
BENCHMARK(BM_Crc32cSoftware)->Range(64, 64 << 10);

// ---- Bloom filters ----------------------------------------------------------------

// Probes for absent keys in a filter over 4M keys (5 MB, past the LLC on
// most CPUs). Each standard probe touches up to k cache lines; each blocked
// probe touches one (experiment 4).
void BM_BloomProbe(benchmark::State& state, FilterType type) {
  constexpr int kKeys = 4'000'000;
  std::vector<uint64_t> hashes(kKeys);
  for (int i = 0; i < kKeys; ++i) {
    hashes[static_cast<size_t>(i)] = Hash64("member" + std::to_string(i));
  }
  std::string filter;
  BuildFilter(type, 10, hashes, &filter);
  std::mt19937_64 rng(1);
  {
    const PerfScope perf(state);
    for (auto _ : state) {
      benchmark::DoNotOptimize(FilterMayContain(filter, rng()));
    }
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK_CAPTURE(BM_BloomProbe, blocked, FilterType::kBlockedBloom);
BENCHMARK_CAPTURE(BM_BloomProbe, standard, FilterType::kStandardBloom);

// ---- Block seek -----------------------------------------------------------------

void BM_BlockSeek(benchmark::State& state) {
  BlockBuilder builder(static_cast<int>(state.range(0)));
  std::vector<std::string> keys;
  for (int i = 0; i < 40; ++i) {  // About one 4 KiB block of 16 B keys and 64 B values.
    std::string k;
    AppendInternalKey(&k, MakeUserKey(i), 1, ValueType::kValue);
    builder.Add(k, std::string(64, 'v'));
    keys.push_back(k);
  }
  const std::string bytes(builder.Finish());
  Buffer heap = AllocateBuffer(bytes.size(), nullptr);
  std::memcpy(heap.get(), bytes.data(), bytes.size());
  BlockContents contents;
  contents.data = std::string_view(heap.get(), bytes.size());
  contents.heap = std::move(heap);
  const Block block(std::move(contents));
  BlockIter<> iter(block);
  std::mt19937 rng(3);
  for (auto _ : state) {
    iter.Seek(keys[rng() % keys.size()]);
    benchmark::DoNotOptimize(iter.Valid());
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BlockSeek)->Arg(1)->Arg(16)->ArgNames({"restart_interval"});

// ---- Block cache ------------------------------------------------------------------

void BM_CacheLookup(benchmark::State& state) {
  static std::unique_ptr<BlockCache> cache;
  constexpr int kBlocks = 2000;
  if (state.thread_index() == 0) {
    cache = std::make_unique<BlockCache>(64 << 20, 16);
    for (int i = 0; i < kBlocks; ++i) {
      cache->Release(
          cache->Insert({1, static_cast<uint64_t>(i) * 4096}, std::make_unique<Block>(), 4096));
    }
  }
  std::mt19937 rng(static_cast<uint32_t>(state.thread_index()));
  for (auto _ : state) {
    BlockCache::Handle* h = cache->Lookup({1, (rng() % kBlocks) * 4096ULL});
    benchmark::DoNotOptimize(h);
    cache->Release(h);
  }
  state.SetItemsProcessed(state.iterations());
  if (state.thread_index() == 0) {
    cache.reset();
  }
}
BENCHMARK(BM_CacheLookup)->ThreadRange(1, 8)->UseRealTime();

// ---- Arena vs malloc ------------------------------------------------------------------

void BM_ArenaAllocate(benchmark::State& state) {
  static std::unique_ptr<Arena> arena;
  if (state.thread_index() == 0) {
    arena = std::make_unique<Arena>();
  }
  const auto size = static_cast<size_t>(state.range(0));
  for (auto _ : state) {
    benchmark::DoNotOptimize(arena->Allocate(size));
  }
  state.SetItemsProcessed(state.iterations());
  if (state.thread_index() == 0) {
    arena.reset();
  }
}
void BM_Malloc(benchmark::State& state) {
  const auto size = static_cast<size_t>(state.range(0));
  std::vector<void*> blocks;
  blocks.reserve(1 << 16);
  for (auto _ : state) {
    void* p = std::malloc(size);  // NOLINT(cppcoreguidelines-no-malloc): the baseline.
    benchmark::DoNotOptimize(p);
    blocks.push_back(p);
    if (blocks.size() == blocks.capacity()) {
      state.PauseTiming();
      for (void* b : blocks) {
        std::free(b);  // NOLINT(cppcoreguidelines-no-malloc)
      }
      blocks.clear();
      state.ResumeTiming();
    }
  }
  for (void* b : blocks) {
    std::free(b);  // NOLINT(cppcoreguidelines-no-malloc)
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ArenaAllocate)->Arg(64)->ThreadRange(1, 8)->UseRealTime();
BENCHMARK(BM_Malloc)->Arg(64)->ThreadRange(1, 8)->UseRealTime();

// ---- False sharing ------------------------------------------------------------------------

// Each thread increments its own counter. Unpadded, eight counters share
// one or two cache lines, which ping-pong between cores on every write.
// Padded to a cache line each, they never interfere. The relaxed load+store
// is the same per-thread update pattern DBStats uses.
struct Unpadded {
  std::atomic<uint64_t> value{0};
};
struct alignas(kCacheLineSize) Padded {
  std::atomic<uint64_t> value{0};
};

template <typename Counter>
void BM_Counter(benchmark::State& state) {
  static std::array<Counter, 64> counters;
  auto& mine = counters[static_cast<size_t>(state.thread_index())].value;
  for (auto _ : state) {
    // Relaxed: only this thread writes this counter.
    mine.store(mine.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Counter<Unpadded>)->ThreadRange(1, 8)->UseRealTime();
BENCHMARK(BM_Counter<Padded>)->ThreadRange(1, 8)->UseRealTime();

}  // namespace
}  // namespace talus

BENCHMARK_MAIN();
