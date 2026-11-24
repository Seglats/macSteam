# C17: leaving a round and quitting are slow, and the quit doesn't finish

| | |
|---|---|
| First seen | 2026-11-14 18:02, `macfinals-119`, `MACFINALS_NT_OFF=syscall,reloc STEAMDRIVER_PERF=1` |
| Origin | unknown trigger; the hang mechanism is identified (2026-11-15) |
| Status | open — mechanism found live on `macfinals-129`; holder-naming diagnostics + owner-death-safe section installed as `macfinals-131`; next live hang names the wedge |
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

### Live capture on `macfinals-129` (2026-11-15 22:11, `perf/20261115-221106/wine.53230`)

The hang fired on the first `-129` session, 3.5 minutes in, at the quit (22:14:36):

- The 5 s win32u ERR named the send: `no reply after 10001 ms: still waiting for tid 01ac
  (window 0x20096, msg d (WM_GETTEXT))`.
- The dumps show **about 30 threads entering their final NT call within one second** of 22:14:36 —
  17 stuck in `NtClose` (78–493 s, handles 444, 294, bdc, 570, 898, 8b0, bd4, be4, be0, 388, 298,
  …), `0520` in `NtReadFile(0x2d8)`, `05c8` in `NtUserCallNoParam(8)` (= `NtUserThreadDetach`)
  **holding `loader_section`**, `01ac` in `NtWaitForSingleObject(0x868, INFINITE)` (the never-pumping
  owner), `0278` in `NtUserMessageCall` (the sender, 3 s later), and six in
  `NtWaitForAlertByThreadId` (critical-section/SRW victims; the Wine log's timeout lines name
  `threadpool.cs`/`timerqueue.cs` blocked by `01e0`, itself in `NtClose`). Durations grow by exactly
  the elapsed time between dumps — permanent. 170 `exited` lines and **zero `never returned`**:
  no thread died mid-call, so the `macfinals-128` marking rules out a dead holder.
- The wineserver (sampled read-only at 22:19; sampling the game is off limits) is **healthy**: the
  main loop parked in `poll`, and the msync message-pump thread parked in its untimed *receive* on
  an empty queue — proven by resolving the sampled PC against the shipped binary:
  `_mach_message_pump+0xd9`, the return address of the `mach_msg2_trap` receive call
  (`MACH_RCV_MSG` on `receive_port`). An idle pump with an empty queue also proves no client's
  close-notification send is pending or blocked.
- Source walk (`dlls/ntdll/unix/server.c`, `sync.c`): `NtClose` holds the process-global
  `fd_cache_mutex` across `close_inproc_sync` (an msync mach send) **and** the `close_handle`
  server round-trip; `server_get_unix_fd` holds it across `wine_server_receive_fd`, a **blocking
  `recvmsg` on the shared `fd_socket` with no timeout**; `get_inproc_sync` holds it across the
  cache-fill round-trip. One thread stuck anywhere in there serializes every other NtClose,
  every uncached wait and every uncached fd read in the process.

## What we think (mechanism, 2026-11-15)

Re-read of the `macfinals-119` data, the live `-129` capture and the Wine source (2026-11-15):

1. **One thread wedges forever inside the `fd_cache_mutex` section, and the entire quit
   collapses behind that single mutex.** With both wineserver threads provably healthy, the
   only client-side operation in the section that can block indefinitely is
   `wine_server_receive_fd`'s untimed `recvmsg` on the shared `fd_socket` (the msync mach
   send is ruled out by the healthy, empty-queue pump). Prime suspect: `0520`, which entered
   `NtReadFile(0x2d8)` in the freeze second — a file handle whose fd the server was expected
   to send; if that fd message was lost or already consumed by a desynced earlier reader,
   the `recvmsg` never returns. Every other stuck call is then a victim: the 17 `NtClose`s
   queue on the mutex; `NtUserThreadDetach` (`05c8`) blocks in its own `NtClose`s while
   holding `loader_section` (the C05/C16 shape); `01e0` blocks in `NtClose` while holding
   `threadpool.cs`/`timerqueue.cs`; `01ac`'s wait on the uncached handle `0x868` parks
   inside the cache fill **before it ever pumps**, so the `WM_GETTEXT` send (`0278`) can
   never be answered, so `ExitProcess`'s hooks block and `NtTerminateProcess` never runs,
   leaving the QPC spinners at 20 % CPU. One mutex explains every observation, including
   why the owner looked "alive and never pumping" without any dead thread.
2. **The never-answered `SendMessage` is a symptom, not the cause.** The window owner is
   alive (the wineserver would reply `STATUS_ACCESS_DENIED` on its death) but frozen in the
   cache-fill behind the same mutex — this corrects the earlier reading that the owner was
   one of the quit-time user-code spinners.
3. **`043c` (-119) and the unknown trigger are the same family: something in the process
   corrupts Wine's own communications fds.** `043c`'s reply pipe was closed in-process
   mid-play (`read: Bad file descriptor`; a dead server gives EOF/EPIPE); a lost or
   mis-consumed fd message on `fd_socket` is exactly what wedges `receive_fd`. The live
   capture rules out a dead mutex-holder (zero `never returned` lines), so the trigger is a
   live thread ending up on the wrong side of an fd-channel desync — origin still unknown.

Same family as [C05](C05-quit-hang-silent.md) (quit hang, threads that don't end; its
`loader_section` holder is the ThreadDetach→NtClose victim here) and
[C16](C16-load-hang-loader-winmm.md) (loader lock held by a thread blocked below Wine).

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
3. **Name the mutex holder — done and installed (`macfinals-131`, 2026-11-15):**
   `patches/wine-ntdll-fdcache-holder-name.patch` records the `fd_cache_mutex` holder
   (wtid + acquisition time) on section enter/leave, and every stuck dump now prints it
   first — `fd cache section: held by tid %04x for %.1f s; every NtClose and fd-cache fill
   queues behind it` (or `free`) — so the next live hang names the wedge directly. It also
   makes the section owner-death-safe: `pthread_exit_wrapper` releases the mutex when the
   dying thread holds it (it runs in that thread, so the unlock is legal; without it a dead
   holder would lock every NtClose forever — no dead holder occurred in the live capture,
   but the class is now closed). Probe-verified on `-131`: prior dump behaviour intact
   (exited-marking, sender headline), new line present. No ERR variant: the wedge is
   permanent, so the 60 s dump is the right vehicle at the reading cadence.
4. **Fix candidate once the holder is confirmed:** `receive_fd` must not block forever
   under the mutex (bounded wait → fail the call and keep going), and the section should be
   owner-death-safe (`pthread_exit_wrapper` releases it if the dying thread holds it — it
   runs in the dying thread, so it can legally unlock).
5. Find the desync's origin (the `043c` family): who closes or mis-consumes Wine's
   communications fds mid-play.
