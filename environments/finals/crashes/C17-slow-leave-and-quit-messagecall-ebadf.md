# C17: leaving a round and quitting are slow, and the quit doesn't finish

| | |
|---|---|
| First seen | 2026-11-14 18:02, `macfinals-119`, `MACFINALS_NT_OFF=syscall,reloc STEAMDRIVER_PERF=1` |
| Origin | unknown; not the fast path or the relocation (both off) |
| Status | open (diagnostics ready: `macfinals-125` names a stuck send and dumps every thread; `macfinals-128`/`-129` mark exited threads and fd state) |
| Reproducer | probes for the diagnostics: `tests/windows_probe/stuck_send_probe.c`, `cs_timeout_probe.c` |
| Fix | – |

## What you see

Leaving the round takes long. Quitting takes long too, and the process never exits on its own:
the window is gone, `Discovery-d.exe` stays at about 20 % CPU. No Unreal crash report.

## Evidence

From `perf/20261114-180237` (`wine.70472`, game process) and `steamdriver-wine.log`. The game
started at 18:02:42.

- **0–135 s:** loading and play. GameThread at about 75 % of a core the whole time. Bursts of
  1,500–2,000 handled exceptions a second at 10–25 s, 65–75 s, 85–90 s and 120–130 s, likely loads and
  round changes. Rosetta's exception server is the busiest system thread (77 % system share).
- **About 135 s (18:04:57):** the quit. GameThread drops to 0 and the process idles (about 15,000 NT
  calls a second, mostly timer polling).
- **Thread `0274`** was in `NtUserMessageCall(hwnd 0x20096, WM_GETTEXT, 0x201, …)` for 69 s and
  counting in the last dump. The same call took 2.1 s, 0.3 s and 1.1 s earlier in the session. That
  is a cross-thread `SendMessage` whose target window's thread doesn't process messages.
- **Thread `043c`:** the last Wine log line from the game is
  `wine client error:43c: read: Bad file descriptor` (its reply pipe was closed under it, in this
  process). The first dump (t+88) still shows it healthy (`NtWaitForSingleObject` 0.2 s); it sat in
  `NtSetInformationThread(0x698, 3, …)` for 54 s, then 114 s — so the call (and the error) happened
  at ~t+95, **40 s before the quit**, mid-play. Whether the thread ended there or hung cannot be
  read from this dump (see "What we think"); the `macfinals-128` exited-marking settles it at the
  next occurrence.
- **About 18:06:34:** the counters stop and the Wine log ends with `wineserver crashed, please
  enable coredumps`. There is no macOS crash report for wineserver, so it was most likely killed
  (force quit), not crashed.
- No `wait timed out` lines, so no critical section was stuck. The three `stuck-dump` blocks in this
  run were triggered by ordinary 60 s timed waits, not lock timeouts. They list exited threads as
  `in NtTerminateThread` (see `tests/rosetta_nt/NEXT_SESSION.md`, "Next session", step 1).

## What we think

Re-read of the `macfinals-119` data plus the Wine source (2026-11-15), three parts:

1. **The quit hang is a never-answered `SendMessage`.** `0274` entered the `WM_GETTEXT` send
   at ~t+139 s, exactly at the quit, and `ExitProcess` hooks block on it, so
   `NtTerminateProcess` never runs and the threads spinning in user code on
   `NtQueryPerformanceCounter` keep the process at 20 % CPU. The window's owner cannot be
   dead: the wineserver replies `STATUS_ACCESS_DENIED` to every pending send when a thread
   dies (`kill_thread` → `cleanup_thread` → `destroy_thread_windows` + `free_msg_queue` →
   `cleanup_results`), and it visibly didn't. So the owner is alive and never pumps — one of
   the quit-time threads spinning in user code.
2. **`043c` is a separate, earlier anomaly.** The counters show it healthy at the first dump
   (t+88) and inside `NtSetInformationThread` from ~t+95 — *40 s before the quit*, mid-play.
   Its reply pipe was closed in-process at that moment (`read: Bad file descriptor` means a
   closed fd; a dead wineserver gives EOF/EPIPE). Who closed it is unknown; an fd-number
   reuse after a double close somewhere in the process is the standing candidate.
   `abort_thread` then ends the thread through `pthread_exit_wrapper`, which closes the
   server fds — but the old dump cannot tell that dead thread from one still hung in the
   call, so the two "114 s" sightings proved nothing. The slow answers during play (up to
   2 s) may be the same rarely-pumping owner as (1), not `043c`.

Same family as [C05](C05-quit-hang-silent.md) (quit hang, threads that don't end) and
[C16](C16-load-hang-loader-winmm.md).

## Next steps

1. Fix the dump — **done, verified and installed (`macfinals-125`, 2026-11-15)**:
   `wine-ntdll-syscall-log-stuck-scan/-name/-once.patch` and
   `wine-win32u-sendmessage-stuck-log.patch`. Probe-proven shape: after 5 s the ERR line
   names tid/window/msg; at 60 s a dump headlined the stuck sender
   (`0024 in NtUserMessageCall args=10078,d,40,…`) with the never-pumping owner beside it,
   exited threads as one `exited` line, each condition dumped once. First game run on
   `-125` (192 s): clean quit, no `no reply` line — the hang is intermittent, so keep
   counting sessions until one fires.
2. **Tell dead threads from hung ones — done, shipped as `macfinals-128`/`-129`
   (2026-11-15):** `wine-ntdll-syscall-log-exited-mark.patch` marks a thread's slot in
   `pthread_exit_wrapper`, the funnel every death passes through, so the dump says
   `exited; never returned from NtSetInformationThread` instead of a forever-growing
   duration, and an ERR names all four fd numbers when a reply read fails fatally
   (`reply read failed: fds reply %d alert %d wait %d,%d request %d`).
3. At the next slow quit, read the owner of the window `0274` sends to (the 5 s `no reply`
   ERR line names its tid), and what it's doing — with the exited-marking, every thread in
   the dump is now provably alive or dead.
4. Find who closes the reply pipe mid-play (`043c`, t+95). The new fd ERR line plus the
   dump's alive/dead reading decide whether this is a leak into the quit or harmless.
