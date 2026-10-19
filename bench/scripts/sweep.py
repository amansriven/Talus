#!/usr/bin/env python3
"""Engine x threads x workload sweeps with db_bench, plus plots.

Example (Linux benchmark host, all three engines, pinned to CPUs 0-15):

  bench/scripts/sweep.py --build build/release --out results/sweep \
      --engines talus,leveldb,rocksdb --threads 1,2,4,8,16 \
      --benchmarks fillrandom,readrandom,readwhilewriting --num 2000000 \
      --repetitions 3 --warmup-ops 20000 --cpus 0-15

Writes raw JSON per run, hardware.json, summary.json, and:
  throughput_<benchmark>.png   throughput vs threads, one line per engine
  latency_cdf_<benchmark>.png  latency percentiles per engine (max threads)
  timeseries_<benchmark>.png   per-second p99/p99.9/p99.99 per engine
  write_amp.png                Talus write amplification over time
"""

from __future__ import annotations

import argparse
from pathlib import Path

import talus_bench as tb


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, default=Path("build/release"))
    ap.add_argument("--out", type=Path, default=Path("results/sweep"))
    ap.add_argument("--engines", default="talus")
    ap.add_argument("--threads", default="1,2,4,8")
    ap.add_argument("--benchmarks", default="fillrandom,readrandom")
    ap.add_argument("--num", type=int, default=1_000_000)
    ap.add_argument("--repetitions", type=int, default=3)
    ap.add_argument("--warmup-ops", type=int, default=10_000)
    ap.add_argument("--value-size", type=int, default=100)
    ap.add_argument("--rate", type=float, default=0, help="total arrival rate; 0 = closed loop")
    ap.add_argument("--duration", type=float, default=0, help="seconds cap per workload")
    ap.add_argument("--cpus", default=None, help="taskset CPU list for the whole process (Linux)")
    ap.add_argument("--db", default="/tmp/talus_sweep_db")
    args = ap.parse_args()

    engines = args.engines.split(",")
    threads = [int(t) for t in args.threads.split(",")]
    benchmarks = args.benchmarks.split(",")
    args.out.mkdir(parents=True, exist_ok=True)
    tb.save_json(args.out / "hardware.json", tb.hardware_info(args.db))

    summary: dict = {}
    for engine in engines:
        for t in threads:
            # fillseq first, so read workloads see every key.
            run_list = ["fillseq"] + [b for b in benchmarks if b != "fillseq"]
            print(f"== {engine}, {t} threads", flush=True)
            data = tb.run_db_bench(
                args.build, args.out / "raw" / f"{engine}_t{t}.json", cpus=args.cpus,
                rate=args.rate, duration=args.duration, engine=engine, threads=t, benchmarks=run_list, num=args.num,
                value_size=args.value_size, repetitions=args.repetitions,
                warmup_ops=args.warmup_ops, pin_cpus=True, db=f"{args.db}_{engine}")
            for entry in data["results"]:
                summary.setdefault(entry["benchmark"], {}).setdefault(engine, {})[t] = {
                    "summary": entry["summary"], "median_run": tb.median_run(entry)}
    tb.save_json(args.out / "summary.json",
                 {b: {e: {t: v["summary"] for t, v in per.items()} for e, per in eng.items()}
                  for b, eng in summary.items()})

    for bench, per_engine in summary.items():
        tb.plot_throughput_vs_threads(
            {e: [(t, v["summary"]["ops_per_sec"]["median"]) for t, v in per.items()]
             for e, per in per_engine.items()},
            args.out / f"throughput_{bench}.png", f"{bench}: throughput vs threads")
        top = max(threads)
        tb.plot_latency_cdf({e: per[top]["median_run"]["latency_ns"]["cdf"]
                             for e, per in per_engine.items() if top in per},
                            args.out / f"latency_cdf_{bench}.png", f"{bench}: latency, {top} threads")
        tb.plot_timeseries({e: per[top]["median_run"]["timeseries"]
                            for e, per in per_engine.items() if top in per},
                           args.out / f"timeseries_{bench}.png", f"{bench}: latency over time, {top} threads")
    if "talus" in engines:
        writes = [b for b in summary if b.startswith("fill") or b in ("overwrite", "readwhilewriting")]
        tb.plot_write_amp({b: summary[b]["talus"][max(threads)]["median_run"]["stats_series"] for b in writes},
                          args.out / "write_amp.png", "Talus write amplification over time")


if __name__ == "__main__":
    main()
