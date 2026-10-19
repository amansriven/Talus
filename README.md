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
| **Zero-allocation hot paths** | `Get` (with `PinnableValue`) and `Put` never call `malloc` in steady state, and a test enforces it |
| **Per-thread stats** | Counters live in padded per-thread slots, so stats don't cause false sharing |

## 🛡️ How we know it's correct

"It works on my machine" isn't the bar.

- 🎲 **Model-based randomized testing:** 1M+ random operations run against both Talus and a `std::map`, which must agree exactly. Failures save a replay file for deterministic reproduction.
- 💥 **Crash harness:** a child process writes synced batches, the parent `SIGKILL`s it at a random moment, then reopens the DB and checks that every acknowledged batch is present and none is half-applied.
- 🧨 **Corruption tests:** flipping bits in WAL, SST and MANIFEST files gives a clean `Corruption` error, never a crash or wrong data.
- 🐛 **Fuzzing** (libFuzzer) for every on-disk format parser.
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

## 🗺️ Repo layout

```
include/Talus/   public API
src/util/        coding, CRC32C, hash, arena, Status
src/…            memtable/, wal/, table/, cache/, version/, db/   (coming soon)
tests/           unit, model, crash, corruption, stress, allocation tests
fuzz/            libFuzzer targets
bench/           db_bench, YCSB, microbenchmarks, plotting scripts
docs/            DESIGN.md (byte-level formats and concurrency proofs), PERFORMANCE.md
```

## 📚 Further reading

- [`docs/DESIGN.md`](docs/DESIGN.md): every file format byte by byte, plus the reasoning behind every memory ordering
- [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md): the experiments: hypothesis, data, counters, conclusion
- [`docs/PHASES.md`](docs/PHASES.md): the build plan
