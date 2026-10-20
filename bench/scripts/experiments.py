#!/usr/bin/env python3
"""Runs the seven A/B experiments of spec §12 and writes their data and plots.

  bench/scripts/experiments.py --build build/release --out results/experiments \
      --cpus 0-15 [--only 1,4] [--quick]

Each experiment writes to OUT/<name>/: raw JSON for every variant,
summary.md (the results table) and one or more plots. hardware.json records
the machine. --quick shrinks every size, for a smoke test, not real numbers.

  1 group_commit         sync-write throughput and p99 vs threads, grouping on/off
  2 concurrent_memtable  write throughput scaling 1..N threads, concurrent vs single-writer
  3 super_version        readrandom scaling and p99.9, thread-local vs mutex acquisition
  4 bloom                readmissing latency, L2/LLC misses/op, false-positive rate: blocked vs standard,
                         plus per-probe counters from BM_BloomProbe
  5 rate_limiter         readwhilewriting p99.9/p99.99 (open loop) and time series, limiter off vs tuned
  6 mmap_hugepages       readrandom: pread vs mmap, huge pages off vs on
  7 prefetch             skiplist lookup with and without prefetching (microbenchmark)
  8 wait_policy          supplement to 1-2: spin/yield/block policy vs write throughput and p99
"""

from __future__ import annotations

import argparse
from pathlib import Path

import talus_bench as tb


class Ctx:
    def __init__(self, args: argparse.Namespace) -> None:
        self.build: Path = args.build
        self.out: Path = args.out
        self.cpus: str | None = args.cpus
        self.quick: bool = args.quick
        self.reps: int = 1 if args.quick else args.repetitions
        self.db = args.db
        self.max_threads = args.max_threads

    def threads(self) -> list[int]:
        ts, t = [], 1
        while t <= (min(4, self.max_threads) if self.quick else self.max_threads):
            ts.append(t)
            t *= 2
        return ts

    def n(self, full: int, quick: int) -> int:
        return quick if self.quick else full

    def db_bench(self, exp: str, variant: str, **flags) -> dict:
        flags.setdefault("repetitions", self.reps)
        flags.setdefault("pin_cpus", True)
        flags.setdefault("db", f"{self.db}_{exp}")
        return tb.run_db_bench(self.build, self.out / exp / f"{variant}.json", cpus=self.cpus, **flags)


