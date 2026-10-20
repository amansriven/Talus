# Running the experiments on a Windows laptop

This guide turns any x86-64 Windows laptop into a bare-metal Linux
benchmark machine for an afternoon, without touching Windows. You boot
Ubuntu from a USB stick, clone the repository, and run one script:
[`bench/scripts/laptop_run.sh`](../bench/scripts/laptop_run.sh).

**Why bother when there is a GCP VM?** The VM's PMU (GCP's STANDARD level)
hides L3 events, so it cannot count **LLC misses**, which experiments 4 and 6
are about (see [GCP_BUDGET.md](GCP_BUDGET.md)). Linux running directly on
the laptop sees every counter the CPU has, and has no hypervisor adding noise.

**Why not just run it in Windows or WSL?** Talus uses POSIX file I/O, so it
doesn't build natively on Windows. WSL2 runs Linux inside a Hyper-V virtual
machine that normally exposes no hardware counters, which would put you back
where the GCP VM is.

Nothing here installs anything on the laptop's disk. When you reboot without
the USB stick, the laptop is exactly as it was.

---

## 1. Check the laptop's specs (in Windows, 2 minutes)

Open **Task Manager → Performance → CPU** and **→ Memory**, or run
`msinfo32`. Note the CPU name, core count and RAM, and look up the result
in this table:

| You see | What it means | What the script does |
|---|---|---|
| **Intel Core 12th gen or newer** (e.g. i7-1260P, i5-1335U, Core Ultra 5/7) | Hybrid CPU: fast P-cores plus slow E-cores. Mixing them makes every number unreliable. | Uses only P-cores, one thread per core. All counters, including L2 and LLC, should work. |
| **Intel Core 6th–11th gen** (e.g. i7-8550U, i5-1135G7) | Every core is the same kind. | Uses one thread per physical core. All counters should work. |
| **AMD Ryzen** | All counters except my L2 counter, which uses an Intel-only event. On many Ryzen generations the generic LLC event is also unsupported, because AMD counts L3 in a separate PMU. | Detects this and marks the missing counters as optional. If LLC turns out unsupported, the laptop adds bare-metal stability but not LLC data. |
| **ARM** (Snapdragon, "Copilot+ PC" with an X Elite/Plus) | Not x86-64. | Not supported: stop here. |
| **Under 8 GB of RAM** | The live session, the RocksDB build and the 5M-key experiments all share RAM. | Warns. Close everything else; it may still fail. |
| **16 GB of RAM or more** | Comfortable. | Builds with up to RAM/2 parallel jobs. |

Threads in the experiments go up to the largest power of two that fits the
chosen cores (capped at 8). A 4-P-core laptop runs 1, 2 and 4 threads, the
same as the GCP VM, which makes the two directly comparable.

## 2. Before you reboot

