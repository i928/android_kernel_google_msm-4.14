# sunfish_defconfig / sunfish_defconfig.opt: measured results

| File | SCHEDSTATS |
|---|---|
| `sunfish_defconfig` (default) | on (`CONFIG_SCHEDSTATS=y`) |
| `sunfish_defconfig.opt` | off (as in lopro) |
| `b1c1_defconfig` / `b1c1_defconfig.opt` | same split for crosshatch (not measured yet) |

All other lines are identical. Both files sit on top of lopro port batches 1+2. To build the `.opt` variant, set
`TARGET_KERNEL_CONFIG := sunfish_defconfig.opt` in `device/google/sunfish/BoardConfigLineage.mk`
(`b1c1_defconfig.opt` in `device/google/crosshatch/BoardConfigLineage.mk`). LineageOS `kernel.mk` copies the
file to `.config` and runs `olddefconfig`, so the name doesn't need to end in `_defconfig`.

Resolved `.config` difference: `SCHEDSTATS` and the hidden `SCHED_INFO` (selected only by SCHEDSTATS).
Turning it off loses `/proc/<pid>/schedstat`. ftrace, perfetto and atrace keep working.
The runtime cost is real: `/system/etc/init/atrace.rc` writes `sched_schedstats=1` at boot.

## SCHEDSTATS A/B (sunfish, 2026-11-21/22)

Bench: `schedbench` (static arm64): hackbench 10x400 + pipe ping-pong 200k round trips, 5 rounds.

| Build | Config | Uptime | ping-pong (us/round trip) | hackbench (s) |
|---|---|---|---|---|
| #45 | base, SCHEDSTATS on | 6248 s | 14.49-15.12, median 14.79 | 1.011-1.231, median 1.049 |
| #46 | batches 1+2, SCHEDSTATS **off** (= .opt) | 600 s | 13.57-14.20, median **13.96** | 0.985-1.122, median 1.083 |
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
