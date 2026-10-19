# Spec: Talus — a high-performance LSM-tree storage engine in C++20

Build a persistent, embedded, ordered key-value storage engine based on a log-structured merge-tree (LSM), in the style of LevelDB/RocksDB. Every component of the storage engine's data path must be implemented from scratch.

The project has two equal goals:
1. a correct, crash-safe storage engine;
2. rigorous C++ performance engineering: lock-free/low-contention concurrency, allocation-free hot paths, cache-conscious data layout, and tail-latency measurement backed by hardware evidence.

This spec is authoritative. Where it is ambiguous, choose the option closest to LevelDB/RocksDB semantics and record the decision in `docs/DESIGN.md`.

## 1. Toolchain and constraints

- **Language:** C++20. Build with CMake (≥3.20) using presets:
  - `debug`
  - `release`
  - `profile` (release + `-fno-omit-frame-pointer` + debug info)
  - `asan` (ASan+UBSan)
  - `tsan`
- **Compiler flags:** `-Wall -Wextra -Wpedantic -Werror`. Provide `.clang-format` and `.clang-tidy`.
- **Platforms:** Linux x86-64 is primary, and all performance work targets it. The project must also build and pass tests on macOS ARM64. Linux-only features (`perf_event_open`, CPU affinity, `MADV_HUGEPAGE`) degrade gracefully elsewhere.
- **Dependencies:** fetched via CMake `FetchContent`.
  - GoogleTest, Google Benchmark, HdrHistogram_c.
  - Optional LZ4 (CMake option, off by default).
  - LevelDB and RocksDB, **only** for comparison benchmarks (`-DTalus_BENCH_COMPARE=ON`).
- **No other dependencies.** No third-party data structures, allocators, hashing, concurrency or storage libraries in the engine core. Ask before adding anything.
- **Error handling:** all fallible engine APIs return a `Status` with codes `OK`, `NotFound`, `Corruption`, `IOError`, `InvalidArgument`, `Busy`. No exceptions on read or write paths.
- **Memory and resources:**
  - No raw `new`/`delete` outside arena, cache and pool implementations.
  - All file descriptors are owned by RAII wrappers.
- **Atomics:**
  - Every `std::atomic` operation uses an explicit `memory_order`, with a comment justifying it.
  - Every lock-free algorithm has a comment block stating its invariants and why it is safe (including memory reclamation).

## 2. Public API (`include/Talus/db.h`)

```cpp
class DB {
 public:
  static Status Open(const Options&, const std::string& path, std::unique_ptr<DB>* out);
  Status Put(const WriteOptions&, std::string_view key, std::string_view value);
  Status Delete(const WriteOptions&, std::string_view key);
  Status Write(const WriteOptions&, WriteBatch* batch);          // atomic multi-op
  Status Get(const ReadOptions&, std::string_view key, std::string* value);
  Status Get(const ReadOptions&, std::string_view key, PinnableValue* value); // zero-copy
  std::unique_ptr<Iterator> NewIterator(const ReadOptions&);     // Seek, SeekToFirst, Next, Valid, key, value, status
  const Snapshot* GetSnapshot();
  void ReleaseSnapshot(const Snapshot*);
  Status Flush();
  Status CompactRange(std::string_view begin, std::string_view end);
  DBStats GetStats() const;
  ~DB();                                                         // clean shutdown
};
```

- **`PinnableValue`:** exposes a `std::string_view` pointing directly into a pinned block-cache entry or memtable memory, and releases the pin on destruction or `Reset()`. It is the allocation-free read path.
- **`WriteOptions{bool sync = false}`:** with `sync=true`, the write is durable (WAL fsynced) before the call returns.
- **`ReadOptions{const Snapshot* snapshot = nullptr; bool fill_cache = true;}`.**
- Keys and values are arbitrary byte strings. Key ordering is bytewise.

**`Options`, with defaults:**

