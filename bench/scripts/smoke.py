#!/usr/bin/env python3
"""Validate benchmark binaries; results are smoke checks, not performance claims.

  python3 bench/scripts/smoke.py --build build/compare --engines talus,leveldb,rocksdb
  python3 bench/scripts/smoke.py --build build/compare --require-perf --require-affinity

Uses temporary databases and writes validation.json plus raw JSON to --out.
No matplotlib dependency. Exit nonzero on errors, missing samples or bad timing.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import tempfile
from pathlib import Path


WORKLOADS = "fillseq,fillrandom,overwrite,readrandom,readmissing,seekrandom,readseq,readwhilewriting"


def validate(data: dict, require_perf: bool = False, require_affinity: bool = False) -> None:
    env = data["environment"]
    assert math.isfinite(env["timer_ns_per_tick"]) and env["timer_ns_per_tick"] > 0, env
    if require_affinity:
        assert env["affinity_supported"] and env["allowed_cpus"], env
    for entry in data["results"]:
        assert entry["summary"]["repetitions"] == len(entry["repetitions"])
        for metric in ("ops_per_sec", "mb_per_sec", "p50_ns", "p90_ns", "p99_ns", "p999_ns", "p9999_ns", "max_ns"):
            assert math.isfinite(entry["summary"][metric]["median"])
            assert entry["summary"][metric]["stddev"] >= 0
        for r in entry["repetitions"]:
            assert r["errors"] == 0 and r["ops"] > 0, (entry["benchmark"], r)
            assert r["seconds"] > 0 and math.isfinite(r["ops_per_sec"]) and r["ops_per_sec"] > 0, r
            assert r["latency_ns"]["count"] == r["ops"], r
            assert sum(row["ops"] for row in r["timeseries"]) == r["ops"], r
            h = r["latency_ns"]
            ps = [h[k] for k in ("p50", "p90", "p99", "p999", "p9999", "max")]
            assert ps == sorted(ps) and ps[0] > 0, ps
            assert h["cdf"], h
            if require_perf:
                assert r["perf"]["available"], r["perf"]
                for event in ("cycles_per_op", "instructions_per_op", "ipc", "l1d_misses_per_op", "llc_misses_per_op", "branch_misses_per_op"):
                    assert event in r["perf"] and r["perf"][event] >= 0, r["perf"]
                assert r["perf"]["cycles_per_op"] > 0 and r["perf"]["instructions_per_op"] > 0


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", type=Path, default=Path("build/release"))
    ap.add_argument("--out", type=Path, default=Path("build/phase9-validation/smoke"))
    ap.add_argument("--engines", default="talus")
    ap.add_argument("--require-perf", action="store_true")
    ap.add_argument("--require-affinity", action="store_true")
    ap.add_argument("--micro", action="store_true", help="also exercise every Google Benchmark case")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    checks = []

    def run(tool: str, name: str, flags: dict, db: str) -> dict:
        output = (args.out / f"{name}.json").resolve()
        cmd = [str(args.build / "bench" / tool), f"--db={db}", f"--output={output}"]
        cmd += [f"--{k}={v}" for k, v in flags.items()]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if proc.returncode:
            raise RuntimeError(f"{' '.join(cmd)}\n{proc.stderr}")
        data = json.loads(output.read_text())
        validate(data, args.require_perf, args.require_affinity)
        checks.append(name)
        print(f"PASS {name}", flush=True)
        return data

    with tempfile.TemporaryDirectory(prefix="talus-benchmark-smoke-") as temp:
        for engine in args.engines.split(","):
            for rate in (0, 10000):
                common = dict(engine=engine, threads=2, rate=rate, repetitions=2,
                              pin_cpus=1, warmup_ops=3, memtable_size=65536)
                data = run("db_bench", f"{engine}_db_rate{rate}",
                           dict(common, benchmarks=WORKLOADS, num=1003), f"{temp}/db")
                assert [r["benchmark"] for r in data["results"]] == WORKLOADS.split(",")
                for entry in data["results"]:
                    for r in entry["repetitions"]:
                        assert r["ops"] == 1003, r
                        if engine == "talus" and entry["benchmark"] == "readrandom":
                            assert r["engine_stats"]["super_version_slow_path"] == r["initial_engine_stats"]["super_version_slow_path"], r
                        if rate:
                            assert r["seconds"] >= 0.09, r
                for distribution in ("uniform", "zipfian"):
                    for workload in "abcdef":
                        data = run("ycsb", f"{engine}_ycsb_{workload}_{distribution}_rate{rate}",
                                   dict(common, workload=workload, distribution=distribution,
                                        records=503, operations=503, value_size=100), f"{temp}/ycsb")
                        assert data["results"][0]["repetitions"][0]["ops"] == 503
                        assert all(r["ops"] == 503 for r in data["results"][1]["repetitions"])
            run("db_bench", f"{engine}_sync", dict(engine=engine, threads=2, num=32,
                sync=1, benchmarks="fillseq,readrandom", pin_cpus=1), f"{temp}/sync")
        # A low arrival rate must not make --duration wait for the next far-future op.
        data = run("db_bench", "duration_cap", dict(benchmarks="fillseq", num=10,
                   rate=0.1, duration=0.05), f"{temp}/duration")
        assert data["results"][0]["repetitions"][0]["seconds"] < 1
        # Bad inputs must fail before opening/deleting a database.
        for tool, flag in (("db_bench", "threads=0"), ("db_bench", "num=-1"),
                           ("db_bench", "rate=nan"), ("db_bench", "num=oops"),
                           ("db_bench", "sync=maybe"), ("ycsb", "records=0"),
                           ("ycsb", "operations=-1"), ("ycsb", "max_scan_length=0")):
            proc = subprocess.run([str(args.build / "bench" / tool), f"--{flag}",
                                   f"--db={temp}/invalid"], capture_output=True, timeout=10)
            assert proc.returncode != 0, (tool, flag)
        checks.append("invalid_arguments")
    if args.micro:
        output = (args.out / "micro.json").resolve()
        proc = subprocess.run([str(args.build / "bench" / "micro_bench"),
                               "--benchmark_min_time=0.001s", "--benchmark_repetitions=1",
                               f"--benchmark_out={output}", "--benchmark_out_format=json"],
                              capture_output=True, text=True, timeout=300)
        assert proc.returncode == 0, proc.stderr
        data = json.loads(output.read_text())
        assert data["benchmarks"]
        for b in data["benchmarks"]:
            # Hardware CRC is legitimately unavailable on some machines.
            assert not b.get("error_occurred") or "no hardware CRC32C" in b.get("error_message", ""), b
        checks.append("microbenchmarks")
    (args.out / "validation.json").write_text(json.dumps(dict(checks=checks,
        require_perf=args.require_perf, require_affinity=args.require_affinity), indent=2) + "\n")
    print(f"Passed {len(checks)} benchmark checks", flush=True)


if __name__ == "__main__":
    main()
