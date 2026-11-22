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

**Result:** SCHEDSTATS on makes each wakeup/context-switch round trip about 8-10% slower (13.96 vs 15.04/15.39 us,
and the ranges don't overlap). Hackbench throughput shows no measurable cost.
Default stays on (user decision 2026-11-21, review later).

## Batches 1+2 vs base (#45 -> #46/#47)

- Boot: autorun PASS, 24.8 s (#46) / 24.6 s (#47); 0 avc, 0 module errors.
- MemTotal +1592 kB (5715000 -> 5716592 kB); modules loaded 41 -> 34 (7 unused dropped).
- Tracing: 95 event dirs, Wi-Fi / BT / mobile data up.
- Resolved `.config` vs lopro: 193 -> ~85 differing symbols.
