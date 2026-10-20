# Benchmark tools (Phase 9)

Phase 9 provides the measurement tools and repeatable validation. Phase 10
runs the full experiments and writes PERFORMANCE.md. Smoke results are never
headline performance results.

## Build

```sh
cmake --preset release
cmake --build --preset release

# Optional comparison engines; dependencies stay outside the engine core.
cmake -S . -B build/compare -G Ninja -DCMAKE_BUILD_TYPE=Release -DTalus_BENCH_COMPARE=ON
cmake --build build/compare --parallel 4 --target db_bench ycsb micro_bench talus_bench_timer_test talus_bench_harness_test
```

Comparison dependencies are LevelDB 1.23 and RocksDB 9.10.0. No compression
library is enabled. Python plot scripts need matplotlib (`python3 -m pip
install 'matplotlib>=3.8,<4'`). The binary smoke validator uses only Python's
standard library.

## Validate before measuring

```sh
./build/compare/bench/talus_bench_timer_test
./build/compare/bench/talus_bench_harness_test
python3 bench/scripts/smoke.py --build build/compare --engines talus,leveldb,rocksdb --micro --out build/validation/smoke
```

The smoke validator runs all eight db_bench workloads and YCSB A–F with
uniform and Zipfian distributions, both load modes, two repetitions, odd
operation counts, warmup and multiple threads. It checks operation errors,
positive timing, histogram/time-series sample counts, percentiles and JSON
summaries. It also exercises sync writes, duration limits, invalid arguments
and (with --micro) every Google Benchmark case. Adapter regression tests
verify binary keys/values, iteration, deletion and recovery for every compiled
engine. Benchmark operation failures produce a nonzero exit code.

The `benchmarks` CI job repeats this on Ubuntu x86-64 and macOS ARM64, tests
Linux pinning under a restricted CPU set, and uploads raw JSON and plots.
CI permits missing hardware counters; it is a correctness check, not a
performance measurement platform.

On an x86-64 Linux host with PMU access, require every hardware counter:

```sh
python3 bench/scripts/smoke.py --build build/compare --engines talus,leveldb,rocksdb --require-perf --require-affinity --out build/validation/pmu
```

This fails if any worker cannot collect cycles, instructions, IPC, L1d read
misses, LLC read misses or branch misses. Ordinary runs omit unsupported
counters and report `perf.available=false` if none are usable. Linux counter
values are scaled by time-enabled/time-running for multiplexing, and an event
is reported only when every worker collected it. Counters cover worker
userspace work, including harness overhead, and exclude background threads.

## Workloads and output

```sh
./build/compare/bench/db_bench --engine=talus --benchmarks=fillseq,readrandom,readmissing,seekrandom,readseq,readwhilewriting --num=100000 --threads=2 --warmup_ops=1000 --repetitions=3 --output=/tmp/talus-closed.json
./build/compare/bench/db_bench --engine=rocksdb --benchmarks=fillseq,readrandom --num=100000 --threads=2 --rate=50000 --output=/tmp/rocksdb-open.json
./build/compare/bench/ycsb --engine=leveldb --workload=a --distribution=uniform --records=100000 --operations=100000 --threads=2 --rate=50000 --output=/tmp/leveldb-ycsb.json
```

Flags use `--name=value`. `--rate=0` is closed loop. A positive --rate is the
total arrival rate across measured worker threads. Open-loop latency starts
at each operation's scheduled arrival and includes queueing delay, preventing
coordinated omission. --duration is an upper scheduling bound; in-flight
operations can take longer. The optional readwhilewriting writer is unmeasured
and its writes are not throttled by the reader arrival rate. readseq advances
one iterator entry per measured operation and honors --rate.

Every result includes requested engine settings, timer source/conversion,
available engines, allowed CPU IDs, raw repetitions, throughput/MB/s,
p50/p90/p99/p99.9/p99.99/max, median and sample standard deviation, a latency
CDF, per-second latency statistics, engine stats and available perf counters.
`initial_engine_stats` is captured after warmup, before measured operations;
`engine_stats` is cumulative through the end of the repetition/background
settling. Use their difference for measured counter changes. DBStats samples
in `stats_series` support write-amplification-over-time plots.

