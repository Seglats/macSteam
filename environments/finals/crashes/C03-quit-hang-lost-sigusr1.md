# C03: quitting hangs, threads wait forever on loader/threadpool/winmm locks

| | |
|---|---|
| First seen | 2026-11-14 15:49, `macfinals-113`, `STEAMDRIVER_PERF=1`; 1 of 2 quits on `-113` |
| Origin | ours: the fast path's diverts at macOS shared-cache syscall sites, made fatal by C01's deferral |
| Status | fixed in `macfinals-114` (reproducer passes; not yet confirmed in game) |
| Reproducer | `python3 tests/finals_bisect/teardown_check.py --mode waitexit` |
| Fix | [fixes/C03-fix.md](fixes/C03-fix.md) |

## What you see

The game restores the display mode and closes its window, then the process never exits. A
minute later the Wine log fills with `RtlpWaitForCriticalSection ... wait timed out`:

| Lock | Held by |
|---|---|
| `loader_section` (the exiting thread needs it) | 04b8 |
| `threadpool.cs`, `timerqueue.cs` | 0190 |
| `WINMM_cs` | 074c |

## Evidence

- First lock-timeout hang in 46 launches; first launch on `-113`.
- `perf_report.py`: GameThread stops at the quit (`NtUserDestroyWindow`), 4 threads keep ticking.
- `waitexit` reproduces a SIGUSR1 that is never handled: a worker answers 24 suspensions, the
  25th is lost (`--winedebug +server`: `*sent signal* signal=30`, no context back).
- A per-thread dump showed the stuck thread with a deferred signal never restored (pending word 2).

## Cause (confirmed)

See [fixes/C03-fix.md](fixes/C03-fix.md).

## Related

The 16:58 quit hang on `-114` with the fast path off looks different (no thread kills, no lock
timeouts): [C05](C05-quit-hang-silent.md).
