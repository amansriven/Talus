#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "memtable/memtable.h"
#include "util/port.h"
#include "version/version_set.h"

namespace talus {

// An immutable bundle of everything a read needs: the active memtable, the
// immutable memtable (if one is waiting to be flushed) and the current
// Version. A new one is installed, with a higher generation, on every
// memtable switch, flush and compaction. Intrusively reference-counted; the
// last Unref() destroys it, and with it possibly the memtables and Version.
struct SuperVersion {
  std::shared_ptr<MemTable> mem;
  std::shared_ptr<MemTable> imm;  // May be null.
  std::shared_ptr<const Version> version;
  uint64_t generation = 0;

  void Ref() {
    // Relaxed: taking a reference needs no ordering. The caller already
    // holds a reference or the DB mutex, so the object cannot vanish
    // concurrently.
    refs_.fetch_add(1, std::memory_order_relaxed);
  }
  void Unref();

 private:
  std::atomic<int> refs_{1};
};

// Statistics counted per thread, summed into DBStats.
enum class Counter : size_t {
  kSuperVersionFastPath,
  kSuperVersionSlowPath,
  kGets,
  kTablesConsulted,
  kBloomChecks,
  kBloomNegatives,
  kBlockCacheHits,
  kBlockCacheMisses,
  kUserBytesWritten,
  kWalBytesWritten,
  kWriteGroups,
  kGroupSize1,  // Groups with 1 member; kGroupSize1 + i counts [2^i, 2^(i+1)).
  kGroupSize2,
  kGroupSize4,
  kGroupSize8,
  kGroupSize16,
  kGroupSize32,
  kGroupSize64Plus,
  kNumCounters,
};
inline constexpr size_t kNumCounters = static_cast<size_t>(Counter::kNumCounters);
using CounterTotals = std::array<uint64_t, kNumCounters>;

// One thread's state for one DB: its cached SuperVersion and its counters.
// Padded to a cache line so threads never share one.
struct alignas(kCacheLineSize) ThreadSlot {
  // A reference owned by the slot; touched only by the owning thread, or by
  // the DB's destructor once no thread uses the DB.
  SuperVersion* super_version = nullptr;
  uint64_t generation = 0;

  // Written only by the owning thread, read by GetStats().
  std::array<std::atomic<uint64_t>, kNumCounters> counters{};

  void Add(Counter c, uint64_t n = 1) {
    // A relaxed load and store instead of fetch_add: only this thread
    // writes, so no atomic read-modify-write (a locked instruction on x86)
    // is needed. Readers summing concurrently see a recent value.
    auto& counter = counters[static_cast<size_t>(c)];
    counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
  }
};

// Per-thread slots for one DB instance. Local() returns the calling thread's
// slot without locking, after its first use. A global registry under one
// mutex tracks every thread's slots, so:
//  * when a thread exits, its slots release their SuperVersions and fold
//    their counters into per-DB "retired" totals;
//  * when a DB is destroyed, its slots in every live thread are released
//    and cleared.
// Each DB gets a small index; indexes are reused only after that cleanup.
class PerThreadState {
 public:
  PerThreadState();
  ~PerThreadState();

  PerThreadState(const PerThreadState&) = delete;
  PerThreadState& operator=(const PerThreadState&) = delete;

  ThreadSlot& Local();

  // Counter totals over all live threads, plus threads that have exited.
  CounterTotals Sum() const;

 private:
  size_t index_;
};

}  // namespace talus