Linux --pin_cpus=1 maps workers onto the process's allowed CPU set, so it
respects taskset/cgroup restrictions and checks affinity failures. macOS
reports affinity/counters unsupported. macOS ARM64 uses mach_absolute_time;
other ARM64 uses CNTVCT and x86-64 uses RDTSCP. Startup calibration rejects
backward/stopped/implausible counters and selects steady_clock as fallback.

## Sweeps and experiments

```sh
python3 bench/scripts/sweep.py --build build/compare --engines talus,leveldb,rocksdb --threads 1,2 --benchmarks readrandom,readwhilewriting --num 2000 --repetitions 2 --warmup-ops 10 --rate 10000 --out build/validation/sweep
python3 bench/scripts/experiments.py --build build/compare --quick --max-threads 2 --out build/validation/experiments

# Full measurements on a controlled Linux host (choose the CPU list there).
python3 bench/scripts/sweep.py --build build/compare --engines talus,leveldb,rocksdb --threads 1,2,4,8 --num 2000000 --repetitions 3 --cpus 0-7 --out results/sweep
python3 bench/scripts/experiments.py --build build/compare --max-threads 8 --cpus 0-7 --out results/experiments
```

Raw JSON, hardware/OS/disk/governor metadata, tables and plots are retained.
Runs shorter than a second may have no engine-stat samples and thus an empty
write-amplification plot. --quick validates script plumbing; it does not
establish a performance conclusion. --cpus limits the whole process on Linux;
--pin_cpus selects individual workers within that set. Background CPU pinning
is separately configurable with --background_cpus in the C++ tools.

## Comparison limits

Memtable size, block size, bloom bits, cache size, open-file limits, compression
off and sync request flags are passed to all engines. Table reads use mmap in
every engine by default (`--mmap_reads=1`): LevelDB always maps tables on
64-bit hosts and cannot be switched to pread, so Talus and RocksDB
(`allow_mmap_reads`) are matched to it. `--mmap_reads=0` switches Talus and
RocksDB to pread through their block caches. LevelDB fixes its level
count, L0 triggers, level sizes, cache sharding and background concurrency in
its implementation; changing Talus/RocksDB-only knobs does not change those
LevelDB constants. RocksDB dynamic level sizing is disabled to match the
configured level sizes. Bloom implementations and cache metadata overhead
still differ. Compare default common settings and retain the configuration.

LevelDB has no public wait-for-background-work API: its adapter polls L0 file
count as a bounded heuristic. This does not prove compaction quiescence or
force active memtables to disk. Talus/RocksDB use their background wait APIs.
Repetitions of read/update workloads share the preceding loaded database;
fill workloads start fresh each repetition. YCSB's load phase is closed loop;
the selected arrival rate applies to its run phase. Workload D uses a latest
key distribution. Concurrent inserts can briefly be selected before their
write completes, so YCSB reads allow NotFound while still counting I/O errors.

## Bare-metal laptop runs

To get every hardware counter (including LLC misses, which small GCP VMs
hide), boot any x86-64 laptop from an Ubuntu USB stick and run
`bench/scripts/laptop_run.sh --install`. It picks performance cores, detects
supported counters, builds, runs the read-path experiments and packs the
results. Step-by-step instructions, including Windows-side preparation:
[LAPTOP.md](LAPTOP.md).

## Keep cloud use short

Build and validate locally first. For hardware-counter evidence, use a single
short Linux session, retain raw JSON on the boot disk, copy results out and
delete the disposable VM/disks. Set a maximum run duration with automatic
termination when provisioning, rather than relying on a budget alert to stop
billing. No VM is created by any script in this repository.

Do not reuse the old `c4-standard-16-lssd --performance-monitoring-unit=enhanced`
suggestion from the original development chat: Google's current documentation
restricts enhanced PMU on C4 to 144/288-vCPU shapes. Standard PMU is available
on smaller C4 shapes but does not expose all LLC events. Confirm supported
counters and an hourly quote before choosing a host; a small VM must not be
presented as satisfying every hardware-counter requirement.
See [PMU setup](https://docs.cloud.google.com/compute/docs/enable-pmu-in-vms)
and [event support](https://docs.cloud.google.com/compute/docs/pmu-overview).
