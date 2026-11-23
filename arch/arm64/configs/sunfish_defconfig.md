# sunfish_defconfig / sunfish-opt_defconfig: measured results

| File | SCHEDSTATS | Tracing (FTRACE) |
|---|---|---|
| `sunfish_defconfig` (default) | on | on |
| `sunfish-opt_defconfig` | off (as lopro) | off (lopro batch 3, as lopro) |
| `b1c1_defconfig` | on | on |
| `b1c1-opt_defconfig` | off | on |

All four sit on top of lopro port batches 1+2. Everything else is identical between a defconfig and its `-opt`.
Resolved `.config` difference, sunfish defconfig -> -opt (olddefconfig, clang r614150): SCHEDSTATS, SCHED_INFO,
FTRACE, BPF_EVENTS, PROBE_EVENTS, UPROBE_EVENTS, UPROBES, TRACING_EVENTS_GPIO, BRANCH_PROFILE_NONE.

- SCHEDSTATS off also drops the hidden SCHED_INFO (selected only by SCHEDSTATS): loses `/proc/<pid>/schedstat`.
  The cost of having it on is live: `/system/etc/init/atrace.rc` writes `sched_schedstats=1` at boot.
- FTRACE off: no perfetto/atrace/systrace, no tracepoint BPF (gpuWork, memevents), no `/sys/kernel/tracing` events.
  Not measured yet.

**How to build the -opt variant:** sunfish device branch `lineage-23.2_pub-evo-opt` sets
`TARGET_KERNEL_CONFIG := sunfish-opt_defconfig` (`device/google/sunfish/BoardConfigLineage.mk`). For crosshatch, set
`b1c1-opt_defconfig` in `device/google/crosshatch/BoardConfigLineage.mk`. The name must end in `_defconfig`:
LineageOS `kernel.mk` also runs `make <TARGET_KERNEL_CONFIG>` for dtbo.img (a `sunfish_defconfig.opt` name failed
there with "No rule to make target", 2026-11-22).

## SCHEDSTATS A/B (sunfish, 2026-11-21/22)

Bench: `schedbench` (static arm64): hackbench 10x400 + pipe ping-pong 200k round trips, 5 rounds.

| Build | Config | Uptime | ping-pong (us/round trip) | hackbench (s) |
|---|---|---|---|---|
| #45 | base, SCHEDSTATS on | 6248 s | 14.49-15.12, median 14.79 | 1.011-1.231, median 1.049 |
| #46 | batches 1+2, SCHEDSTATS **off** (-opt minus batch 3) | 600 s | 13.57-14.20, median **13.96** | 0.985-1.122, median 1.083 |
| #47 | batches 1+2, SCHEDSTATS **on** (= defconfig) | 600 s | 14.63-15.47, median **15.04** | 0.965-1.101 |
| #47 | same, second run | 7.7 h | 15.23-15.87, median 15.39 | - |
| #49 | sunfish-opt_defconfig: SCHEDSTATS **off** + FTRACE **off** (OTA build) | 600-674 s | 13.20-15.29, median **14.78** (10 rounds) | 0.978-1.123, median 1.035 |

