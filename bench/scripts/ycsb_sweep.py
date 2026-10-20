#!/usr/bin/env python3
"""YCSB A-F across engines, plus a grouped throughput plot.

  bench/scripts/ycsb_sweep.py --build build/compare --out results/ycsb \
      --engines talus,leveldb,rocksdb --workloads a,b,c,d,e,f --distribution zipfian \
      --records 1000000 --operations 1000000 --threads 4 --repetitions 3 --cpus 0-3

Each (engine, workload) loads a fresh database, then runs the workload.
Writes raw JSON per run, hardware.json, summary.md and throughput.png.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import talus_bench as tb


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, default=Path("build/release"))
    ap.add_argument("--out", type=Path, default=Path("results/ycsb"))
    ap.add_argument("--engines", default="talus")
    ap.add_argument("--workloads", default="a,b,c,d,e,f")
    ap.add_argument("--distribution", default="zipfian")
    ap.add_argument("--records", type=int, default=1_000_000)
    ap.add_argument("--operations", type=int, default=1_000_000)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--value-size", type=int, default=100)
    ap.add_argument("--repetitions", type=int, default=3)
    ap.add_argument("--warmup-ops", type=int, default=10_000)
    ap.add_argument("--cpus", default=None, help="taskset CPU list for the whole process (Linux)")
    ap.add_argument("--db", default="/tmp/talus_ycsb_db")
    args = ap.parse_args()

    engines, workloads = args.engines.split(","), args.workloads.split(",")
    args.out.mkdir(parents=True, exist_ok=True)
    tb.save_json(args.out / "hardware.json", tb.hardware_info(args.db))
    tput: dict[str, dict[str, float]] = {}
    rows = []
    for w in workloads:
        for engine in engines:
            print(f"== workload {w}, {engine}", flush=True)
            data = tb.run_ycsb(args.build, args.out / "raw" / f"{engine}_{w}.json", cpus=args.cpus,
                               engine=engine, workload=w, distribution=args.distribution,
                               records=args.records, operations=args.operations, threads=args.threads,
                               value_size=args.value_size, repetitions=args.repetitions,
                               warmup_ops=args.warmup_ops, pin_cpus=True, db=f"{args.db}_{engine}")
            s = tb.result(data, f"workload_{w}")["summary"]
            tput.setdefault(engine, {})[w] = s["ops_per_sec"]["median"]
            rows.append(f"| {w.upper()} | {engine} | {s['ops_per_sec']['median']:,.0f} | "
                        f"{s['p50_ns']['median'] / 1e3:.1f} | {s['p99_ns']['median'] / 1e3:.1f} | "
                        f"{s['p999_ns']['median'] / 1e3:.1f} |")

    (args.out / "summary.md").write_text("\n".join([
        f"YCSB, {args.distribution} keys, {args.records:,} records, {args.operations:,} operations, "
        f"{args.threads} threads, {args.value_size} B values; medians of {args.repetitions} repetitions.",
        "", "| workload | engine | ops/s | p50 µs | p99 µs | p99.9 µs |", "|---|---|---|---|---|---|", *rows]) + "\n")

    fig, ax = tb.plt.subplots(figsize=(8, 4.5))
    width = 0.8 / len(engines)
    for i, engine in enumerate(engines):
        ax.bar([x + i * width for x in range(len(workloads))], [tput[engine][w] for w in workloads],
               width, label=engine)
    ax.set_xticks([x + width * (len(engines) - 1) / 2 for x in range(len(workloads))])
    ax.set_xticklabels([w.upper() for w in workloads])
    ax.set_xlabel("YCSB workload")
    ax.set_ylabel("ops/s")
    ax.grid(alpha=0.3, axis="y")
    ax.legend()
    tb._finish(fig, args.out / "throughput.png",
               f"YCSB throughput ({args.distribution}, {args.threads} threads)")


if __name__ == "__main__":
    main()
