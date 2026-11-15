# C01: Rosetta crash during start-up ("no code fragment")

| | |
|---|---|
| First seen | 2026-11-12 (`macfinals-92`); 8 of 45 launches up to 2026-11-14, more with `STEAMDRIVER_PERF=1` |
| Origin | ours: the syscall fast path's native stub (since the fast path shipped in `macfinals-43`) |
| Status | fixed in `macfinals-113` |
| Reproducer | `python3 tests/finals_bisect/teardown_check.py --mode exit` (and `--mode suspendonly`) |
| Fix | [fixes/C01-fix.md](fixes/C01-fix.md) |

## What you see

The game dies during start-up, usually shown as Denuvo's "taking longer to start than expected".
The Wine log (`<compatdata>/steamdriver-wine.log`) has
`rosetta error: no code fragment associated with the given arm pc`.

## Evidence

- `startup_crashes.py`: always in the game process, at moments with many threads starting or
  stopping. 6 of 8 came right after `msmpeg2vdec.dll`/`MSAudDecMFT.dll` loaded. Those decoders
  load in every launch, at the moment the launcher process is torn down, so they weren't
  involved.
- Reproducer on `macfinals-112`: hook on, 20 / 20 rounds crashed; `MACFINALS_NT_OFF=syscall`,
  0 / 20; no hook, 0 / 20; suspensions only, 10 / 10 crashed.

## Cause (confirmed)

A signal (SIGQUIT at process exit, SIGUSR1 for a suspension) landing while a thread runs the
hook's own ARM code. Details and fix: [fixes/C01-fix.md](fixes/C01-fix.md).