| Option | Default | Notes |
|---|---|---|
| `memtable_size` | 4 MiB | |
| `block_size` | 4 KiB | |
| `block_restart_interval` | 16 | |
| `bloom_bits_per_key` | 10 | |
| `block_cache_capacity` | 8 MiB | |
| `block_cache_shards` | 16 | |
| `max_open_files` | 1000 | |
| `target_file_size` | 2 MiB | |
| `l1_max_bytes` | 10 MiB | |
| `level_size_multiplier` | 10 | |
| `num_levels` | 7 | |
| `l0_compaction_trigger` | 4 | |
| `l0_slowdown_trigger` | 8 | |
| `l0_stop_trigger` | 12 | |
| `max_background_compactions` | 1 | |
| `compression` | none | lz4 optional |
| `paranoid_checks` | false | |
| `allow_concurrent_memtable_write` | true | |
| `compaction_rate_limit_bytes_per_sec` | 0 | 0 = unlimited |
| `use_huge_pages` | false | `MADV_HUGEPAGE` for arenas and cache |
| `background_thread_cpus` | empty | optional CPU affinity list |

## 3. Internal key format and MVCC

- **Internal key** = `user_key || fixed64(sequence << 8 | type)`, where `type` ∈ {Value=1, Deletion=0}.
- **Ordering:** user key ascending, then sequence number descending.
- **Sequence numbers:** a global monotonic 56-bit counter. A `WriteBatch` consumes consecutive sequence numbers and becomes visible atomically.
  - Publish the visible sequence number with a release store, only after all batches with lower sequences are fully inserted.
- **Reads:** return the newest entry whose sequence ≤ the read sequence. That is the snapshot's sequence, or the latest *visible* sequence if there is no snapshot. A Deletion entry means `NotFound`.
- **Compaction** preserves every version visible to any live snapshot. Tombstones may be dropped only at the bottommost level for their key range, and only when no snapshot needs them.

## 4. Memtable

- **Skiplist** with max height 12 and branching factor 4. Node height comes from a fast thread-local PRNG.
- **Node layout:** each node is a **single contiguous arena allocation**: the internal key, then the value, then a variable-length array of `std::atomic<Node*>` next pointers sized to the node's height. No per-node heap allocation.
- **Concurrent arena:** allocation bumps an atomic offset in the current block with a relaxed `fetch_add`. Only installing a new 4 KiB (or huge-page-backed) block takes a mutex. Allocations are aligned for atomics.
- **Concurrent inserts** (when `allow_concurrent_memtable_write`):
  - Lock-free multi-writer insertion that links nodes bottom-up with CAS and retries on contention.
  - Readers are always lock-free: acquire loads on next pointers, release/CAS on publication.
  - With the option off, use single-writer insertion as the baseline for comparison.
- **Prefetching:** `__builtin_prefetch` the next node while searching, and document measured impact (see section 12).
- **Switching memtables:**
  - When `memtable_size` is exceeded, the active memtable becomes immutable, a new memtable and a new WAL are created, and a background flush is scheduled.
  - At most one immutable memtable may be pending.
  - Writers stall while one is pending and the active memtable fills again.

## 5. Write-ahead log and group commit

- One WAL file per memtable: `NNNNNN.log`.
- **Record format:** `crc32c (4B, over type+payload) | length (4B) | type (1B) | payload`.
- **Payload:** a serialized `WriteBatch`: `sequence (8B) | count (4B)`, then for each op `type (1B) | varint32 key_len | key | [varint32 value_len | value]`.
- **Recovery:**
  - On `Open`, replay WAL files newer than the manifest's log number.
  - Stop at the first truncated or corrupt record and treat everything after it as never written. With `paranoid_checks`, return `Corruption` instead.
  - After replay, flush to an SSTable.
- **Group commit (leader/follower):**
  - Writers enqueue onto a writer queue. The head becomes leader.
  - The leader merges queued batches up to 1 MiB, assigns sequence ranges, and performs one WAL append (plus one fsync if any batch in the group requested sync).
  - **When concurrent memtable writes are enabled, followers insert their own batches into the memtable in parallel.** Otherwise the leader inserts all of them.
  - The last writer to finish publishes the group's visible sequence number and wakes everyone with their individual statuses.
  - Writers wait with an adaptive strategy: brief spin with a `pause`/`yield` hint, then block on `std::atomic::wait` or a futex. Document the spin threshold you chose and how you measured it.
- **WAL buffer:** reuse the record encoding buffer across writes, with no allocation per write in steady state.
- **fsync:** use `fdatasync` on Linux and `fcntl(F_FULLFSYNC)` on macOS.
- **CRC32C:**
  - Hardware-accelerated: SSE4.2 `_mm_crc32_u64` on x86-64, ARMv8 CRC instructions on ARM64.
  - Table-driven software fallback, with runtime dispatch.
  - Unit-test all paths against known vectors.