def table(path: Path, header: list[str], rows: list[list], intro: str) -> None:
    lines = [intro, "", "| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    for row in rows:
        lines.append("| " + " | ".join(f"{c:,.1f}" if isinstance(c, float) else str(c) for c in row) + " |")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")
    print(f"  wrote {path}")


def perf(run: dict, key: str) -> str:
    p = run.get("perf", {})
    return f"{p[key]:.2f}" if key in p else "n/a"


def micro_rows(data: dict) -> list[dict]:
    """Google Benchmark entries: the medians when there are repetitions, else the single runs."""
    medians = [b for b in data["benchmarks"] if b.get("aggregate_name") == "median"]
    return medians or [b for b in data["benchmarks"] if b.get("run_type") != "aggregate"]


def micro_perf(b: dict, key: str) -> str:
    """A PerfScope counter from a micro_bench entry; "ipc" is derived."""
    if key == "ipc":
        ok = b.get("cycles_per_op", 0) > 0 and "instructions_per_op" in b
        return f"{b['instructions_per_op'] / b['cycles_per_op']:.2f}" if ok else "n/a"
    return f"{b[key]:.2f}" if key in b else "n/a"


# ---- 1. Group commit ------------------------------------------------------------------------

def exp_group_commit(c: Ctx) -> None:
    exp = "1_group_commit"
    tput, p99, rows = {}, {}, []
    for gc in (True, False):
        label = "group commit" if gc else "no grouping"
        for t in c.threads():
            data = c.db_bench(exp, f"gc{int(gc)}_t{t}", benchmarks="fillrandom", sync=True,
                              group_commit=gc, threads=t, num=c.n(400_000, 128) * t,
                              duration=c.n(20, 3))
            entry = tb.result(data, "fillrandom")
            run = tb.median_run(entry)
            tput.setdefault(label, []).append((t, entry["summary"]["ops_per_sec"]["median"]))
            p99.setdefault(label, []).append((t, entry["summary"]["p99_ns"]["median"] / 1e3))
            groups = run["engine_stats"]["write_groups"]
            rows.append([label, t, entry["summary"]["ops_per_sec"]["median"],
                         entry["summary"]["p99_ns"]["median"] / 1e3,
                         run["ops"] / groups if groups else 0.0, perf(run, "cycles_per_op")])
    tb.plot_throughput_vs_threads(tput, c.out / exp / "throughput.png", "Sync writes: throughput vs threads")
    tb.plot_throughput_vs_threads(p99, c.out / exp / "p99.png", "Sync writes: p99 latency vs threads",
                                  ylabel="p99 (µs)")
    table(c.out / exp / "summary.md", ["variant", "threads", "ops/s", "p99 µs", "writes/fsync", "cycles/op"],
          rows, "Experiment 1: group commit on vs off (fillrandom, sync=true).")


# ---- 2. Concurrent memtable inserts ------------------------------------------------------------

def exp_concurrent_memtable(c: Ctx) -> None:
    exp = "2_concurrent_memtable"
    tput, rows = {}, []
    for concurrent in (True, False):
        label = "concurrent (CAS)" if concurrent else "single writer"
        for t in c.threads():
            data = c.db_bench(exp, f"cm{int(concurrent)}_t{t}", benchmarks="fillrandom",
                              concurrent_memtable=concurrent, threads=t,
                              num=c.n(4_000_000, 100_000), memtable_size=64 << 20)
            entry = tb.result(data, "fillrandom")
            run = tb.median_run(entry)
            tput.setdefault(label, []).append((t, entry["summary"]["ops_per_sec"]["median"]))
            rows.append([label, t, entry["summary"]["ops_per_sec"]["median"],
                         entry["summary"]["p99_ns"]["median"] / 1e3, perf(run, "ipc"),
                         perf(run, "l1d_misses_per_op")])
    tb.plot_throughput_vs_threads(tput, c.out / exp / "throughput.png", "Writes: concurrent vs single-writer memtable")
    table(c.out / exp / "summary.md", ["variant", "threads", "ops/s", "p99 µs", "IPC", "L1d misses/op"],
          rows, "Experiment 2: concurrent vs single-writer memtable inserts (fillrandom, no sync).")


# ---- 3. SuperVersion acquisition ----------------------------------------------------------------

def exp_super_version(c: Ctx) -> None:
    exp = "3_super_version"
    num = c.n(2_000_000, 50_000)
    c.db_bench(exp, "fill", benchmarks="fillseq", num=num, repetitions=1)
    tput, rows = {}, []
    for tls in (True, False):
        label = "thread-local (lock-free)" if tls else "mutex"
        for t in c.threads():
            data = c.db_bench(exp, f"tls{int(tls)}_t{t}", benchmarks="readrandom", use_existing_db=True,
                              tls_super_version=tls, threads=t, num=num,
                              reads=c.n(1_000_000, 50_000) * t, warmup_ops=1000)
            entry = tb.result(data, "readrandom")
            run = tb.median_run(entry)
            tput.setdefault(label, []).append((t, entry["summary"]["ops_per_sec"]["median"]))
            rows.append([label, t, entry["summary"]["ops_per_sec"]["median"],
                         entry["summary"]["p999_ns"]["median"] / 1e3,
                         run["engine_stats"]["super_version_slow_path"] -
                         run["initial_engine_stats"]["super_version_slow_path"], perf(run, "cycles_per_op")])
    tb.plot_throughput_vs_threads(tput, c.out / exp / "throughput.png", "readrandom: SuperVersion acquisition")
    table(c.out / exp / "summary.md", ["variant", "threads", "ops/s", "p99.9 µs", "slow-path acquisitions",
                                       "cycles/op"],
          rows, "Experiment 3: thread-local SuperVersion vs mutex-protected acquisition (readrandom).")


# ---- 4. Bloom filters -------------------------------------------------------------------------

def exp_bloom(c: Ctx) -> None:
    exp = "4_bloom"
    rows, cdfs, fprs = [], {}, {}
    for blocked in (True, False):
        label = "blocked" if blocked else "standard"
        data = c.db_bench(exp, label, benchmarks="fillseq,readmissing", blocked_bloom=blocked,
                          num=c.n(5_000_000, 100_000), threads=1, warmup_ops=1000,
                          db=f"{c.db}_{exp}_{label}")
        entry = tb.result(data, "readmissing")
        run = tb.median_run(entry)
        st = run["engine_stats"]
        fpr = 1 - st["bloom_negatives"] / st["bloom_checks"] if st["bloom_checks"] else 0.0
        fprs[label] = fpr * 100
        cdfs[label] = run["latency_ns"]["cdf"]
        rows.append([label, entry["summary"]["ops_per_sec"]["median"], entry["summary"]["p50_ns"]["median"] / 1e3,
                     entry["summary"]["p99_ns"]["median"] / 1e3, perf(run, "l1d_misses_per_op"),
                     perf(run, "l2_misses_per_op"), perf(run, "llc_misses_per_op"), f"{fpr * 100:.3f}%"])
    tb.plot_latency_cdf(cdfs, c.out / exp / "latency_cdf.png", "readmissing latency: blocked vs standard bloom")
    tb.plot_bars(fprs, c.out / exp / "fpr.png", "Measured false-positive rate", "%")
    table(c.out / exp / "summary.md", ["filter", "ops/s", "p50 µs", "p99 µs", "L1d misses/op", "L2 misses/op",
                                       "LLC misses/op", "false-positive rate"],
          rows, "Experiment 4: cache-line-blocked vs standard bloom filter (readmissing).")
    # The filter probe alone, without the rest of the read path diluting it.
    data = tb.run_micro(c.build, c.out / exp / "micro.json", "BM_BloomProbe",
                        repetitions=1 if c.quick else 5, cpus=c.cpus,
                        min_time="0.001s" if c.quick else "0.5s")
    table(c.out / exp / "probe.md", ["filter", "ns/probe", "cycles/probe", "L1d misses/probe",
                                     "L2 misses/probe", "LLC misses/probe"],
          [[b["run_name"].split("/")[1], b["real_time"], micro_perf(b, "cycles_per_op"),
            micro_perf(b, "l1d_misses_per_op"), micro_perf(b, "l2_misses_per_op"),
            micro_perf(b, "llc_misses_per_op")] for b in micro_rows(data)],
          "Experiment 4b: one filter probe for an absent key (BM_BloomProbe, 4M keys, 10 bits/key).")


# ---- 5. Compaction rate limiter ----------------------------------------------------------------

def exp_rate_limiter(c: Ctx) -> None:
    exp = "5_rate_limiter"
    rows, series, wamp = [], {}, {}
    # Readers get half the cores: the writer, flush and two compaction
    # threads need the rest, and an oversubscribed machine would measure the
    # OS scheduler rather than the limiter.
    readers = max(1, c.max_threads // 2)
    # (rate limit, background nice): the limiter sweep, plus lowering the
    # background threads' CPU priority instead of their I/O rate.
    variants = ((0, 0), (4 << 20, 0)) if c.quick else (
        (0, 0), (8 << 20, 0), (16 << 20, 0), (32 << 20, 0), (64 << 20, 0), (0, 10))
    for limit, nice in variants:
        label = ("limiter off" if limit == 0 else f"limit {limit >> 20} MiB/s") + (
            f", bg nice {nice}" if nice else "")
        data = c.db_bench(exp, f"limit{limit}_nice{nice}", benchmarks="fillrandom,readwhilewriting",
                          rate_limit=limit, bg_nice=nice, threads=readers, num=c.n(2_000_000, 100_000),
                          rate=c.n(100_000, 20_000), duration=c.n(30, 5), max_background_compactions=2)
        entry = tb.result(data, "readwhilewriting")
        run = tb.median_run(entry)
        series[label] = run["timeseries"]
        wamp[label] = run["stats_series"]
        st, st0 = run["engine_stats"], run["initial_engine_stats"]
        writer_mb_s = (st["user_bytes_written"] - st0["user_bytes_written"]) / run["seconds"] / 1e6
        rows.append([label, entry["summary"]["p99_ns"]["median"] / 1e3,
                     entry["summary"]["p999_ns"]["median"] / 1e3, entry["summary"]["p9999_ns"]["median"] / 1e3,
                     writer_mb_s, (st["stall_micros"] - st0["stall_micros"]) / 1e6,
                     st["compactions"] - st0["compactions"], st["rate_limiter_wait_micros"] / 1e6])
    tb.plot_timeseries(series, c.out / exp / "timeseries.png",
                       "readwhilewriting (open loop): read latency over time")
    tb.plot_write_amp(wamp, c.out / exp / "write_amp.png", "Write amplification over time")
    table(c.out / exp / "summary.md", ["variant", "p99 µs", "p99.9 µs", "p99.99 µs", "writer MB/s",
                                       "write stall s", "compactions", "limiter wait s"],
          rows, "Experiment 5: protecting reads from compaction: rate limits and background CPU priority (readwhilewriting, open loop, half the cores reading).")


# ---- 6. pread vs mmap, huge pages ---------------------------------------------------------------

def exp_mmap_hugepages(c: Ctx) -> None:
    exp = "6_mmap_hugepages"
    num = c.n(5_000_000, 100_000)
    c.db_bench(exp, "fill", benchmarks="fillseq", num=num, repetitions=1)
    rows, cdfs = [], {}
    for mmap in (False, True):
        for huge in (False, True):
            label = f"{'mmap' if mmap else 'pread'}{' + huge pages' if huge else ''}"
            data = c.db_bench(exp, label.replace(" ", "").replace("+", "_"), benchmarks="readrandom",
                              use_existing_db=True, mmap_reads=mmap, huge_pages=huge,
                              threads=c.max_threads if not c.quick else 2, num=num,
                              reads=c.n(4_000_000, 50_000),
                              warmup_ops=10_000, cache_size=c.n(64 << 20, 4 << 20))
            entry = tb.result(data, "readrandom")
            run = tb.median_run(entry)
            cdfs[label] = run["latency_ns"]["cdf"]
            rows.append([label, entry["summary"]["ops_per_sec"]["median"], entry["summary"]["p50_ns"]["median"] / 1e3,
                         entry["summary"]["p99_ns"]["median"] / 1e3, perf(run, "l2_misses_per_op"),
                         perf(run, "llc_misses_per_op"), perf(run, "cycles_per_op")])
    tb.plot_latency_cdf(cdfs, c.out / exp / "latency_cdf.png", "readrandom: pread vs mmap, huge pages")
    table(c.out / exp / "summary.md", ["variant", "ops/s", "p50 µs", "p99 µs", "L2 misses/op", "LLC misses/op",
                                       "cycles/op"],
          rows, "Experiment 6: pread vs mmap table reads, huge pages off vs on (readrandom).")


# ---- 7. Skiplist prefetching ---------------------------------------------------------------------

def exp_prefetch(c: Ctx) -> None:
    exp = "7_prefetch"
    data = tb.run_micro(c.build, c.out / exp / "micro.json", "BM_SkipListLookup",
                        repetitions=1 if c.quick else 5, cpus=c.cpus,
                        min_time="0.001s" if c.quick else "0.5s")
    rows, bars = [], {}
    for b in micro_rows(data):
        prefetch = "<true>" in b["run_name"]
        size = b["run_name"].split("/")[1]
        label = f"{'prefetch' if prefetch else 'no prefetch'}, {int(size):,} keys"
        bars[label] = b["real_time"]
        rows.append([label, b["real_time"], b.get("items_per_second", 0.0), micro_perf(b, "cycles_per_op"),
                     micro_perf(b, "ipc"), micro_perf(b, "l1d_misses_per_op"), micro_perf(b, "l2_misses_per_op")])
    tb.plot_bars(bars, c.out / exp / "lookup_ns.png", "Skiplist lookup time", "ns per lookup")
    table(c.out / exp / "summary.md", ["variant", "ns/lookup", "lookups/s", "cycles/lookup", "IPC",
                                       "L1d misses/lookup", "L2 misses/lookup"], rows,
          "Experiment 7: prefetching on vs off in skiplist search (BM_SkipListLookup).")


# ---- 8. Group-commit wait policy (supplement to 1 and 2) -------------------------------------------

def exp_spin(c: Ctx) -> None:
    exp = "8_wait_policy"
    rows, tput = [], {}
    # (spin polls, yield µs): block almost at once, the old spin-then-block,
    # a long spin, and the adaptive spin-yield-block default.
    policies = ((0, 0), (256, 0)) if c.quick else ((0, 0), (256, 0), (4096, 0), (256, 100))
    for sync in (False, True):
        for spin, yield_us in policies:
            label = f"{'sync' if sync else 'no sync'}, spin {spin}, yield {yield_us} µs"
            for t in c.threads():
                data = c.db_bench(exp, f"sync{int(sync)}_spin{spin}_yield{yield_us}_t{t}", benchmarks="fillrandom",
                                  sync=sync, spin=spin, yield_us=yield_us, threads=t,
                                  num=c.n(200_000 if sync else 2_000_000, 128) * t, duration=c.n(15, 3))
                entry = tb.result(data, "fillrandom")
                run = tb.median_run(entry)
                groups = run["engine_stats"]["write_groups"]
                if not sync:
                    tput.setdefault(f"spin {spin}, yield {yield_us} µs", []).append(
                        (t, entry["summary"]["ops_per_sec"]["median"]))
                rows.append([label, t, entry["summary"]["ops_per_sec"]["median"],
                             entry["summary"]["p99_ns"]["median"] / 1e3, run["ops"] / groups if groups else 0.0,
                             perf(run, "cycles_per_op")])
    tb.plot_throughput_vs_threads(tput, c.out / exp / "throughput.png",
                                  "fillrandom (no sync): throughput vs threads by wait policy")
    table(c.out / exp / "summary.md", ["policy", "threads", "ops/s", "p99 µs", "writes/group", "cycles/op"],
          rows, "Experiment 8 (supplement to 1 and 2): how group-commit waiters wait (fillrandom).")


EXPERIMENTS = {
    1: exp_group_commit, 2: exp_concurrent_memtable, 3: exp_super_version, 4: exp_bloom,
    5: exp_rate_limiter, 6: exp_mmap_hugepages, 7: exp_prefetch, 8: exp_spin,
}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, default=Path("build/release"))
    ap.add_argument("--out", type=Path, default=Path("results/experiments"))
    ap.add_argument("--only", default=",".join(str(k) for k in EXPERIMENTS))
    ap.add_argument("--quick", action="store_true", help="tiny sizes: a smoke test, not real numbers")
    ap.add_argument("--repetitions", type=int, default=3)
    ap.add_argument("--max-threads", type=int, default=16)
    ap.add_argument("--cpus", default=None, help="taskset CPU list for the benchmark process (Linux)")
    ap.add_argument("--db", default="/tmp/talus_exp_db")
    args = ap.parse_args()
    if args.max_threads <= 0 or args.repetitions <= 0:
        ap.error("max-threads and repetitions must be positive")
    ctx = Ctx(args)
    args.out.mkdir(parents=True, exist_ok=True)
    tb.save_json(args.out / "hardware.json", tb.hardware_info(args.db))
    for k in (int(x) for x in args.only.split(",")):
        print(f"=== experiment {k}: {EXPERIMENTS[k].__name__[4:]}", flush=True)
        EXPERIMENTS[k](ctx)


if __name__ == "__main__":
    main()
