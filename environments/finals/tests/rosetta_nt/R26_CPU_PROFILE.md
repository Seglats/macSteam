# Where THE FINALS' CPU time goes (2026-11-25, `macfinals-159`, Steam launch)

Time Profiler over 2 minutes of play (`tests/finals_bisect/cpu_profile.py record`), plus the
counters of the 01:42 session (`STEAMDRIVER_PERF=1`, `perf_report.py`).

## Thread groups (2 min of play, 771 s of CPU = 6.4 cores on average)

| Threads | Cores | What they run |
|---|---|---|
| Background workers | 2.51 | translated x86 (game jobs); Wine's Unix side ≤ 5 % |
| Foreground workers | 1.06 | same |
| **GameThread** | **0.90** | translated x86, ~90 % of one core: **the limit** |
| unnamed threads ("wine") | 0.88 | GPU submission: D3DMetal encode, AGX driver, `IOGPUCommandQueueSubmitCommandBuffers` (21 % of these) |
| RenderThread 0 / RHIThread | 0.28 / 0.21 | game code, D3DMetal (RHIThread ~15 % D3DMetal) |

Counters in steady play (GameThread busy): **GameThread 90 ms user + 6 ms system per 100 ms**,
Wine-handled faults median 0/s, macOS page faults median 2,700/s, mmap 30/s. The syscall fast
path removed the kernel half measured on 2026-11-02/14. Wine's Unix side is the first named frame
in ≤ 7 % of GameThread samples (an upper bound; see below).

## What the profiler can and cannot show here

- The NT hook forces `DISABLE_AOT=1`, so every x86 instruction runs from Rosetta's JIT cache
  (`0x9c9e…`): leaf PCs belong to no module, game and Wine PE code look the same.
- The game's code keeps no frame pointers: GameThread stacks stop after one or two frames. Deeper
  frames in Wine's `ntdll.so` are partly stale (`segv_handler < NtQuerySection < …` chains are
  impossible), so per-function numbers inside ntdll are unreliable. `ntdll.so` loads at
  0x2087bf000; symbolise with the build tree's `ntdll.so` (`nm -n`, nearest symbol below).
- Instruments names the game's PE addresses `wine` (the main executable).
- Native frames (D3DMetal, IOGPU, AGX, libdispatch) symbolise normally.

## Cores (powermetrics and the profile's core column, same session type)

This Mac has 6 **S cores** (fastest, up to 4.6 GHz) and 12 **P cores** in two clusters (up to
4.38 GHz). In play: **S cluster 84 % busy on all six cores, clocked 3.68 GHz (median)**; P clusters
34 % busy at ~3.9 GHz. GPU 47 W, CPU 22 W.

| Thread group | on S cores | S-core load |
|---|---|---|
| GameThread | 96 % | 0.86 |
| Background workers | 35 % | 0.88 |
| Foreground workers | 60 % | 0.64 |
| GPU submission (Metal/D3DMetal) | 80 % | 0.70 |
| Render/RHI/others | ~85 % | ~0.7 |

So GameThread shares a full, clocked-down cluster with work nothing waits on within a frame,
while the P cores have room.

## Wine threads have no QoS class

The Mach policies wineserver sets on every thread (`apply_thread_priority`: precedence, extended,
latency and throughput QoS tiers) **opt the thread out of QoS for good**: afterwards
`pthread_get_qos_class_np` reads 0 and `pthread_set_qos_class_self_np` fails with EPERM (any one of
those four policies does it, checked with a plain x86_64 test). `THREAD_QOS_POLICY` through
`thread_policy_set` is refused from user space (invalid argument). So macOS places Wine threads by
Mach base priority alone. Measured (`prio_spin.exe`, `ps -M`, values ~5 below base for busy
threads): idle 0, lowest 17, below normal 22, normal 26, above normal 31, highest 35, time
critical 58, so base ≈ 22 / 27 / 31 / 36 / 40. Below normal sits in the scheduler's utility band
(20–30), which still spilled onto the S cores.