## 6. SSTable format (`NNNNNN.sst`, immutable)

```
[data block 0] ... [data block N] [filter block] [index block] [footer]
```

- **Data block:** entries with prefix compression: `varint32 shared | varint32 non_shared | varint32 value_len | key_delta | value`.
  - Every `block_restart_interval` entries, a restart point stores the full key.
  - The block ends with `fixed32 restart_offsets[] | fixed32 num_restarts`.
  - Lookup within a block uses binary search over restart points, then a linear scan.
- **Block trailer:** `compression_type (1B) | crc32c (4B)`, verified on every read from disk.
- **Filter block:** a **cache-line-blocked bloom filter**.
  - Each key hashes to one 64-byte-aligned block, and all of its probes fall within that block, so a probe costs at most one cache miss.
  - Keyed on the user key.
  - Also implement a standard (unblocked) bloom filter behind the same `FilterPolicy` interface, for comparison.
- **Index block:** same format as a data block. Each entry maps a shortest separator key to a `BlockHandle{varint64 offset, varint64 size}`.
- **Footer:** fixed 48 bytes: filter handle, index handle, zero padding, then an 8-byte magic number.
- **Hashing:** implement your own 64-bit hash (Murmur- or xxHash-style). Don't import a library.
- **Extension points:** use C++20 concepts to define the `Comparator`, `FilterPolicy` and iterator interfaces where it doesn't cost runtime performance.

## 7. Caches

- **Block cache:**
  - Sharded LRU, with the shard selected by hash of `(file_number, block_offset)`.
  - Each shard struct is `alignas(64)` to prevent false sharing, and has its own mutex.
  - Capacity is tracked in bytes and charged by block size.
  - Entries are reference-counted handles, so an evicted block stays valid while pinned by a reader or a `PinnableValue`.
- **Table cache:** LRU of open SSTable readers (file handle, parsed index, filter), bounded by `max_open_files`.
- **Huge pages:** when `use_huge_pages` is set, back cache memory and arenas with `madvise(MADV_HUGEPAGE)`.

## 8. Versions, manifest and compaction

- **Version:** an immutable, reference-counted list of SSTable metadata per level.
  - Metadata per file: file number, size, smallest and largest internal key.
  - Level 0 files may overlap. Levels 1 and above are non-overlapping and sorted.
- **SuperVersion:** an immutable, reference-counted bundle of {active memtable, immutable memtable, current Version}. A new one is installed on every flush, compaction or memtable switch, and tagged with an atomic generation number.
- **Lock-free read-path acquisition:**
  - Each thread caches a SuperVersion pointer in thread-local storage, together with its generation.
  - A `Get` compares the cached generation against the global atomic generation (acquire load). If they match, it uses the cached reference **without taking any mutex**.
  - If they differ, it takes the DB mutex once to swap in a new reference.
  - Old SuperVersions are freed when their reference count reaches zero, and thread-exit cleanup releases cached references.
  - Count slow-path acquisitions in `DBStats`.
- **Read path:**
  - Search the memtable, then the immutable memtable, then L0 files from newest to oldest, then L1 and below with binary search on file ranges.
  - Skip a table when the bloom filter says the key is absent.
  - Readers never block on flush or compaction.
- **MANIFEST:**
  - An append-only log of `VersionEdit` records, using the same record format as the WAL. An edit can add a file (level + metadata), delete a file, and set the next file number, last sequence number and log number.
  - `CURRENT` names the live manifest.
  - Update `CURRENT` atomically: write a temp file, fsync it, `rename`, then fsync the directory.
- **Leveled compaction:**
  - Compute a score per level. For L0 it is file count ÷ `l0_compaction_trigger`. For L*n* (n ≥ 1) it is level size ÷ the level's max bytes (`l1_max_bytes × multiplier^(n−1)`).
  - Compact the highest-scoring level when its score is ≥ 1. Choose input files with a round-robin compaction pointer per level, plus every overlapping file in the next level.
  - Use a k-way heap merging iterator and apply the MVCC drop rules from section 3.
  - Split output at `target_file_size`, or earlier if it would overlap too many grandparent files.