1. **BitLocker recovery key.** Most Windows 11 laptops encrypt the disk.
   Booting from USB can make Windows ask for the recovery key on the next
   start. Get it now from <https://account.microsoft.com/devices/recoverykey>
   (or your organization's portal) and keep it on your phone.
2. **Get the scripts onto GitHub.** The laptop clones the repository, so
   `laptop_run.sh` and the phase 10 changes must be committed and pushed.
3. **A USB stick of 8 GB or more.** It will be erased.
4. **Optional: a second USB stick or drive** to carry the results back. The
   live session forgets everything when it shuts down.
5. **Optional: an external SSD** if you also want the disk-heavy experiments
   (section 6). It will be reformatted.

## 3. Make the Ubuntu USB stick (in Windows, about 15 minutes)

1. Download **Ubuntu 24.04 LTS Desktop** (the `amd64` ISO, about 6 GB) from
   <https://ubuntu.com/download/desktop>.
2. Download **Rufus** from <https://rufus.ie>. It runs without installing.
3. Plug in the stick, open Rufus and set:
   - **Device:** your USB stick. Check the size, so you don't erase another drive.
   - **Boot selection:** click SELECT and choose the Ubuntu `.iso`.
   - **Partition scheme:** GPT.
   - **Target system:** UEFI.
4. Click **START**. If Rufus asks between ISO image and DD image mode,
   choose **ISO**. Wait until the status bar says **READY**.

**balenaEtcher** works too. Secure Boot can stay on; Ubuntu boots with it.

## 4. Boot into Ubuntu

1. **Plug in the charger.** On battery the CPU throttles, and the script
   refuses to run.
2. Boot from the stick. The easiest way, from inside Windows: **Settings →
   System → Recovery → Advanced startup → Restart now → Use a device →** your
   USB stick (often listed as "EFI USB Device"). Or press the boot-menu key
   repeatedly right after pressing power:

   | Brand | Key |
   |---|---|
   | Dell | F12 |
   | HP | Esc, then F9 |
   | Lenovo | F12 (or the small Novo button) |
   | ASUS | Esc or F8 |
   | Acer | F12 (may need enabling in BIOS with F2) |
   | MSI | F11 |
   | Microsoft Surface | Hold Volume Down while pressing power |

3. At the black GRUB menu, choose **Try or Install Ubuntu**. When the
   installer window opens, choose **Try Ubuntu**. Do **not** click Install.
4. Connect to Wi-Fi (top-right corner).
5. **Stop the laptop from sleeping.** A sleeping laptop pauses the run and
   ruins its timings. Open a terminal (Ctrl+Alt+T) and run:
   ```bash
   gsettings set org.gnome.desktop.session idle-delay 0
   gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type 'nothing'
   ```

**Back to Windows afterwards:** copy the results off first (section 7), then
shut Ubuntu down, unplug the stick and power on. The live session keeps
nothing.

## 5. Clone and run (in the Ubuntu terminal)

```bash
sudo apt-get update && sudo apt-get install -y git
git clone https://github.com/amansriven/Talus.git
cd Talus
bench/scripts/laptop_run.sh --install
```

That's the whole run. `--install` installs the compiler, CMake, Ninja,
matplotlib and `perf` first. If you rerun later in the same session, drop it.
Expect roughly 10–25 minutes to build (it downloads and compiles LevelDB and
RocksDB too) and 30–60 minutes of experiments. Leave the laptop alone while
it runs: browsing or other programs show up in the numbers.

What the script does, in order:

1. **Checks the machine:** Linux on x86-64, not a VM, on mains power, enough RAM.
2. **Picks CPUs:** one hardware thread per performance core (section 1).
3. **Sets up counters:** `perf_event_paranoid=1` and the `performance` CPU
   governor. It then runs `perf stat` on the first chosen core to see which
   counters this CPU supports, and marks any missing ones as optional instead
   of failing on them.
4. **Builds** a release build with LevelDB and RocksDB (`build/compare`).
5. **Runs a smoke check** that requires every supported counter on every
   benchmark worker. If this fails, nothing else runs.
6. **Runs experiments 3, 4, 6 and 7**, the read-path and cache experiments:
   SuperVersion acquisition, bloom filters, pread vs mmap with huge pages,
   and skiplist prefetching. They don't depend on the disk, which matters
   because a live session stores files in RAM.
7. **Packs everything** into `~/talus-laptop-results-<date>.tar.gz`: raw JSON,
   tables, plots, the `perf stat` check, `lscpu`, the laptop model and the git
   commit.

Useful options (`--help` lists them all):

| Option | Use |
|---|---|
| `--only 4,6` | Run just these experiments. |
| `--cpus 0,2,4,6` | Override the CPU choice. |
| `--skip-build` | Reuse the existing build on a rerun. |
| `--full DIR` | Run everything on a real disk (next section). |

## 6. Optional: the full suite on an external SSD

Experiments 1, 2, 5 and 8 (sync writes, write scaling, the rate limiter, the
spin threshold) and the LevelDB/RocksDB comparisons depend on real fsyncs.
In a live session, `$HOME` is RAM, where fsync costs nothing. To run them,
plug in an external SSD:

```bash
lsblk                              # find the SSD, e.g. /dev/sda; check the SIZE column carefully
sudo mkfs.ext4 -F /dev/sda         # ERASES that drive
sudo mkdir -p /mnt/ssd && sudo mount /dev/sda /mnt/ssd && sudo chown "$USER" /mnt/ssd
bench/scripts/laptop_run.sh --skip-build --full /mnt/ssd
```

The script refuses a directory that lives in RAM. Results from a USB SSD
include the USB link's latency, so report them as "laptop with external USB
SSD", not as a comparison with the GCP VM's internal NVMe.

## 7. Get the results off the laptop

Before you shut down, copy the `.tar.gz` somewhere permanent. Any of these works:

- **Second USB stick:** it appears in the file manager; drag the file onto it.
- **Browser:** upload it to Google Drive from Firefox.
- **Git:** commit it on a branch and push (it's a few MB).

Then put the archive in the repository under `build/phase10/laptop/` on the
Mac, or tell me where it is, and it goes into `docs/PERFORMANCE.md` next to the
GCP and M3 results.

## 8. Troubleshooting

| Problem | Fix |
|---|---|
| The stick doesn't appear in the boot menu | Re-make it in Rufus with GPT and UEFI, or try another USB port. |
| "Secure Boot violation" | Ubuntu normally boots with Secure Boot on. If it doesn't, turn Secure Boot off in the BIOS setup (usually F2 or Del at power-on). |
| The laptop boots straight into Windows | Use the Advanced startup route in section 4; Windows Fast Startup can skip the boot-menu key. |
| Windows asks for a BitLocker recovery key afterwards | Enter the key from section 2. This happens once. |
| `linux-tools-<version>` not found during `--install` | Handled: the script falls back to `linux-tools-generic` and uses that `perf` directly. |
| "perf cannot count cycles" | You are in a VM or WSL, not booted from the USB stick. |
| "LLC misses not supported here" | Expected on many AMD CPUs (section 1). The run continues without them. |
| Build killed (out of memory) | Close the browser and rerun; the build continues where it stopped. |
| "on battery" | Plug in the charger. If it's plugged in but still reported, check the cable; the script reads `/sys/class/power_supply`. |
| Smoke check fails | Read `~/talus-laptop-results/smoke.log`; the last lines name the failing check. |