**Result:** SCHEDSTATS on makes each wakeup/context-switch round trip about 8-10% slower (13.96 vs 15.04/15.39 us,
and the ranges don't overlap). Hackbench throughput shows no measurable cost.
Default stays on (user decision 2026-11-21, review later).

**#49 (-opt, 2026-11-22) does NOT reproduce the #46 gain.** Its ping-pong median, 14.78 us, is in line with the SCHEDSTATS-on
builds (#45 14.79, #47 15.04), not with #46 (13.96), and its range is wider. FTRACE off cannot plausibly add cost
(function tracing is off in every build). So the 8-10% SCHEDSTATS figure is in doubt: #46 may have been a good run,
or #49's conditions differed. #49 came from a full OTA (new ROM build, first boot); #45-#47 were kernel-only flashes
on the same ROM. CPU was 96.5% idle during #49 (loadavg 4-7 is mostly non-CPU load and doesn't mean the CPU is busy).
Next, cheapest: on one SCHEDSTATS=y kernel, toggle `/proc/sys/kernel/sched_schedstats` 0/1 between interleaved
rounds. Same build, same boot, no reflash.

**Runtime toggle, #50 (default sunfish_defconfig, OTA build, 2026-11-22), settles it.** Same boot, uptime 611 s, CPU 98% idle,
8 interleaved pairs of `sched_schedstats` 0/1 (`schedtoggle.sh`, order alternates per pair):

| sched_schedstats | ping-pong median (range) | mean | hackbench median |
|---|---|---|---|
| 0 | **14.39** us (13.76-15.09) | 14.45 | 1.049 s |
| 1 (boot default, atrace.rc) | **15.26** us (14.54-15.74) | 15.16 | 1.054 s |

Collecting schedstats costs ~0.7 us per round trip (~5-6%): 7 of 8 pairs are slower with it on, mean +0.71 us.
Hackbench throughput shows no difference. The real figure is ~5%, not the 8-10% from #46 vs #47. #49 (compiled out,
median 14.78) sits between the two, within its wide range. The same gain comes from writing
`sched_schedstats=0` at runtime, with SCHEDSTATS kept compiled in (`/proc/<pid>/schedstat` comes from SCHED_INFO and
stays).

**Boot-time fix, #51 (2026-11-22):** device branch `lineage-23.2_pub-evo-schedstats` (af10453e) adds
`/system_ext/etc/init/init.schedstats.rc`, which writes `sched_schedstats=0` on `sys.boot_completed=1` (system_ext
so it runs as init; vendor_init cannot write proc_sched). Verified: 0 at boot, 0 avc; ping-pong median 14.54 us
(13.65-15.04, 8 rounds), hackbench 1.019 s, matching the toggle's off side.

#49 other results: MemTotal 5716888 kB (+296 kB vs #47); 34 modules; 0 avc; all-3 integrity PASS.
FTRACE=n here does not remove tracefs: TRACING/EVENT_TRACING stay =y (selected by other options), 94 event dirs
remain (95 before). It does drop tracepoint BPF: BpfLoader-rs gpuMem, gpuWork, bpfMemEvents fail (EINVAL), and
UprobeStats BitmapAllocation fails (UPROBES off). Boot is unaffected.

## Batches 1+2 vs base (#45 -> #46/#47)

- Boot: autorun PASS, 24.8 s (#46) / 24.6 s (#47); 0 avc, 0 module errors.
- MemTotal +1592 kB (5715000 -> 5716592 kB); modules loaded 41 -> 34 (7 unused dropped).
- Tracing: 95 event dirs, Wi-Fi / BT / mobile data up.
- Resolved `.config` vs lopro: 193 -> ~85 differing symbols.

## Batch 4: COMPAT_VDSO (sunfish, #52, 2026-11-22)

`CONFIG_COMPAT_VDSO=y` in `sunfish_defconfig` and `sunfish-opt_defconfig`. The arm64 Makefile now builds vdso32 with
clang + ld.lld when there is no 32-bit GNU toolchain (LineageOS NO_GCC; LineageOS had reverted the option for that
reason). Measured with `vdsobench32` (freestanding 32-bit, 3 x 2M calls of `clock_gettime(CLOCK_MONOTONIC)`):

| Path | ns/call |
|---|---|
| syscall (#51 and #52) | 113.7-120.9 |
| `__vdso_clock_gettime` (#52) | **66.8** (-41%) |

All 32-bit processes now map `[vdso]`: zygote, zygiskd32, audio HAL, cas, media.c2, drmserver, mediaserver,
webview_zygote. Boot 24.7 s, 0 avc, 0 crashes, MemTotal unchanged, all-3 integrity PASS.
Not taken from lopro batch 4: MSM_TZ_LOG (debugfs-only, DEBUG_FS is off), IRQ_SBALANCE (sunfish runs vendor
msm_irqbalance; only worth an A/B with that service disabled).