- **Background work:**
  - A thread pool with one dedicated flush thread and `max_background_compactions` compaction threads.
  - Flush has priority over compaction.
  - Pin threads to `background_thread_cpus` if that option is set.
- **Compaction rate limiter:**
  - A token-bucket limiter on compaction write bandwidth, configured by `compaction_rate_limit_bytes_per_sec`.
  - Refill in small intervals so I/O is smooth, not bursty.
- **Write stalls:**
  - At ≥ `l0_slowdown_trigger` L0 files, delay each write by 1 ms.
  - At ≥ `l0_stop_trigger` files, block writes until compaction catches up.
  - Record stall count and total stall time.
- **Obsolete files:** delete files no live Version references, only after the manifest edit removing them is durable.

## 9. Hot-path performance requirements (enforced by tests)

- **Zero heap allocations** in steady state for:
  - `Get(…, PinnableValue*)` on a block-cache hit or memtable hit.
  - `Put` and `Write`, excluding memtable arena growth and WAL file growth.
- **Enforcement:** a dedicated test binary replaces global `operator new`/`delete` with counting versions. It warms up, then asserts the allocation count stays at zero across 100K operations on these paths.
- **No mutex on the `Get` fast path:** verified by `DBStats` slow-path counters staying at zero during a read-only steady-state benchmark.
- **No shared atomic counters on hot paths:** statistics are kept per thread in cache-line-padded slots and summed when `GetStats()` is called.

## 10. Statistics (`DBStats`)

- **Write amplification:** bytes written by users, to the WAL, by flush and by compaction, with write amplification = total disk bytes written ÷ user bytes.
- Bytes read by compaction.
- **Read amplification:** average number of SSTables consulted per `Get`.
- **Space amplification:** SST bytes ÷ estimated live bytes.
- Per level: file count and total bytes.
- Bloom filter checks and how many returned "definitely absent".
- Block cache hits and misses.
- SuperVersion fast-path and slow-path acquisitions.
- Group-commit group sizes (histogram).
- Stall count and total stall time.
- Number of flushes and compactions, the time spent on each, and rate-limiter wait time.

## 11. Testing (all must exist and pass)

- **Unit tests** for every component:
  - arena (including concurrent allocation), skiplist (single-writer and concurrent), varint/coding, CRC32C (all paths);
  - both bloom filters, with false-positive rate within 20% of theoretical at 10 bits/key;
  - block builder/reader, SSTable round-trip, merging iterator, LRU cache;
  - VersionEdit encoding, WAL reader/writer, rate limiter.
- **Model-based randomized test:**
  - A seeded, reproducible sequence of Put, Delete, Get, Scan, Snapshot, Flush, CompactRange and reopen operations, run against both the DB and a `std::map` model, which must match exactly. Default suite: ≥1M operations.
  - On failure, the test writes the seed and the operation log to a file, and a `--replay <file>` mode reproduces the failure deterministically.
- **Concurrent model test:**
  - N threads each own a disjoint key range and their own model.
  - Background flush and compaction run throughout, plus concurrent snapshot readers that verify snapshot isolation.
- **Crash harness:**
  - A parent process forks a child that writes verifiable sequential batches with `sync=true` and reports each acknowledgment through a pipe.
  - The parent sends `SIGKILL` at a random time, reopens the DB, and verifies that every acknowledged batch is present and no batch is partially applied.
  - Iterations are configurable: default 100 in CI, 10,000 for a long run.
- **Corruption tests:** flipping or truncating bytes in WAL, SST and MANIFEST files must produce `Corruption` (or a clean truncation, in the WAL tail case), never a crash or wrong data.
- **Fuzz targets** (libFuzzer, clang only) for the SSTable reader, block reader, WAL reader and VersionEdit decoder.
- **TSan stress test:** concurrent writers, readers, iterators and snapshots while flushes and compactions run, with zero TSan reports.
- **Allocation test:** as described in section 9.
- **CI:** GitHub Actions running `debug`, `asan` and `tsan` on Ubuntu, plus `release` on macOS.

## 12. Benchmarks and performance analysis

**Timing:**
- Measure per-operation latency with a cycle-counter timer: `rdtsc`/`rdtscp` on x86-64, `cntvct_el0` on ARM64.
- Calibrate against `std::clock_gettime(CLOCK_MONOTONIC)` at startup, with a `std::chrono` fallback.
- Record into HdrHistogram.

