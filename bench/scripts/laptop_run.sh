#!/bin/bash
# Runs the Talus experiments on a bare-metal x86-64 laptop booted into Linux
# (typically an Ubuntu live USB), where every hardware counter, including
# LLC misses, is available. See docs/LAPTOP.md for the full walkthrough.
#
#   bench/scripts/laptop_run.sh --install              # first run: install packages, then everything
#   bench/scripts/laptop_run.sh                        # read-path experiments 3,4,6,7
#   bench/scripts/laptop_run.sh --full /media/ssd/db   # everything, DBs on a real disk
#
# Options:
#   --install        apt-install the build and perf packages first (needs sudo + internet)
#   --only LIST      experiments to run (default 3,4,6,7: the ones that need no real disk)
#   --full DB_DIR    run bench/scripts/full_run.sh instead (all experiments, engine sweeps,
#                    YCSB) with databases in DB_DIR, which must not be a RAM filesystem
#   --cpus LIST      CPUs to use (default: one hardware thread per performance core)
#   --out DIR        results directory (default ~/talus-laptop-results)
#   --skip-build     reuse an existing build/compare
#
# Run it as your normal user from the repository root; it calls sudo itself.
# It ends by packing everything into one .tar.gz to copy off the machine.
set -euo pipefail

ONLY=3,4,6,7 FULL_DB="" CPUS="" OUT="$HOME/talus-laptop-results" INSTALL=0 BUILD=1
while [[ $# -gt 0 ]]; do
  case $1 in
    --install) INSTALL=1 ;;
    --only) ONLY=$2; shift ;;
    --full) FULL_DB=$2; shift ;;
    --cpus) CPUS=$2; shift ;;
    --out) OUT=$2; shift ;;
    --skip-build) BUILD=0 ;;
    -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done

