#!/bin/bash
# Phase 10 measurement run: the seven A/B experiments, the engine sweeps
# (closed and open loop) and YCSB A-F against LevelDB and RocksDB.
#
#   bench/scripts/full_run.sh BUILD OUT DB_DIR [CPUS] [MAX_THREADS]
#   bench/scripts/full_run.sh build/compare results /mnt/ssd/talus 0-3 4
#
# BUILD must be configured with -DTalus_BENCH_COMPARE=ON. CPUS is a taskset
# list (Linux only; empty on macOS). DB_DIR should be on the disk under test.
# Every step keeps its raw JSON; a failed step is logged and the run continues.
set -uo pipefail

BUILD=$1 OUT=$2 DB=$3 CPUS=${4:-} T=${5:-4}
cd "$(dirname "$0")/../.."
mkdir -p "$OUT" "$DB"
THREADS=1
for ((t = 2; t <= T; t *= 2)); do THREADS+=",$t"; done
PIN=()
[[ -n $CPUS ]] && PIN=(--cpus "$CPUS")

step() {
  local name=$1; shift
  echo "== $name $(date -u +%H:%M:%S)" | tee -a "$OUT/steps.log"
  if "$@" > "$OUT/$name.log" 2>&1; then echo "PASS $name" | tee -a "$OUT/steps.log"
  else echo "FAIL $name ($?)" | tee -a "$OUT/steps.log"; fi
  rm -rf "${DB:?}"/*
}

S=bench/scripts
step experiments python3 $S/experiments.py --build "$BUILD" --out "$OUT/experiments" \
  --max-threads "$T" --repetitions 3 --db "$DB/exp" ${PIN[@]+"${PIN[@]}"}
step sweep_closed python3 $S/sweep.py --build "$BUILD" --out "$OUT/sweep_closed" \
  --engines talus,leveldb,rocksdb --threads "$THREADS" \
  --benchmarks fillrandom,readrandom,readmissing,seekrandom,readwhilewriting \
  --num 2000000 --repetitions 3 --warmup-ops 20000 --db "$DB/sweep" ${PIN[@]+"${PIN[@]}"}
step sweep_open python3 $S/sweep.py --build "$BUILD" --out "$OUT/sweep_open" \
  --engines talus,leveldb,rocksdb --threads "$T" --benchmarks readrandom,readwhilewriting \
  --num 2000000 --rate 100000 --duration 30 --repetitions 3 --warmup-ops 20000 \
  --db "$DB/open" ${PIN[@]+"${PIN[@]}"}
step ycsb python3 $S/ycsb_sweep.py --build "$BUILD" --out "$OUT/ycsb" \
  --engines talus,leveldb,rocksdb --threads "$T" --records 1000000 --operations 1000000 \
  --repetitions 3 --db "$DB/ycsb" ${PIN[@]+"${PIN[@]}"}
touch "$OUT/DONE"