**On trial: `STEAMDRIVER_BG_THREADS=1`** (`wine-server-bg-threads.patch`, `macfinals-165`): threads
with base priority below normal get importance ≤ -12, base < 20, the background band.

## Leads

1. **Thread QoS** (see above for what was found and tried). wineserver maps Windows priorities to Mach QoS tiers (`server/thread.c`,
   `apply_thread_priority`): BELOW_NORMAL → throughput tier 2 / latency tier 3 (utility), LOWEST
   and IDLE → tier 5 (background). macOS schedules those mostly on efficiency cores; Windows runs
   below-normal threads on every core. UE's background workers (2.5 cores of jobs) change their
   priority ~1,200 times a second, so they may spend time on E-cores while GameThread waits on them
   (GameThread waits ~6.5 % of the time). Each change also costs ~43 µs (server round trip and four
   or five `thread_policy_set` calls, done even when nothing changed). To measure: per-cluster
   load during play (`sudo powermetrics --samplers cpu_power`), then an A/B with below-normal
   threads kept at the normal tiers.
2. **AVX advertised or not** (`STEAMDRIVER_AVX=0`): the game may take different code paths, and
   Rosetta's AVX translation may be slower or faster than its SSE paths. A/B on FPS and
   GameThread load.
3. Small Wine costs: `find_reserved_free_area` (fixed-address map retries; RHIThread ~7 %,
   workers ~3 %), priority changes when nothing changed.

## Thread-band A/B result (2026-11-25 08:27 / 08:32, `macfinals-171`)

Practice range, same spot and view, 120 s Time Profiler each (`cpu-profile/20261125-082704.trace`
= A, switch off; `20261125-083159.trace` = B, `STEAMDRIVER_BG_THREADS=1`, confirmed in the
wineserver's environment). Driver stats `/tmp/finals-ab-{a,b}-*.jsonl` over the same windows.

| | A (off) | B (`STEAMDRIVER_BG_THREADS=1`) |
|---|---|---|
| Process CPU | 819 s (6.8 cores) | 780 s (6.5 cores) |
| Background Workers on the S cluster | 37 % (of 356 s) | **40 %** (of 327 s) |
| Foreground Workers on S | 67 % | 68 % |
| GameThread on S / CPU | 98 % / 112 s | 98 % / 112 s |
| `wine` (GPU submission) on S | 84 % | 84 % |
| Frame rate (avg / min per second) | 131.7 / 109 | 138.5 / 108 |
| Frame time avg / p95 of per-second max | 7.61 / 18.0 ms | 7.24 / 17.4 ms |

**The mechanism did not happen:** the background workers' share of the fastest cores did not
drop (37 → 40 %), so the clamp to importance −12 does not steer them off the S cluster. On Apple
Silicon, cluster placement follows QoS / thread-group recommendations, which Wine threads lack
(see above); base priority alone does not move them. The +5 % frame rate in B is within what a
slightly different view or moment gives in one 2-minute sample each and is not attributed to the
switch. **Conclusion: `STEAMDRIVER_BG_THREADS` stays off (default) — no measurable placement
effect.** A real fix needs the threads to carry a QoS class, which Mach policies set by
`apply_thread_priority` currently prevent.

### 10-minute repeat (2026-11-25 08:43 / 09:08)

A on `macfinals-171` (`cpu-profile/20261125-084331.trace`), B on `macfinals-174` (the C06 fix
only touches window teardown; `20261125-090815.trace`), 600 s each, practice range, similar play.

| | A (off) | B (`STEAMDRIVER_BG_THREADS=1`) |
|---|---|---|
| Process CPU | 3,814 s (6.4 cores) | 3,883 s (6.5 cores) |
| Background Workers on S | 40 % | 39 % |
| Foreground Workers / GameThread / `wine` on S | 68 / 98 / 85 % | 67 / 98 / 85 % |
| Frame rate avg / 1 %-low (per-second) | 138.7 / 121 | 139.1 / 126 |
| Frame time avg / per-second max p95 / p99 | 7.23 / 17.1 / 36.2 ms | 7.22 / 17.2 / 36.1 ms |

Confirms the 2-minute result: **no effect**. The switch stays off.

## Protect bursts at transitions (2026-11-25 09:20, `burst-20261125-092026.trace`)

Leaving the practice range, the GameThread runs bursts of up to ~28,000 `NtProtectVirtualMemory`
calls a second (one page of the game's own image, `PAGE_NOACCESS` → `PAGE_EXECUTE_READ`, per the
stuck dump's memory-call ring) with ~1 `NtContinue` per 1.5 protects, and lots of
`NtFreeVirtualMemory`. Captured by `tests/finals_bisect/burst_watch.py` (Time Profiler, 30 s) with
the perf counters over the same window:

| GameThread, 22.4 s of burst rows | |
|---|---|
| CPU | 11,964 ms |
| `NtProtectVirtualMemory` | 23,329 calls, 214 ms (9.2 µs each) — **1.8 %** |
| `NtFreeVirtualMemory` | 19,146 calls, 200 ms — 1.7 % |
| waits (`NtWaitForSingleObject`) | 5,323 ms (blocked, not CPU) |

Time Profiler agrees: `ntdll.so` is on the GameThread's stack in 8 % of its samples (all Wine
Unix-side work, not only protects); 69 % of samples have no symbolized frame at all —
translated x86 code (Rosetta's code cache at `0x997…`, which includes the game's code and the
fresh translation of each page that just became executable) and exception round trips.

**Conclusion:** Wine's handling of `NtProtectVirtualMemory` is ~2 % of the GameThread's time in
a burst; making it free would not be noticeable. The cost of the bursts is executing and
re-translating the newly-executable pages under Rosetta and delivering the exceptions —
outside what Wine changes, and the protection scheme itself is the game's (not analysed).

### When the bursts happen: transitions only

Counters for the whole of run B's session (`perf/20261125-090611/wine.78478`, 873 s; burst row =
≥ 50 GameThread protects in 100 ms):

