# Current project status

Updated October 20, 2026.

Talus is a C++20 embedded ordered key-value engine. The public API lives in
`include/Talus`; `src/db/db_impl.cc` connects WAL/group commit, memtables,
thread-local SuperVersions, recovery, flush and leveled compaction. SSTables,
bloom filters and caches serve reads beyond memory; sequence numbers and
snapshots provide MVCC visibility.

## Roadmap

- Phases 0–10 are complete. See "Phase 10" below and
  [PERFORMANCE.md](PERFORMANCE.md).
- Open: LLC-miss counters (need an enhanced-PMU or bare-metal host;
  [LAPTOP.md](LAPTOP.md)), scaling beyond 4 cores, and narrowing LevelDB's
  lead on range seeks.

## Phase 10

- **Measurements.** The full suite ran on a GCP c4-standard-8-lssd (4
  Granite Rapids cores, SMT off, local NVMe, Ubuntu 24.04, GCC 13.3): eight
  A/B experiments, closed- and open-loop sweeps of Talus, LevelDB 1.23 and
  RocksDB 9.10 at 1–4 threads, and YCSB A–F, medians of 3 repetitions. Raw
  results are under the ignored `build/phase10/`; plots and summaries are in
  `docs/perf/`.
- **Engine changes from the measurements** (PERFORMANCE.md §3): adaptive
  spin/yield/block waiting in group commit; tables pinned in each Version's
  file metadata (including at recovery), so reads skip the table cache;
  each mmap block's checksum verified once per open table; mmap reads by
  default; the next WAL created outside the DB mutex; new tables preloaded by
  the thread that wrote them; mapped tables hold no file descriptor and the
  table limit respects `RLIMIT_NOFILE`; an optional background-thread nice
  value. The benchmark comparison now maps tables in all three engines.
- **Verification.** 278 tests pass in release on macOS and Linux x86-64; the
  269 engine tests also pass under ASan+UBSan and TSan. The model test's default directory is now unique
  per process, after two concurrent CTest runs corrupted each other's
  database. The 10,000-kill crash test passes on Linux x86-64 NVMe with the
  final code: 652,044 acknowledged batches, all recovered, none torn. Its
  first Linux run failed at iteration 73 with `Too many open files`, which
  led to the descriptor fix above; the harness now prints the child's error.

## Phase 9 work completed

- Fixed backward macOS ARM64 benchmark timestamps with mach_absolute_time
  and validated calibration/fallback; added a sleep-based regression test.
- Made readseq honor open-loop arrival rate; guarded invalid iterators.
- Preserved remainder operations across threads in db_bench and YCSB loads
  and runs; handled small Zipfian key spaces.
- Made low-rate duration caps stop before waiting for far-future arrivals.
- Added strict numeric/boolean parsing, count/size validation, output-write
  checks, iterator/background error propagation and nonzero failure exits.
- Linux worker affinity now respects the process's allowed CPU set and checks
  failures. Environment JSON records affinity support and permitted CPU IDs.
- Linux perf counters use enabled/running time to scale multiplexed events;
  totals require availability on every worker. LLC counters request LLC read
  misses specifically. Unsupported counters degrade gracefully.
- Added p90 and MB/s repetition summaries, after-warmup engine-stat baselines
  and histogram range sufficient to preserve queues exceeding one minute.
- Disabled RocksDB dynamic level sizing to match configured level budgets;
  documented remaining LevelDB/RocksDB comparison limits.
- Added repeatable smoke.py validation and CTest integration, plus regression
  tests for scheduled queueing latency, duration, remainder/sample counts,
  operation failures, long latency, counter aggregation and adapter semantics.
- Added Ubuntu/macOS comparison-engine CI coverage, restricted CPU-set testing,
  microbenchmarks, script checks and retained validation artifacts.
- Validated sweep/plot and all seven A/B scripts at smoke sizes; fixed quick
  thread limits and measured SuperVersion slow-path deltas after warmup.

## Local evidence (macOS ARM64)

Commands and interpretation are documented in [BENCHMARKS.md](BENCHMARKS.md).
Raw artifacts are under the ignored build/phase9-validation directory:

- All three engine adapters build with LevelDB 1.23 and RocksDB 9.10.0.
- Eight benchmark regression tests pass; adapter round-trip/delete/iterator/
  reopen checks cover all three engines and binary keys/values.
- The 84-check all-engine smoke matrix covers all eight db_bench workloads,
  YCSB A–F, uniform/Zipfian distributions, both load modes, two repetitions,
  multiple threads, warmup, odd operation counts, sync writes, input errors,
  duration caps and every Google Benchmark case.
- All seven experiment scripts generate JSON, summary tables and PNG plots.
- Comparison-engine sweep/plot validation covers one/two threads and two
  repetitions, including open-loop reads while writing.

The strict --require-perf/--require-affinity validator is ready for a Linux
host. macOS correctly reports unsupported counters and affinity. No Linux
hardware results are claimed, and no GCP resources have been created.

## GCP validation attempt

Project access and billing-enabled status were verified for talus-512109.
The user authorized a $5 cumulative cloud-spending cap; see
[GCP_BUDGET.md](GCP_BUDGET.md). Six suitable enhanced-PMU provisioning
attempts were rejected for capacity. No VM or disk was created, and Linux
validation remains pending. Prepared source/bootstrap/validation files are
retained locally for a later attempt.

## TSan CI allocation-link fix

Linux Clang 18 TSan defines strong new/delete symbols, which conflicted with
the allocation test's replacements. Linux TSan now counts calls through
linker wrappers and forwards to the sanitizer allocator. A positive counter
self-test verifies all six instrumented allocation overloads; zero-allocation
assertions remain enabled. On a disposable Ubuntu 24.04 x86-64 VM, the target
linked and all six allocation tests passed under TSan. Release and ASan/UBSan
allocation suites also passed locally. This is a targeted test result, not a
claim that the full Linux suite or PMU acceptance has passed.

The temporary TSan VM, boot disk and validation SSH metadata have been removed.
Final instance, disk and reserved-address inventories are empty.

## Linux x86-64 C4 validation (STANDARD PMU)

On October 19, 2026 a c4-standard-8 VM (Intel Xeon Platinum 8581C, Emerald
Rapids, Ubuntu 24.04, GCC, CMake 3.28) built Talus with LevelDB and RocksDB
and ran:

- the full CTest suite: 275/275 pass, including the 1M-operation model test,
  crash harness and benchmark regression tests;
- `smoke.py --require-perf --optional-perf=llc_misses --require-affinity
  --micro` over all three engines: 84 checks pass, plus 29 checks restricted
  to a single CPU with `taskset`.

Cycles, instructions, IPC, L1d misses and branch misses are measured per
operation. LLC misses are not available: GCP's STANDARD PMU exposes core
events only, and the ENHANCED PMU (L3 events) requires a 144- or 288-vCPU C4,
which exceeds this project's 12-vCPU quota. Full PMU acceptance therefore
still needs a quota increase or another Linux x86-64 host.

The run found and fixed one portability bug: `bench/` relied on `tests/`'s
`include(GoogleTest)`. Before CMake 3.29 the discovery script path is
directory-scoped, so the two benchmark test executables registered only
`*_NOT_BUILT` placeholders. Logs are in the ignored
`build/phase9-validation/c4-standard/` directory.