cd "$(dirname "$0")/../.."
say() { printf '\n== %s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[[ $(uname -s) == Linux ]] || die "run this inside Linux (an Ubuntu live USB), not Windows or WSL"
[[ $(uname -m) == x86_64 ]] || die "this script expects an x86-64 laptop"
[[ $EUID -ne 0 ]] || die "run as your normal user; the script uses sudo where needed"
mkdir -p "$OUT"
LOG="$OUT/laptop_run.log"
exec > >(tee -a "$LOG") 2>&1

# ---- 1. Packages ---------------------------------------------------------------------------
if [[ $INSTALL == 1 ]]; then
  say "Installing packages"
  sudo add-apt-repository -y universe
  sudo apt-get update -qq
  sudo apt-get install -y -qq g++ cmake ninja-build git python3 python3-matplotlib \
    util-linux linux-tools-common
  # perf ships per kernel version; a live USB's kernel may lack its package.
  sudo apt-get install -y -qq "linux-tools-$(uname -r)" || sudo apt-get install -y -qq linux-tools-generic
fi
for tool in cmake ninja g++ python3 taskset; do
  command -v "$tool" > /dev/null || die "$tool is missing: rerun with --install"
done
# /usr/bin/perf is a wrapper that fails when the running kernel's tools are
# missing; any installed version works for counting events.
PERF=perf
if ! perf --version > /dev/null 2>&1; then
  PERF=$(ls -d /usr/lib/linux-tools/*/perf 2> /dev/null | sort -V | tail -1 || true)
  [[ -n $PERF ]] || die "perf is missing: rerun with --install"
fi
python3 -c 'import matplotlib' 2> /dev/null || die "python3-matplotlib is missing: rerun with --install"

# ---- 2. Machine checks -------------------------------------------------------------------------
say "Checking the machine"
VENDOR=$(grep -m1 vendor_id /proc/cpuinfo | awk '{print $3}')
MODEL=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')
RAM_GB=$(awk '/MemTotal/ {printf "%d", $2 / 1048576}' /proc/meminfo)
VIRT=$(systemd-detect-virt 2> /dev/null || true)
echo "CPU: $MODEL ($VENDOR), RAM: ${RAM_GB} GB, virtualization: ${VIRT:-unknown}"
[[ $VIRT == none ]] || warn "this looks like a VM ($VIRT): hardware counters may be missing"
(( RAM_GB >= 8 )) || warn "under 8 GB of RAM: the build and the 5M-key experiments may run out of memory"
ON_AC=unknown
for ps in /sys/class/power_supply/*; do
  [[ -f $ps/type && $(cat "$ps/type") == Mains ]] && ON_AC=$(cat "$ps/online")
done
[[ $ON_AC != 0 ]] || die "the laptop is on battery: plug in the charger (battery power throttles the CPU)"

# Performance cores: hybrid Intel CPUs (12th gen and later) list them in
# /sys/devices/cpu_core/cpus. Use one hardware thread per physical core, so no
# two workers share a core through SMT.
if [[ -z $CPUS ]]; then
  CPUS=$(python3 - << 'EOF'
import pathlib, subprocess
def expand(spec):
    out = set()
    for part in spec.strip().split(","):
        if part:
            a, _, b = part.partition("-")
            out.update(range(int(a), int(b or a) + 1))
    return out
p = pathlib.Path("/sys/devices/cpu_core/cpus")
candidates = expand(p.read_text()) if p.exists() else None
seen, chosen = set(), []
for line in subprocess.run(["lscpu", "-p=CPU,CORE,SOCKET"], capture_output=True, text=True).stdout.splitlines():
    if line.startswith("#"):
        continue
    cpu, core, socket = (int(x) for x in line.split(","))
    if candidates is not None and cpu not in candidates:
        continue
    if (socket, core) not in seen:
        seen.add((socket, core))
        chosen.append(cpu)
print(",".join(map(str, chosen)))
EOF
)
  if [[ -e /sys/devices/cpu_core/cpus ]]; then
    echo "Hybrid CPU: using performance cores only (E-cores: $(cat /sys/devices/cpu_atom/cpus 2> /dev/null || echo none))"
  fi
fi
NCPU=$(tr ',' '\n' <<< "$CPUS" | grep -c .)
MAX_T=1
while (( MAX_T * 2 <= NCPU && MAX_T * 2 <= 8 )); do MAX_T=$((MAX_T * 2)); done
FIRST_CPU=${CPUS%%,*}
echo "CPUs: $CPUS ($NCPU cores) -> threads up to $MAX_T"

# ---- 3. Counters and CPU frequency ---------------------------------------------------------------
say "Enabling hardware counters and the performance governor"
sudo sysctl -q -w kernel.perf_event_paranoid=1
if compgen -G '/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor' > /dev/null; then
  echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor > /dev/null || true
fi
PERF_CHECK="$OUT/perf_check.csv"
taskset -c "$FIRST_CPU" "$PERF" stat -x, -o "$PERF_CHECK" \
  -e cycles,instructions,L1-dcache-load-misses,LLC-load-misses,branch-misses,r10d1 \
  -- python3 -c 'sum(range(3 * 10**6))' || true
supported() { grep -E ",$1(,|/)|/$1/," "$PERF_CHECK" | grep -q -E '^[0-9]'; }
OPTIONAL=()
for ev in cycles instructions; do supported "$ev" || die "perf cannot count $ev: no PMU access (is this a VM?)"; done
supported LLC-load-misses || { warn "LLC misses not supported here"; OPTIONAL+=(llc_misses); }
if [[ $VENDOR != GenuineIntel ]] || ! supported r10d1; then OPTIONAL+=(l2_misses); fi
OPT_FLAG=""
(( ${#OPTIONAL[@]} )) && OPT_FLAG="--optional-perf=$(IFS=,; echo "${OPTIONAL[*]}")"
echo "Counters: $(grep -E '^[0-9<]' "$PERF_CHECK" | cut -d, -f1,3 | tr '\n' ' ')"
echo "Optional counters for this CPU: ${OPTIONAL[*]:-none}"

# ---- 4. Build ----------------------------------------------------------------------------------
if [[ $BUILD == 1 ]]; then
  JOBS=$(( RAM_GB / 2 )); (( JOBS >= 1 )) || JOBS=1; (( JOBS <= $(nproc) )) || JOBS=$(nproc)
  say "Building (release, with LevelDB and RocksDB, $JOBS jobs; downloads dependencies)"
  cmake -S . -B build/compare -G Ninja -DCMAKE_BUILD_TYPE=Release -DTalus_BENCH_COMPARE=ON > "$OUT/configure.log"
  cmake --build build/compare --parallel "$JOBS" > "$OUT/build.log" || die "build failed: see $OUT/build.log"
fi

# ---- 5. Run ------------------------------------------------------------------------------------
say "Smoke check with every available counter required"
python3 bench/scripts/smoke.py --build build/compare --engines talus --require-perf --require-affinity \
  $OPT_FLAG --out "$OUT/smoke" > "$OUT/smoke.log" 2>&1 || die "smoke check failed: see $OUT/smoke.log"
tail -1 "$OUT/smoke.log"

if [[ -n $FULL_DB ]]; then
  mkdir -p "$FULL_DB"
  FS=$(df --output=fstype "$FULL_DB" | tail -1)
  [[ $FS != tmpfs && $FS != overlay && $FS != squashfs ]] ||
    die "$FULL_DB is on $FS (RAM): sync-write results would be meaningless; use a real disk"
  say "Full run: experiments, engine sweeps and YCSB (databases on $FULL_DB, $FS)"
  bench/scripts/full_run.sh build/compare "$OUT/full" "$FULL_DB/talus" "$CPUS" "$MAX_T"
else
  DB_DIR="$HOME/talus-laptop-db"
  mkdir -p "$DB_DIR"
  say "Experiments $ONLY (databases in $DB_DIR on $(df --output=fstype "$DB_DIR" | tail -1))"
  python3 bench/scripts/experiments.py --build build/compare --out "$OUT/experiments" --only "$ONLY" \
    --max-threads "$MAX_T" --cpus "$CPUS" --db "$DB_DIR/exp"
  rm -rf "$DB_DIR"
fi

# ---- 6. Pack the results -------------------------------------------------------------------------
{
  echo "model: $MODEL"; echo "vendor: $VENDOR"; echo "ram_gb: $RAM_GB"; echo "cpus_used: $CPUS"
  echo "laptop: $(cat /sys/class/dmi/id/sys_vendor /sys/class/dmi/id/product_name 2> /dev/null | tr '\n' ' ')"
  echo "kernel: $(uname -r)"; echo "git: $(git rev-parse HEAD 2> /dev/null)$(git diff --quiet 2> /dev/null || echo ' (modified)')"
  lscpu
} > "$OUT/machine.txt"
TARBALL="$HOME/talus-laptop-results-$(date +%Y%m%d-%H%M).tar.gz"
tar -czf "$TARBALL" -C "$(dirname "$OUT")" "$(basename "$OUT")"
say "Done. Copy this file off the laptop before you shut down (a live session forgets everything):"
echo "  $TARBALL"
