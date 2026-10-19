# Current project status

Updated October 19, 2026.

Talus is a C++20 embedded ordered key-value engine. The public API lives in
`include/Talus`; `src/db/db_impl.cc` connects WAL/group commit, memtables,
thread-local SuperVersions, recovery, flush and leveled compaction. SSTables,
bloom filters and caches serve reads beyond memory; sequence numbers and
snapshots provide MVCC visibility.

## Roadmap

- Phases 0–7: engine implementation exists end to end.
- Phase 8: verification harnesses exist. Fresh macOS release run passed all
  266 tests present at the start of the handoff, including the 1M-operation
  model test and 100-iteration crash harness. Sanitizer/CI and the 10,000-kill
  acceptance run require separately retained evidence.
- Phase 9: benchmark implementation and macOS validation are complete. Linux
  x86-64/PMU acceptance remains pending access to a host or a CI run. The new
  benchmark CI job is prepared locally but has not been pushed or executed.
- Phase 10: full measured experiments, PERFORMANCE.md and headline results
  remain outstanding. The quick experiments below validate tooling only.

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