**`bench/db_bench`:**
- **Flags:** `--benchmarks`, `--num`, `--key_size` (16), `--value_size` (100), `--threads`, `--sync`, `--engine=Talus|leveldb|rocksdb`, `--warmup_ops`, `--repetitions`, `--pin_cpus`.
- **Workloads:** `fillseq`, `fillrandom`, `overwrite`, `readrandom`, `readmissing`, `seekrandom`, `readseq`, `readwhilewriting`.
- **Load modes:**
  - **closed-loop**, as fast as possible;
  - **open-loop**, at a fixed target arrival rate, with latency measured from each operation's *scheduled* start time to avoid coordinated omission.
- **Output** as JSON, for each workload:
  - ops/sec and MB/s;
  - p50, p90, p99, p99.9, p99.99 and max latency;
  - median and standard deviation across repetitions;
  - a per-second latency-percentile time series, so stalls and compaction interference are visible;
  - a `DBStats` snapshot.

**Hardware counters (Linux):**
- Use `perf_event_open` to report cycles, instructions, IPC, L1d/LLC misses and branch misses per operation for each workload.

**`bench/ycsb`:**
- Workloads A–F with uniform and zipfian distributions.
- Supports both load modes and produces the same JSON output.

**Comparison engines:**
- LevelDB and RocksDB sit behind the same thin engine interface.
- They use matched settings (memtable size, block size, bloom bits, cache size, compression off, sync semantics), and the configuration is printed with the results.

**Google Benchmark microbenchmarks:**
- Skiplist insert and lookup, single-writer vs. concurrent, with prefetching on and off.
- CRC32C, hardware vs. software.
- Bloom filter probe, blocked vs. standard.
- Block seek.
- Cache lookup.
- Arena vs. `malloc` allocation.
- **False-sharing demonstration:** padded vs. unpadded per-thread counters.

**Required A/B experiments.** Each is run by a script and written up in `docs/PERFORMANCE.md` with: hypothesis, setup, results table or plot, hardware-counter evidence, and conclusion.

1. Group commit on vs. off: sync-write throughput and p99 latency vs. thread count.
2. Concurrent vs. single-writer memtable inserts: write throughput scaling from 1 to N threads.
3. Thread-local SuperVersion vs. mutex-protected acquisition: `readrandom` scaling and p99.9.
4. Blocked vs. standard bloom filter: `readmissing` latency, LLC misses per operation, false-positive rate.
5. Compaction rate limiter off vs. tuned: p99.9 and p99.99 of `readwhilewriting` in open-loop mode, with time-series plots.
6. `pread` vs. `mmap` table reads, and huge pages on vs. off.
7. Prefetching on vs. off in skiplist search.

**Python scripts in `bench/scripts/`:**
- Run sweeps and experiments with CPU pinning, warmup and repetitions.
- Record hardware info (CPU model, core count, SMT state, RAM, disk, kernel or OS version, CPU frequency governor).
- Store raw JSON and produce matplotlib plots:
  - throughput vs. threads;
  - latency CDFs per engine;
  - latency time series during compaction;
  - write amplification over time;
  - one plot per A/B experiment.

## 13. Repository layout

```
include/Talus/      public headers
src/                engine (util/, memtable/, wal/, table/, cache/, version/, db/)
tests/              unit, model, concurrent model, crash, corruption, stress, alloc
fuzz/               libFuzzer targets
bench/              db_bench, ycsb, microbenchmarks, scripts/
docs/DESIGN.md      architecture; every file format byte-by-byte; the concurrency model;
                    invariants and memory-ordering justification for every lock-free path
docs/PERFORMANCE.md every A/B experiment with hypothesis, data, counters and conclusion;
                    an honest comparison with LevelDB/RocksDB, including where they win and why
README.md           build/test/bench instructions, headline results, plots, hardware used
```

## 14. Definition of done

- Every section above is implemented: no stubs, TODOs or disabled tests.
- All tests pass under every preset, including the zero-allocation test.
- The crash harness passes 10,000 iterations.
- `db_bench` and YCSB run against all three engines, in both load modes, with hardware counters on Linux.
- All seven A/B experiments are run and written up in `docs/PERFORMANCE.md`.
- `docs/DESIGN.md` is precise enough that someone could write a compatible reader from the document alone, and justifies every memory ordering and lock-free invariant.