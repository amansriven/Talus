# 🪨 Talus

> *talus (n.)*: the slope of broken rock that piles up at the foot of a cliff.
> Things fall, they settle into layers, and over time they get compacted.
> That's also how this database works.

**Talus** is an embedded, persistent, ordered key-value storage engine built on a
**log-structured merge-tree (LSM)**, in the spirit of LevelDB and RocksDB, written
from scratch in **C++20**. Everything on the data path is built from scratch:
no borrowed skiplists, no borrowed hash functions, no borrowed allocators.

It has two goals, and they're equally important:

1. **Never lose your data.** Crash it, corrupt it, `SIGKILL` it 10,000 times. Every acknowledged write must survive.
2. **Go fast, and prove it.** Lock-free data structures, zero-allocation hot paths, cache-line-aware layouts, and tail-latency claims backed by hardware performance counters.

> 🚧 **Status:** Phases 0–7 of 10 are done ([roadmap](docs/PHASES.md)). The engine works end to end:
> crash-safe writes with group commit, a lock-free read path, SSTables with cache-line-blocked bloom
> filters, leveled compaction, snapshots, iterators and zero-copy reads. Next up: the full
> verification gauntlet (Phase 8), then benchmarks against LevelDB and RocksDB.

---

## 🧭 What's an LSM-tree, in one minute?

Disks hate random writes and love sequential ones. An LSM-tree turns your random
writes into sequential ones by **buffering in memory, then dumping sorted chunks to disk**.

### The life of a write ✍️

```
Put("cat", "meow")
   │
   ├─▶ 1. Write-ahead log (WAL)     appended + checksummed, so a crash can't lose it
   │
   └─▶ 2. Memtable                  a lock-free skiplist in RAM, sorted by key
            │
            │  (memtable fills up: 4 MiB)
            ▼
        3. Flush ──▶ L0 SSTable     an immutable, sorted, indexed file on disk
                        │
                        ▼
        4. Compaction ──▶ L1 ──▶ L2 ──▶ … ──▶ L6
           (merge and sort downward, dropping overwritten and deleted data)
```

Like rock on a talus slope, data settles into **levels**, and each level is about
10× bigger than the one above it.

### The life of a read 🔍

```
Get("cat")
  1. Memtable?               (RAM, lock-free)
  2. Immutable memtable?     (RAM, being flushed)
  3. L0 files, newest first  (bloom filter: "definitely not here", so skip the file)
  4. L1…L6                   (binary search to the one file that could hold it)
```

**Bloom filters** let reads skip files that can't contain the key. The **block cache**
keeps hot data in memory, so most reads never touch the disk.

---

## ⚡ What makes it fast

| Technique | What it buys |
|---|---|
| **Lock-free concurrent skiplist** | Many writers insert into the memtable at once using CAS, while readers never block |
| **Concurrent arena allocator** | One relaxed `fetch_add` per allocation; a mutex only when a new 4 KiB block is needed |
| **Group commit** | 100 threads that each want `fsync` share one `fsync` in total |
| **Thread-local SuperVersion** | The `Get` fast path takes **zero mutexes**, just one atomic generation check |
| **Cache-line-blocked bloom filter** | Every probe stays inside one 64-byte cache line, so there's at most one cache miss |
| **Hardware CRC32C** | SSE4.2 on x86, ARMv8 CRC on ARM, with a slicing-by-8 software fallback chosen at runtime |
| **Zero-allocation hot paths** | `Put`, `Write`, and `Get` into a `PinnableValue` (memtable or block-cache hit) measured at **0 heap allocations per 100K ops** |
| **Per-thread stats** | Counters live in padded per-thread slots, so stats don't cause false sharing |

## 🛡️ How we know it's correct

"It works on my machine" isn't the bar.