| When | Burst length |
|---|---|
| 09:06:27, start-up / loading | ~20 s (intermittent) |
| 09:07:28, entering the practice range | 7.5 s |
| 09:20:22, leaving the range | ~14 s, plus two short ones (1.3 s, 0.8 s) |
| 10 minutes of play in the range (A/B window) | **none** — no protects, no `NtContinue`, on any thread |

About 19 s of burst rows in 873 s. In-match performance (frame rate, the p99 hitches, the
GameThread limit) is not affected by them; they cost loading time and the menu's
responsiveness right after a transition (the 5–10 s of dead buttons after leaving, C14).

## Maybe in the future: a fault fast path

Where the extra burst cost goes, compared with native x86:

1. **Exception delivery** — each access fault goes kernel → Rosetta's exception server →
   emulated signal → Wine → the game's handler. The same route costs ~16–22 µs per syscall
   (R17) against ~2 µs on Proton; at ~15,000 exceptions a second in a burst that is an
   estimated ~0.25–0.3 s per second of the GameThread (estimate from the syscall measurement;
   the fault path itself not timed).
2. **Fresh translation** of each page that just became executable (inside Rosetta; not
   changeable).
3. Translated code running slower than native (not changeable).

Only (1) is within reach: route these access faults straight into Wine's fault handler, the way
the syscall fast path routes the invalid-class `syscall`. **Not pursued (2026-11-25, Rasmus):**
the bursts happen only at transitions (~19 s in a 15-minute session, none during play), so the
gain would be a few seconds of faster loading / menu responsiveness, while the change would
widen the Rosetta behaviour-correction scope (today: POPF/PUSHF/IRET flags and the
invalid-class `syscall` path only — any widening is Rasmus's decision) onto the exception path
near C10's kernel panics. Revisit if the bursts turn out to happen during play in some mode, or
if transition time becomes the thing to fix. Same constraints as the syscall path would apply:
in-process, fail-closed, no `mach_vm_*` writes on translated mappings.
