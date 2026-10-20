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

> ✅ **Status:** All 10 phases are done ([roadmap](docs/PHASES.md)). On a 4-core x86 server, Talus serves
> **2.36M random reads/s at a p99 of 2.7 µs**, beats LevelDB and RocksDB on all six YCSB workloads under
> matched settings, and survives 10,000 `SIGKILL`s without losing an acknowledged write.
> Full results: [PERFORMANCE.md](docs/PERFORMANCE.md).

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
| **Group commit** | Concurrent writers share one `fsync`: 3.0× the durable write throughput at 4 threads |
| **Adaptive writer waiting** | Waiting writers spin, then yield, then sleep, so handing off the WAL rarely costs a context switch: 4.6× write throughput at 4 threads vs. sleeping right away |
| **Tables pinned in each Version** | Point reads go straight to the table, with no table-cache lock: reads scale linearly with cores |
| **mmap reads, checksummed once** | Blocks are read in place from the file mapping, and each block's CRC is verified on its first read only: 2.6× the throughput of `pread` |
| **Thread-local SuperVersion** | The `Get` fast path takes **zero mutexes**, just one atomic generation check |
| **Cache-line-blocked bloom filter** | Every probe stays inside one 64-byte cache line, so there's at most one cache miss |
| **Hardware CRC32C** | SSE4.2 on x86, ARMv8 CRC on ARM, with a slicing-by-8 software fallback chosen at runtime |
| **Zero-allocation hot paths** | `Put`, `Write`, and `Get` into a `PinnableValue` (memtable or block-cache hit) make **0 heap allocations per 100K ops**, and a test enforces it |
| **Per-thread stats** | Counters live in padded per-thread slots, so stats don't cause false sharing |

## 🛡️ How we know it's correct

"It works on my machine" isn't the bar.

- 🧪 **278 tests** pass on Linux x86-64 and macOS ARM64, and the 269 engine tests also run under **ASan, UBSan and ThreadSanitizer**.
- 🎲 **Model-based randomized testing:** 1M random operations (puts, deletes, batches, scans, snapshots, flushes, compactions, reopens) run against both Talus and a `std::map`, which must agree exactly. Failures save a replay file, and `--replay` reproduces them deterministically.
- 💥 **Crash harness:** a child process writes synced batches, the parent `SIGKILL`s it at a random moment, then reopens the DB and checks that every acknowledged batch is present and none is half-applied. Kills regularly land mid-flush and mid-compaction.
- 🧨 **Corruption tests:** flipping or truncating bytes in WAL, SST, MANIFEST and CURRENT files gives a clean `Corruption` error (or a clean WAL tail), never a crash or wrong data.
- 🐛 **Fuzzing** (libFuzzer + ASan + UBSan) for every on-disk format parser: 18M+ executions, zero crashes.
- 🧵 **ThreadSanitizer stress tests:** readers, writers, iterators and snapshots run concurrently while compaction runs, with zero data races allowed.
- 🧮 **Allocation test:** a counting `operator new` proves the hot paths make zero heap allocations.

## 📊 Benchmarks

Measured against **LevelDB 1.23 and RocksDB 9.10** with matched settings on a 4-core
Intel Xeon (Granite Rapids) cloud server with local NVMe. Medians of 3 runs, threads pinned
one per core. RocksDB is matched to the same settings, not tuned for its best numbers.

| | Talus | RocksDB | LevelDB |
|---|---|---|---|
| Random reads, 4 threads | **2.36M ops/s** · p99 2.7 µs | 1.76M · p99 4.1 µs | 1.12M · p99 19.5 µs |
| Random writes, 4 threads | **430k ops/s** · p99 22.5 µs | 337k · p99 16.4 µs | 277k · p99 67.6 µs |
| Reads while writing, 4 threads | **1.10M ops/s** | 889k | 288k |
| YCSB A / C (Zipfian) | **928k / 2.99M** | 747k / 2.00M | 344k / 1.13M |
| Random seeks, 4 threads | 935k | 805k | **1.53M** |

Where LevelDB wins (range seeks) and why is in [PERFORMANCE.md](docs/PERFORMANCE.md), along with
all eight A/B experiments: hypothesis, data, hardware counters and conclusion for each.

![YCSB throughput](docs/perf/ycsb.png)
![Random-read scaling](docs/perf/sweep_readrandom.png)

```bash
# Build with the comparison engines, then run everything (experiments, sweeps, YCSB):
cmake -S . -B build/compare -G Ninja -DCMAKE_BUILD_TYPE=Release -DTalus_BENCH_COMPARE=ON
cmake --build build/compare
bench/scripts/full_run.sh build/compare results /path/on/nvme 0-3 4

# Or a single workload:
./build/compare/bench/db_bench --engine=rocksdb --benchmarks=fillseq,readrandom --num=1000000 --threads=4
```

Linux hardware counters need PMU access; macOS reports them unavailable. See
[BENCHMARKS.md](docs/BENCHMARKS.md) for every flag and [LAPTOP.md](docs/LAPTOP.md) to benchmark on
bare metal from a USB stick.

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

The heavy checks can also be run on their own:

```bash
./build/release/tests/talus_model_test --ops 1000000             # model check (any --seed)
./build/release/tests/talus_model_test --replay talus_model_failure_<seed>.txt
./build/release/tests/talus_crash_test --iterations 10000        # the long crash run
CC=clang CXX=clang++ cmake --preset fuzz && cmake --build --preset fuzz   # libFuzzer targets
```

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
tests/           unit/, model/ (incl. concurrent), crash/, corruption/, stress/, alloc/
fuzz/            libFuzzer targets + seed-corpus generator
bench/           db_bench, YCSB, microbenchmarks, experiment and plotting scripts
docs/            DESIGN.md (byte-level formats and concurrency proofs), PHASES.md
```

## 📚 Further reading

- [`docs/DESIGN.md`](docs/DESIGN.md): every file format byte by byte, plus the reasoning behind every memory ordering
- [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md): results vs LevelDB and RocksDB, and every experiment's hypothesis, data, counters and conclusion
- [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) and [`docs/LAPTOP.md`](docs/LAPTOP.md): how to run the benchmarks, including on a bare-metal laptop
- [`docs/PHASES.md`](docs/PHASES.md): the build plan
