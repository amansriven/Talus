# Talus build phases

Each phase ends with every test passing under the `debug`, `asan`, `tsan` and
`release` presets. Section numbers refer to `spec.md`.

| Phase | Scope | Spec sections |
|---|---|---|
| **0. Scaffolding** | CMake + presets, warnings-as-errors, `.clang-format`/`.clang-tidy`, GoogleTest via FetchContent, GitHub Actions CI, docs skeleton | §1, §11 (CI), §13 |
| **1. Core utilities** | `Status`, fixed/varint coding, CRC32C (SSE4.2, ARMv8, software, runtime dispatch), 64-bit hash, concurrent arena, unit tests | §1, §4 (arena), §5 (CRC32C), §6 (hash) |
| 2. Memtable | Internal key format, thread-local PRNG, skiplist (single-writer, then lock-free concurrent), prefetching, memtable wrapper | §3, §4 |
| 3. Write path | `WriteBatch`, WAL writer/reader, recovery replay, group commit with adaptive waiting, durable fsync | §5 |
| 4. SSTable | Block builder/reader, blocked + standard bloom filters, table builder/reader, merging iterator | §6 |
| 5. Caches | Sharded LRU block cache with pinned handles, table cache, huge-page support | §7 |
| 6. Versions and compaction | Version/VersionEdit, MANIFEST + CURRENT, SuperVersion with thread-local fast path, memtable switching (immutable memtable, new WAL, writer stall), recovery (replay WALs newer than the manifest's log number, then flush to an SSTable), flush, leveled compaction, rate limiter, write stalls, obsolete-file deletion | §8 |
| 7. DB API | `DB::Open`/`Put`/`Get`/`Write`/iterators/snapshots, `PinnableValue`, `DBStats` with per-thread counters | §2, §9, §10 |
| 8. Verification | Model-based randomized test with replay, concurrent model test, crash harness, corruption tests, fuzz targets, TSan stress, zero-allocation test | §9, §11 |
| 9. Benchmarks | Cycle-counter timer, `db_bench`, YCSB, open/closed-loop load, `perf_event_open`, LevelDB/RocksDB adapters, Google Benchmark microbenchmarks, Python sweep scripts | §12 |
| 10. Experiments and docs | The seven A/B experiments, `PERFORMANCE.md`, complete `DESIGN.md`, README with results | §12, §13, §14 |

## Phase 9 acceptance status

Implementation and macOS ARM64 validation are complete: all-engine workload
matrix, microbenchmarks, sweeps/plots, seven quick experiment scripts and
benchmark regression tests pass. On a GCP c4-standard-8 (Linux x86-64,
STANDARD PMU) all 275 tests and the all-engine smoke matrix pass with CPU
affinity and core counters (cycles, IPC, L1d and branch misses). LLC-miss
counters still need an ENHANCED-PMU host (144-vCPU C4 or bare metal). The CI job and strict host validator are ready; see
[BENCHMARKS.md](BENCHMARKS.md) and [STATUS.md](STATUS.md). Full experiment
measurements remain Phase 10.
