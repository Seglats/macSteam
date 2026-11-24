# C05: quitting hangs right after the window closes (no kills, no lock timeouts)

| | |
|---|---|
| First seen | 2026-11-14 16:58, `macfinals-114`, `MACFINALS_NT_OFF=syscall STEAMDRIVER_PERF=1` |
| Origin | unknown; suspect: relocated append/converter code (see "What we think") |
| Status | open |
| Reproducer | none yet |
| Fix | – |

## What you see

Quitting from inside the game: the window closes, the process stays. No Unreal crash report.

## Evidence

- `perf_report.py` (session `perf/20261114-165844`, game process `wine.45755`): normal play
  until 128 s. At 129 s `NtUserChangeDisplaySettings` and `NtUserDestroyWindow` (shutdown), the
  GameThread frees memory for about 2 s, then stops at 0 % CPU for good (130.8 s on). The process
  idles at about 700 NT calls a second until it was killed about 3 minutes later.
- No `nested exception` lines (no threads killed, so Wine's exit never reached
  `NtTerminateProcess`) and no `wait timed out` lines (nobody stuck on a critical section). That
  makes it a different hang from C03, earlier in shutdown.
- The fast path was off (`nt_off=syscall` in `steamdriver-run.log`). The relocated append code and
  converters were still active.

### Second occurrence: 2026-11-14 17:43, `macfinals-117`, `MACFINALS_NT_OFF=syscall`

- GameThread stops at about 100 s (quit), the process idles.
- After 60 s: `loader_section` and `peb_lock` both held by 04b8, the exiting thread.
  `RtlExitUserProcess` takes exactly these two right before `NtTerminateProcess(0)`, which should
  kill every other thread. Threads 0458, 02d4 and 02d0 are still alive and time out waiting on
  them, so SIGQUIT didn't reach them.
- 04b8's last log line: loading `AVRT.dll` during shutdown (audio, from a DLL's detach code), which
  then presumably waits on a thread that should already be dead.
- One nested exception on thread `ScreenSaverInhibitor` with a garbage frame address
  (`0xc0054902bf0549a2`), not NULL: the quit handler's thread-data lookup is sometimes wrong
  under Rosetta (C07's patch only covers NULL). Still noise from a thread being killed.

## What we think

Not the fast path. Threads that don't receive SIGQUIT point at postponed signals in our relocated
code, which runs even with the fast path off: the append hook (during translation) and the
converters (during signal delivery and return). Before the relocation, a signal landing there
crashed instead. Alternatively an older shutdown hang that shows up more now that quits happen from
inside matches. Compare C06 (GameThread at 0 % on `-106`, before today's hook changes).

## Next steps

1. Quit A/B with the hook as on `-112` minus the fast path:
   `MACFINALS_NT_OFF=syscall,reloc STEAMDRIVER_PERF=1 %command%`. If it still hangs, today's hook
   work is ruled out.
2. Then find what the GameThread blocks on at shutdown: its last NT calls in the counters, and the
   Wine log with a light channel set (never heap/relay/snoop).

Update 2026-11-15: C17 was captured live (`macfinals-129`) and the family mechanism is now
known — one thread wedges forever inside the `fd_cache_mutex` section (prime suspect: an
untimed `recvmsg` in `wine_server_receive_fd`), and the loader/`threadpool.cs` holders here are
threads blocked in `NtClose`/`NtUserThreadDetach` behind that mutex. See
[C17](C17-slow-leave-and-quit-messagecall-ebadf.md) "What we think".