- 🧪 **250+ tests, run under ASan, UBSan and ThreadSanitizer** on every change, including a check of the whole engine against a `std::map` model across flushes, compactions and reopens.
- 🎲 **Model-based randomized testing:** 1M+ random operations run against both Talus and a `std::map`, which must agree exactly. Failures save a replay file for deterministic reproduction. *(Scaled up in Phase 8.)*
- 💥 **Crash harness:** a child process writes synced batches, the parent `SIGKILL`s it at a random moment, then reopens the DB and checks that every acknowledged batch is present and none is half-applied. *(Phase 8.)*
- 🧨 **Corruption tests:** flipping bits in WAL, SST and MANIFEST files gives a clean `Corruption` error, never a crash or wrong data. SSTables already get a byte-flip sweep across the whole file.
- 🐛 **Fuzzing** (libFuzzer) for every on-disk format parser. *(Phase 8.)*
- 🧵 **ThreadSanitizer stress tests:** readers, writers, iterators and snapshots run concurrently while compaction runs, with zero data races allowed.

## 📊 Benchmarks

*Coming in Phase 9.* Talus will be benchmarked against **LevelDB and RocksDB**
with matched settings: closed-loop and open-loop (coordinated-omission-safe) load,
latencies from p50 to p99.99, and `perf` hardware counters (IPC, cache misses, branch misses).

The write-up will be honest, including where RocksDB wins and why.

---

## 🔨 Build and test

Requires CMake ≥ 3.20, Ninja and a C++20 compiler (Clang or GCC).

```bash
cmake --preset debug          # also: release, profile, asan, tsan
cmake --build --preset debug
ctest --preset debug
```

| Preset | Purpose |
|---|---|
| `debug` | Day-to-day development |
| `release` | Optimized build |
| `profile` | Release + frame pointers + debug info, for `perf` |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer |

**Platforms:** Linux x86-64 is the primary target (all performance work happens there).
macOS ARM64 is fully supported for building and testing.

## 🚀 Using it

```cpp
#include "Talus/db.h"

talus::Options options;
options.create_if_missing = true;
std::unique_ptr<talus::DB> db;
talus::Status s = talus::DB::Open(options, "/tmp/mydb", &db);

s = db->Put({}, "cat", "meow");
s = db->Put({.sync = true}, "dog", "woof");      // durable before it returns

talus::WriteBatch batch;                         // atomic: all or nothing
batch.Put("cow", "moo");
batch.Delete("cat");
s = db->Write({}, &batch);

std::string value;
s = db->Get({}, "dog", &value);                  // copies the value

talus::PinnableValue pinned;                     // zero-copy, zero-allocation read
s = db->Get({}, "cow", &pinned);                 // pinned.value() points into the engine

const talus::Snapshot* snap = db->GetSnapshot(); // a frozen point in time
auto it = db->NewIterator({.snapshot = snap});
for (it->SeekToFirst(); it->Valid(); it->Next()) {
  // it->key(), it->value(): sorted, as of the snapshot
}
it.reset();
db->ReleaseSnapshot(snap);
```

## 🗺️ Repo layout

```
include/Talus/   public API: DB, Options, WriteBatch, Iterator, PinnableValue, Status
src/util/        coding, CRC32C, hash, arena, files, rate limiter, huge-page pool
src/memtable/    lock-free concurrent skiplist + memtable
src/wal/         write-ahead log reader/writer
src/table/       blocks, bloom filters, SSTable builder/reader, merging iterator
src/cache/       sharded LRU block cache, table cache
src/version/     versions, MANIFEST, compaction picking
src/db/          the engine: group commit, SuperVersions, flush, compaction, iterators
tests/unit/      unit + integration tests   (crash, fuzz, stress, allocation: Phase 8)
fuzz/            libFuzzer targets   (Phase 8)
bench/           db_bench, YCSB, microbenchmarks, plotting scripts   (Phase 9)
docs/            DESIGN.md (byte-level formats and concurrency proofs), PHASES.md
```

## 📚 Further reading

- [`docs/DESIGN.md`](docs/DESIGN.md): every file format byte by byte, plus the reasoning behind every memory ordering
- `docs/PERFORMANCE.md`: the experiments: hypothesis, data, counters, conclusion *(Phase 10)*
- [`docs/PHASES.md`](docs/PHASES.md): the build plan
