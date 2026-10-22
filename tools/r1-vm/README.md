# HiBy R1 software test VM

This is a constrained Malta system-emulation guest for testing Compás player
cores against a small Linux userspace and R1 benchmark profile. It does not
model the X1600 board. The current guest CPU is QEMU `24Kf` (MIPS32r2 hard
float), which boots with the upstream Malta kernel. `XBurstR2` stalled before
the serial console, so the CPU model remains a compatibility compromise and
may change as boot support is investigated.

## Build

From the `compas-emulators` checkout, run:

```sh
tools/r1-vm/build.sh
```

The script prepares a local QEMU system binary if one is not already available,
builds the pinned Malta kernel and static BusyBox, then creates persistent ext4
root and SD-card images. `build-images.py` also needs the player core builds,
ROM fixtures, and the `qemu-mipsel` user emulator. Host setup downloads and
unpacks verified RPMs locally; it does not install system packages.

Existing `rootfs.img` and `sdcard.img` files are preserved. To deliberately
replace them, pass `--force-images`; the image builder also refuses to rebuild
while the VM is running. Set `R1_VM_WORK=/path/to/work` to put build products,
images, VM state, and logs elsewhere. Set `CROSS_COMPILE` to override the
default sibling `compas-player` MIPS musl toolchain.

## Run and inspect

```sh
python3 tools/r1-vm/vm.py start
python3 tools/r1-vm/vm.py match-memory
python3 tools/r1-vm/vm.py status --json
python3 tools/r1-vm/vm.py exec --json 'cat /proc/meminfo | head -5; cat /proc/swaps'
python3 tools/r1-vm/vm.py stop
```

The guest uses one virtual CPU, 64 MiB RAM by default, a persistent ext4 root
disk, and a virtual SD card mounted at `/usr/data/mnt/sd_0`. It starts a serial
shell and enables 24 MiB LZ4 zram swap. It does not start adbd, network
services, or board-specific drivers. `vm.py start` uses the selected quota in
`build/r1-vm/calibration.json` when its kernel, images, QEMU, RAM and host identity match; `--cpu-quota` overrides it.

## Calibrate CPU throughput

Boot the VM with the baseline cores and reference games in place, then run:

```sh
python3 tools/r1-vm/calibration.py
```

The calibrator discards a full-length warmup for each core at each quota, measures the five headless reference cases at
100% quota, estimates and refines a common CPU quota, then writes
`build/r1-vm/calibration.json`. This file records the selected quota, VM status,
per-core measurements, and residual errors. Use `--quick` for one 600-frame
trial (its shorter workload can differ from the 1,800-frame reference),
`--quotas 100,75,50,35` for an explicit sweep, or `--dry-run` to inspect
planned commands. `--self-test` checks the parser and quota-fit math without a
running VM.

The R1 times are approximate references from diagnostic headless runs. The
recorded fixture hashes were not tied to the R1 capture, and one global Linux
CPU quota cannot reproduce per-core performance, X1600 timing, thermal limits,
interactive audio/video pacing, or MHz. The quota is a throughput fit, not a
guest frequency setting. Host CPU load, governor and temperature can change
the fit; recalibrate when those conditions change. The profile and selected quota remain provisional because the original ROM
hashes are unavailable. Calibration automatically honors `R1_VM_WORK`; previous
results are kept with timestamped names. `match-memory` adjusts physical RAM
until Linux reports the R1 target of 58,196 KiB usable RAM (within 64 KiB),
then saves the setting for subsequent starts.

Gearboy showed slow and fast timing clusters during initial trials despite
matching output checks. Treat its timing as provisional; preserve the complete
trial range and validate with longer runs. Host monotonic command durations
are recorded alongside guest wall time (including launch/check overhead).

## Compare release and optimized builds

`compare.py` accepts a JSON list of cases, for example:

```json
[{"id":"gba","core":"gpsp","rom":"/usr/data/mnt/sd_0/Games/GBA/240pee_mb.gba"}]
```

```sh
python3 tools/r1-vm/compare.py --cases cases.json --frames 1800 --trials 3 --output build/r1-vm/comparison.json
python3 tools/r1-vm/report-comparison.py --inputs build/r1-vm/comparison.json --output build/r1-vm/report.json --csv build/r1-vm/report.csv
```

The comparison preserves the VM settings, verifies binary/ROM identities,
disables frame skipping, and alternates paired build order after full-length
warmups. It retains every timing, range, correctness flag and raw log.
`device_log` on a case is historical physical-log metadata, not a guest option.

Keep the release binaries in `build/stage` and the new binaries in `build/target`
before creating the images. A normal `make stage` copies current target builds,
so it does not manufacture an unoptimized baseline. Check the release hashes.

The headless benchmark covers core execution, synchronous scaling, software
audio processing and target text pinning. It bypasses the interactive presenter
queue, framebuffer presentation, ALSA writes and audio-period recovery. Matching
FNV hashes describe an additional check window after timing; they do not verify
every timed frame. Selected game files do not by themselves establish that the
script reaches each game's most demanding scene. Three paired trials are
exploratory measurements, not statistical confidence or physical R1 predictions.
