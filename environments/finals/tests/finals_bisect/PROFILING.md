# Lessons: profiling THE FINALS' freezes (2026-11-09)

The freeze investigation is **paused, not done**. The findings are in
`../rosetta_nt/NOTES.md`, "Freeze hunt, continued and PAUSED". This page covers what the tools
taught, so the next attempt starts with the right one.

## Use Time Profiler (`freeze_profile.py`), not `sample`

| | `sample` (`freeze_sampler.py`, superseded) | Time Profiler (`freeze_profile.py`) |
|---|---|---|
| How | Suspends every thread through the task port for each stack | Kernel sampling (kperf) of threads on the CPU, 1 kHz |
| Stacks on the game | 6–13 per 1.5 s (about 200 threads to suspend each time) | about 1,000 per second per busy thread |
| Timing | Triggered after 0.3 s of freeze, started 0.7–1.2 s late: most 0.3–0.9 s freezes were over | Records the whole session; freeze windows are cut out afterwards from the perf file |
| Developer mode | Not needed | Not needed |
| Translated code | x86 return addresses, leaf "Rosetta JIT" | x86 frames and leaf PC (`outer_work` and so on on the probe) |

Both were proven on probes first, a translated macOS busy loop and a PE busy loop under Wine
with the hook, before touching the game. Neither caused any trouble.

## Traps

- **THE FINALS starts two `Discovery.exe` processes.** A short-lived first one also names a
  GameThread and exits after about 30 s; the real game comes after it. A tool that latches onto the
  first process with a GameThread records nothing useful. Follow the newest one.
- **kperf serves one recording at a time.** A second `xctrace record` fails with "_lockKPerf:
  could not lock kperf". The error only shows when that recording ends, and the recording sits
  there doing nothing in the meantime. Stop the previous recording with SIGINT (xctrace saves and
  exits) before starting the next.
- **Under Wine the process's argv is the Windows command line.** Find a probe with
  `pgrep -f '^runtime/build/probe/<name>.exe'`. For the game, the perf file name `wine.<pid>`
  gives the macOS pid.
- **The freeze signature in perf files:** GameThread at 90 % or more while the whole process
  makes fewer than 6,000 NT calls per 0.1 s (normal play is about 40,000).
- **Exporting a trace:** `xctrace export --toc` gives `<start-date>` (wall time of sample 0).
  `--xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'` gives rows whose
  elements refer back to earlier ones by `id`/`ref`; resolve both when parsing.
- **Frames in the anti-cheat's region (0x7000_0000_0000 and up) are only counted, never broken
  down** (CLAUDE.md, hard boundary 3).

## What was ruled out

- **Shader compilation.** D3DMetal's cache (`$(getconf DARWIN_USER_CACHE_DIR)d3dm/Discovery.exe`,
  about 1.4 GB) persists between runs and had stopped growing, yet the runs still froze.
- **Slow pipeline builds.** These happen only in the startup burst, on `PSOPrecompilePool`.
- **Wine syscall cost and the VM lock.**
