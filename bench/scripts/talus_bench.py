"""Shared helpers for the Talus benchmark scripts.

- hardware_info(): CPU model, cores, SMT state, RAM, disk, kernel/OS and CPU
  frequency governor, recorded next to every result set.
- run_db_bench() / run_ycsb() / run_micro(): run the C++ tools with CPU
  pinning and return their parsed JSON (the raw JSON is also saved).
- plot_*(): matplotlib figures used by sweep.py and experiments.py.
"""

from __future__ import annotations

import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterable

import matplotlib

matplotlib.use("Agg")  # Headless: files only.
import matplotlib.pyplot as plt  # noqa: E402

IS_LINUX = sys.platform.startswith("linux")


# ---- Hardware information ------------------------------------------------------------------

def _read(path: str) -> str | None:
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def _cmd(args: list[str]) -> str | None:
    try:
        return subprocess.run(args, capture_output=True, text=True, check=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def hardware_info(db_path: str | None = None) -> dict:
    info: dict = {
        "hostname": platform.node(),
        "os": platform.platform(),
        "kernel": platform.release(),
        "python": platform.python_version(),
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "logical_cpus": os.cpu_count(),
    }
    if IS_LINUX:
        cpuinfo = _read("/proc/cpuinfo") or ""
        m = re.search(r"^model name\s*:\s*(.+)$", cpuinfo, re.M)
        info["cpu_model"] = m.group(1) if m else platform.processor()
        cores = {(p, c) for p, c in re.findall(r"physical id\s*:\s*(\d+).*?core id\s*:\s*(\d+)", cpuinfo, re.S)}
        info["physical_cores"] = len(cores) or None
        info["smt_active"] = _read("/sys/devices/system/cpu/smt/active")
        meminfo = _read("/proc/meminfo") or ""
        m = re.search(r"MemTotal:\s*(\d+) kB", meminfo)
        info["ram_bytes"] = int(m.group(1)) * 1024 if m else None
        info["cpu_governor"] = _read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
        info["cpu_max_khz"] = _read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq")
        info["transparent_hugepages"] = _read("/sys/kernel/mm/transparent_hugepage/enabled")
        info["perf_event_paranoid"] = _read("/proc/sys/kernel/perf_event_paranoid")
        info["disks"] = _cmd(["lsblk", "-d", "-o", "NAME,MODEL,SIZE,ROTA,TRAN"])
    elif sys.platform == "darwin":
        info["cpu_model"] = _cmd(["sysctl", "-n", "machdep.cpu.brand_string"])
        info["physical_cores"] = int(_cmd(["sysctl", "-n", "hw.physicalcpu"]) or 0) or None
        info["smt_active"] = str(info["logical_cpus"] != info["physical_cores"])
        info["ram_bytes"] = int(_cmd(["sysctl", "-n", "hw.memsize"]) or 0) or None
        info["cpu_governor"] = "n/a (macOS)"
        info["disks"] = _cmd(["diskutil", "info", "/"])
    if db_path:
        df = _cmd(["df", "-h", str(Path(db_path).parent if Path(db_path).parent.exists() else "/")])
        info["db_filesystem"] = df
    return info


# ---- Running the tools ------------------------------------------------------------------------

def _pin_prefix(cpus: str | None) -> list[str]:
    """Restricts the whole benchmark process to `cpus` (e.g. "0-7") on Linux."""
    if cpus and IS_LINUX and shutil.which("taskset"):
        return ["taskset", "-c", cpus]
    return []


def _run_tool(binary: Path, flags: dict, out_json: Path, cpus: str | None) -> dict:
    out_json.parent.mkdir(parents=True, exist_ok=True)
    args = [str(binary)] + [f"--{k}={_flag_value(v)}" for k, v in flags.items()] + [f"--output={out_json}"]
    cmd = _pin_prefix(cpus) + args
    print("  $", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{binary.name} failed ({proc.returncode}):\n{proc.stderr[-3000:]}")
    for line in proc.stderr.splitlines():
        if " ops/s" in line:
            print("    " + line, flush=True)
    return json.loads(out_json.read_text())


def _flag_value(v) -> str:
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, (list, tuple)):
        return ",".join(str(x) for x in v)
    return str(v)


def run_db_bench(build: Path, out_json: Path, cpus: str | None = None, **flags) -> dict:
    return _run_tool(build / "bench" / "db_bench", flags, out_json, cpus)


def run_ycsb(build: Path, out_json: Path, cpus: str | None = None, **flags) -> dict:
    return _run_tool(build / "bench" / "ycsb", flags, out_json, cpus)


def run_micro(build: Path, out_json: Path, benchmark_filter: str, repetitions: int = 1,
              cpus: str | None = None) -> dict:
    out_json.parent.mkdir(parents=True, exist_ok=True)
    cmd = _pin_prefix(cpus) + [
        str(build / "bench" / "micro_bench"),
        f"--benchmark_filter={benchmark_filter}",
        f"--benchmark_repetitions={repetitions}",
        "--benchmark_format=json",
        f"--benchmark_out={out_json}",
        "--benchmark_out_format=json",
    ]
    print("  $", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    return json.loads(out_json.read_text())


def result(data: dict, benchmark: str) -> dict:
    """The entry for `benchmark` in a db_bench/ycsb result file."""
    for r in data["results"]:
        if r["benchmark"] == benchmark:
            return r
    raise KeyError(benchmark)


def median_run(entry: dict) -> dict:
    """The repetition with the median throughput."""
    reps = sorted(entry["repetitions"], key=lambda r: r["ops_per_sec"])
    return reps[len(reps) // 2]


def save_json(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2))


# ---- Plots ------------------------------------------------------------------------------------

def _finish(fig, path: Path, title: str) -> None:
    fig.suptitle(title)
    fig.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=130)
    plt.close(fig)
    print(f"  wrote {path}", flush=True)


def plot_throughput_vs_threads(series: dict[str, list[tuple[int, float]]], path: Path, title: str,
                               ylabel: str = "ops/s") -> None:
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for label, points in series.items():
        points = sorted(points)
        ax.plot([p[0] for p in points], [p[1] for p in points], marker="o", label=label)
    ax.set_xlabel("threads")
    ax.set_ylabel(ylabel)
    ax.set_xscale("log", base=2)
    ax.grid(alpha=0.3)
    ax.legend()
    _finish(fig, path, title)


def plot_latency_cdf(series: dict[str, list[list[float]]], path: Path, title: str) -> None:
    """Percentile plot: x = percentile on a 'nines' scale, y = latency (log)."""
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for label, cdf in series.items():
        xs = [min(p, 99.9999) for _, p in cdf]
        ys = [v / 1e3 for v, _ in cdf]
        ax.plot([1 / (1 - x / 100) for x in xs], ys, label=label)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ticks = [2, 10, 100, 1000, 10000, 100000]
    ax.set_xticks(ticks)
    ax.set_xticklabels(["50%", "90%", "99%", "99.9%", "99.99%", "99.999%"])
    ax.set_xlabel("percentile")
    ax.set_ylabel("latency (µs)")
    ax.grid(alpha=0.3, which="both")
    ax.legend()
    _finish(fig, path, title)


def plot_timeseries(series: dict[str, list[dict]], path: Path, title: str,
                    metrics: Iterable[str] = ("p99", "p999", "p9999")) -> None:
    metrics = list(metrics)
    fig, axes = plt.subplots(len(metrics), 1, figsize=(8, 2.6 * len(metrics)), sharex=True)
    axes = [axes] if len(metrics) == 1 else axes
    for ax, metric in zip(axes, metrics):
        for label, rows in series.items():
            ax.plot([r["second"] for r in rows], [r[metric] / 1e3 for r in rows], label=label)
        ax.set_ylabel(f"{metric} (µs)")
        ax.set_yscale("log")
        ax.grid(alpha=0.3)
    axes[0].legend()
    axes[-1].set_xlabel("seconds")
    _finish(fig, path, title)


def plot_write_amp(series: dict[str, list[dict]], path: Path, title: str) -> None:
    """Write amplification over time from Talus stats samples."""
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for label, samples in series.items():
        pts = [(s["second"], s["stats"]["write_amplification"]) for s in samples
               if isinstance(s.get("stats"), dict) and "write_amplification" in s["stats"]]
        if pts:
            ax.plot([p[0] for p in pts], [p[1] for p in pts], label=label)
    ax.set_xlabel("seconds")
    ax.set_ylabel("write amplification")
    ax.grid(alpha=0.3)
    ax.legend()
    _finish(fig, path, title)


def plot_bars(values: dict[str, float], path: Path, title: str, ylabel: str) -> None:
    fig, ax = plt.subplots(figsize=(7, 4.5))
    labels = list(values)
    ax.bar(labels, [values[k] for k in labels])
    ax.set_ylabel(ylabel)
    ax.grid(alpha=0.3, axis="y")
    for i, k in enumerate(labels):
        ax.annotate(f"{values[k]:.3g}", (i, values[k]), ha="center", va="bottom")
    _finish(fig, path, title)
