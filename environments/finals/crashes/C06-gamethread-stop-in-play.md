# C06: GameThread stops at 0 % CPU during play

| | |
|---|---|
| First seen | 2026-11-13 18:12, `macfinals-106`, `STEAMDRIVER_PERF=1` |
| Origin | ours (winemac event-queue lock) |
| Status | open — mechanism captured live 2026-11-15 (`macfinals-131`); bounded, holder-named lock installed as `macfinals-133`; next occurrence is survivable and self-describing |
| Reproducer | play and alt-tab around minute one (both captures followed an app switch) |
| Fix | mitigation in the runtime; root cause (which holder stalls/dies) to be read off the new ERR lines |

## What you see

The game freezes during play. The 2026-11-13 capture was quit manually; the 2026-11-15 one ran
into Unreal's hang watchdog after 60 s, which crashed the game and opened the UE crash reporter
(`CrashType Hang`, "Hang detected on GameThread").

## Evidence

### Live capture on `macfinals-131` (2026-11-15 23:03, `perf/20261115-230334/wine.65673`)

Rasmus alt-tabbed around minute one of play; the freeze followed immediately.

- **23:04:33.6** — GameThread (`02a4`) enters `NtUserPeekMessage` and never returns (the syscall
  log shows the entry; every later window shows it absent from the run list). `fd cache section:
  free` in the one stuck dump — **not** [C17](C17-slow-leave-and-quit-messagecall-ebadf.md)'s
  mutex. No `wait timed out` lines at all: no Wine-side critical section was stuck.
- **23:04:37.4** — `066c` enters `NtUserMessageCall` (a cross-thread `WM_GETTEXT` to `02a4`'s
  window `0x4005c`), waits for a reply that never comes (`no reply after 9997 ms`). Victim, not
  cause: the sender waits for the frozen GameThread to pump.
- Every other thread — RenderThread, RHIThread, task pools, audio — parked in normal waits
  (waiting for GameThread to feed them). One runnable thread (audio) until the end.
- **23:05:34–36** — UE's hang watchdog (60 s without a GameThread heartbeat) crashes the
  process: `UEMinidump.dmp` + `CrashContext.runtime-xml` in
  `<prefix>/…/users/crossover/AppData/Local/Discovery/Saved/Crashes/UECC-Windows-6B3D…_0000/`,
  then CrashReportClient spawns. No macOS crash report — UE caught it itself.
- **Crash stacks:** the crashed thread (`ThreadID 676` = `02a4`, `GameThread`, `IsCrashed`) is
  `win32u+0x152c4` ← `Discovery-d+0x6f28fa` (the message pump) ← … ← kernel32 thread start.
  Resolved against our shipped win32u.dll, `0x152c4` is **`NtUserPeekMessage+0x14`** — the
  return address of its raw `syscall` stub. Under Rosetta that is how a thread blocked in
  unix-side code looks from Windows: the deepest x86 frame is the syscall stub, and the real
  block is below, in the unix implementation. (The 17-NtClose/fd-cache picture of C17 is absent.)
- No GPTK/D3DMetal frame in any thread; the `AGX: exceeded compiled variants footprint limit`
  line is routine shader-cache pressure (44× in that log, 37× in the previous one, present in
  healthy sessions) — not the trigger.
- 2026-11-13 (`macfinals-106`): GameThread stopped making NT calls at 0 % CPU during play —
  same shape, no watchdog back then, quit manually. The crash dumps' `/Threads` list from
  2026-11-15 matches this exactly.

### Source walk (where under `NtUserPeekMessage` a thread can block forever)

`NtUserPeekMessage` (win32u/message.c) → `check_for_driver_events()` → `macdrv_ProcessEvents`
→ `getEventMatchingMask:` (winemac.drv/cocoa_event.m). On that path the signaling pipe is
`O_NONBLOCK` and the server round-trips are healthy; the only unbounded wait is
**`[eventsLock lock]`** — the `NSLock` guarding the WineEventQueue event array, taken by:

- every message-pump thread draining driver events (the GameThread, several times per frame),
- the Cocoa main thread's `postEventObject` (posting/merging mac events — busiest exactly
  during the deactivation event flood an alt-tab produces),
- the query machinery and `resetMouseEventPositions`/`discardEvents*`.

`NSLock` is not owner-death-safe, and this game does lose threads mid-play (the C17 capture's
exited-marking listed 170 `exited` lines) — a thread dying inside the critical section, or the
main thread stalling in it, wedges every later drain **forever**.

### Second live capture on `macfinals-133` (2026-11-21 04:46, `perf/20261121-044608/wine.18158`)

The freeze/crash reproduced at ~11 minutes in — the queue-lock mitigation did not stop it (and
correctly stayed silent: zero `event queue lock` ERR lines, no false positives). Rasmus had
been AFK for ~3 minutes and found the crash reporter on return — whether the app was
deactivated at the wedge is unknown, which is exactly what the `-134` logging answers. The
dumps caught the holder chain:

- **Background Worker #1 (`0x32c`) wedged in `NtUserDestroyWindow`** (`win32u+0x12a04`, the
  stub's syscall site = blocked in unix code under it). Window destruction takes win32u's
  process-wide **user lock** and calls into the mac driver's window teardown — a synchronous
  rendezvous with the Cocoa main thread.
- **GameThread (`0x15c`, ThreadID 348, the crashed thread) in `NtUserPeekMessage`** since
  04:56:14 (17.9 s at the last dump, never returned; caller frame `Discovery-d+0x2eec3d`, a
  nested pump inside a game handler rather than the outer one) and
  **WindowsRawInputThread (`0x398`) in `NtUserGetMessage`** — both queued behind the user lock
  the destroy holds. UE's watchdog fired 60 s later (04:57:14): `Hang detected on GameThread`.
- **First live catch of a slow op inside the fd-cache section**: `0164` held the section inside
  one `NtClose` from 04:55:32 to 04:56:14 (~42 s, then it completed —
  `fd cache section: held by tid 0164` with a wrapped `18446744073.7 s` duration at acquisition
  race, and `last NtClose returned 17.9 s ago` in the next dump). C17's
  slow-operation-inside-the-section is real, finite (tens of seconds), and self-clearing.
- Zero `no reply`, `wait timed out`, `reply read failed` lines in the whole session — the final
  block is silent; the only unbounded wait on the path is the Wine thread's synchronous wait
  for the Cocoa main thread (`OnMainThread`) under the user lock.

The 2026-11-15 capture is the same shape one lock deeper out of view (then: GameThread peek +
the `WM_GETTEXT` sender in `NtUserMessageCall`, both user-lock waiters). The queue lock was a
wrong-but-adjacent suspect: the rendezvous that wedges is the **mac driver's synchronous
main-thread wait** (`OnMainThread`) reached from user-lock-holding win32u calls (window
destroy, message-call processing), when the Cocoa main thread is unavailable for tens of
seconds. Note the other direction (corrected 2026-11-22): `query:` is the **Cocoa main thread
waiting on a window's Wine thread** (0.1–3 s timeouts, 3 min for drag-and-drop). That can close
a cycle — main thread waits on the GameThread, the GameThread waits on the user lock, the user
lock's holder waits on the main thread — broken only by the query timeout, over and over.
`macfinals-137`'s dump shows both directions at once.

## What we think (mechanism, 2026-11-17, updated 2026-11-21)

At the alt-tab, the GameThread's message-pump drain collided with the WineEventQueue lock: it
waited on `eventsLock` forever (holder stuck or dead — the capture cannot distinguish), freezing
the pump; everything else queued behind the GameThread; 60 s later UE's hang watchdog converted
the freeze into a crash report. The `WM_GETTEXT` no-reply and the one-runnable-thread picture
are all downstream of the single wedged call.

Shares the "one thread blocked below Wine while holding nothing" family with
[C05](C05-quit-hang-silent.md) (at quit) and [C16](C16-load-hang-loader-winmm.md) (at load) —
if their holders also turn out to sit in mac-driver waits, this mechanism may cover them.
[C04](C04-load-spin-gamethread.md) is the *spin* variant (PeekMessage returning 2 M times a
second, a lost wakeup instead of a block) and is **not** addressed by this fix.

## Mitigation (`macfinals-133`, 2026-11-17; corrected in `macfinals-137`, 2026-11-22)

`patches/wine-macdrv-event-queue-lock.patch`, as corrected by
`patches/wine-macdrv-diag-review-fixes.patch`:

- **Correction:** `-133` swapped `eventsLock` (NSLock) for `os_unfair_lock` on the claim that
  the kernel releases it when the owner dies. **That was wrong** — tested 2026-11-22: after
  the owner dies, `trylock` stays busy forever and a blocking lock **aborts the process**
  (exit 132). So `-133`–`-135` turned "dead holder hangs the queue" into "dead holder crashes
  the game" on the Cocoa main thread's posting path. `-137` restores NSLock (same test: a dead
  holder hangs, `tryLock`/`lockBeforeDate:` time out cleanly). macOS has no robust mutexes, so
  no lock here is owner-death-safe; the dead-holder ERR is gone (it could never fire).
- Every hold records its holder — a Wine tid, the Cocoa main thread, or an unlogged thread
  (`-133` recorded 0 for the last two, so a long main-thread hold was never reported) — and
  its acquisition time. A hold > 5 s is named in an ERR when it ends.
- `getEventMatchingMask:` acquires the lock with a **100 ms bound**: on timeout it names the
  holder in an ERR (at most one a second, with a skip count) and skips that drain round (one
  round of driver-event latency instead of a frozen game). The signaling pipe is cleared only
  while the lock is held, so a skipped round leaves the signal in place.

## Next steps

1. **Instrumentation: `macfinals-137` (2026-11-22).** `-135` added it
   (`wine-ntdll-diag-notes-fdcache-wrapfix.patch`, `wine-macdrv-focus-rendezvous-log.patch`);
   a review the next day found it could not show the case it was built for, and
   `wine-ntdll-diag-waits-registry.patch` + `wine-macdrv-diag-review-fixes.patch` fix that:
   - `-135` kept one shared slot per wait kind, so any other thread's short `OnMainThread`
     erased a stuck waiter's record within milliseconds. Now every outstanding wait has its own
     registry entry with a live duration (stress-tested: a stuck entry survived 50/50 dumps
     under 8 churning threads). Every stuck dump prints:
     - `ui state:` lines, each with how long it has held: app active/inactive (read from
       AppKit, not the callback), the key window's hwnd, the game window visible/occluded,
       and displays asleep / screen saver / screen locked (the AFK case).
     - `ui wait:` lines for every live rendezvous, both directions: `tid X waiting for the
       Cocoa main thread (OnMainThread) for N s`, `Cocoa main thread executing a request …;
       tid X`, `Cocoa main thread waiting on a window's thread (query RESIZE_SIZE); tid X;
       hwnd …`.
   - Off by default (Rasmus): `WINEDEBUG=err+fixme,+macfinals_ui` traces every state change
     and every main-thread wait over 20 ms (its own channel — `+macdrv`, which `-135` used,
     also turns on every macdrv trace). Without it, only waits > 10 s ERR.
   - **Next occurrence: read the `ui state` and `ui wait` lines** — they say whether anyone was
     there (focus, visibility, display sleep) and name both sides of the rendezvous.
2. Bounding or deferring the destroy's driver call (and any other user-lock-holding call into
   the mac driver) comes after that names what stalls the Cocoa main thread.

**Third occurrence of the watchdog crash (2026-11-25 04:09, `macfinals-165`, no counters):
leaving the lobby, game frontmost and focused** (`Platform.AppHasFocus=true` — a third trigger
besides alt-tab and AFK). GameThread blocked in `NtUserPeekMessage` from the same nested-pump
site as the 2026-11-21 capture (`Discovery-d+0x2eec3d`); all other threads parked; **no
user-lock holder visible in the Windows threads** — if this is C06, the stall sits on the Cocoa
main thread (invisible to the game's dump; exactly what `-137`'s `ui wait` lines name) or in
the event-queue path of the 2026-11-15 capture. One core was still busy 56 s into the freeze,
so the C04 spin is not ruled out; full details and the game's crash-context fields in
[C04](C04-load-spin-gamethread.md)'s 2026-11-25 entry. Reproducer to try: play, then leave the
lobby, with `STEAMDRIVER_PERF=1 WINEDEBUG=err+fixme,+macfinals_ui %command%`.

### Fourth occurrence, fully instrumented (2026-11-25 05:53, `macfinals-165`, trigger drill)

Caught ~12 min into the trigger drill (no CPU stress; counters, driver stats and
`+macfinals_ui` on), game frontmost and focused. Perf session `perf/20261125-054058/wine.35752`,
crash `UECC-Windows-B92B7E8B4DE55760347875B11C2C18B5_0000`, Time Profiler
`cpu-profile/freeze-20261125-055326.trace`, freeze_watch fired 6 s in.

- **Block, not spin (settles the C04/C06 question for this shape).** GameThread ran at
  85–88 ms CPU per 100 ms until 05:53:19.3, then dropped out of the counters completely: no
  `PeekMessage` loop, process NT calls fell from ~30,000 to ~2,000 per 100 ms (audio and
  network threads only). The 70 s trace has 14.6 s of CPU in all (0.2 cores), GameThread never
  on-CPU.
- **The 200 ms before the stop:** the process registered a window class, created a menu and a
  window (`NtUserRegisterClassExWOW`, `NtUserCreateMenu`, `NtUserCreateWindowEx`), then
  `NtUserDestroyWindow` + `NtUserQueryWindow`; 34 Cocoa-main-thread waits completed in that
  second (max 12.7 ms) — the create/destroy's own rendezvous, all finished.
- **Crash dump:** GameThread (332, `0x14c`) in `NtUserPeekMessage+0x14` from the nested pump
  `Discovery-d+0x2eec3d` (same as 11-21 and 04:09). **Background Worker #0 (`0x31c`) in
  `NtUserDestroyWindow+0x14`** — the 11-21 holder shape again. WindowsRawInputThread in its
  normal `NtUserGetMessage`.
- **Stuck dump at 33.8 s** (fired for an unrelated long wait, so it caught the freeze mid-way):
  `014c in NtUserPeekMessage for 33.8 s`; `031c in NtCallbackReturn for 33.8 s` (that call
  never returns to its caller — it resumes the outer `NtUserDestroyWindow`, so the worker came
  back from a window-proc/hook callback into the destroy's Unix side and stopped there);
  `0214 in NtUserMessageCall … WM_GETTEXT to 0x20096 for 29.6 s` (downstream: it waits for the
  GameThread; its 10 s `no reply` ERR is in the Wine log); and **`0154 not in an NT call; last
  NtClose returned 33.8 s ago`** — a thread that in every earlier dump of the session sits
  outside NT calls closing handles constantly ("0.0 s ago"), stopped at the same instant. The
  process is idle, so 0154 is blocked too, outside any NT call (a Unix call or a lock in Unix
  code). No `ui wait` lines: **no Cocoa-main-thread rendezvous was outstanding.**
- **The Cocoa main thread was healthy throughout:** the driver's main-thread lag probe ran
  100×/s for the whole freeze with max lag 0.07–0.2 ms. The mechanism written up above (the
  user-lock holder waits on a stalled main thread) **does not fit this occurrence.** Note:
  `OnMainThread`'s >10 s ERR and 20 ms trace are logged only when the wait *ends*, so an
  unfinished wait never logs — the registry/`ui wait` lines and the lag probe are what rule it
  out here.
- `ui state` at the dump: app active (frontmost), key window `0x20096`, visible, displays awake
  — nobody was away.

**Working hypothesis (unproven):** three threads stopped at the same instant inside Unix code —
GameThread in the pump, Worker #0 in the destroy (after its callback), 0154 outside NT calls —
with the main thread idle. That is the shape of a lock-order deadlock among Unix-side locks
rather than a rendezvous. Candidates on that path: win32u's user lock, macdrv's
`win_data_mutex` (recursive; `macdrv_DestroyWindow` holds it across `destroy_cocoa_window` and
the release of D3DMetal's client surfaces, CW HACK 22435), and whatever D3DMetal holds while
calling the macdrv hooks (`get_win_data` etc., which take `win_data_mutex`; CodeWeavers'
comment in `d3dmetal.c` already notes ordering there "to avoid deadlocks"). The GameThread's
pump reaches macdrv event handlers (incl. `CLIENT_SURFACE_PRESENTED`) through `ProcessEvents`.

**Next:** name the lock holders at the moment of the freeze — record the holder (Wine tid + since)
of the user lock and `win_data_mutex` the way the fd-cache section already does, print them in
the stuck dump, and fire the dump when one thread has been in a single NT call (or out of NT
calls while it normally isn't) for ~10 s instead of waiting for the 55-s cadence. The drill
reproduces this in ~12 minutes, so one more run should decide it.

**Diagnostics in place (2026-11-25, `macfinals-168`+, `wine-diag-lock-holders.patch`):** the
stuck dump's `lock:` lines name the user-lock and `win_data_mutex` holders with call sites, and
contended waits name both ends; the dump fires 10 s into the freeze. Passive capture during
normal play; procedure in `tests/finals_bisect/README.md` ("Passive capture").

## Root cause found (2026-11-25 09:01, `macfinals-171`): lock-order deadlock in winemac

Fifth occurrence, ~1 minute into a session (run B of the thread-band A/B, practice range).
Perf `perf/20261125-085953/wine.74452`; the 10 s dump (new in `-168`) fired:

```
stuck-dump: thread 014c in NtUserPeekMessage for over 10 s
  lock: user lock free
  lock: macdrv win_data lock held by tid 031c for 10.1 s; taken at winemac.so+0x4d88c
  ui wait: tid 014c waiting for the macdrv win_data lock; tid 031c for 10.1 s
  SIGABRT: none
  014c in NtUserPeekMessage for 10.1 s
  031c in NtCallbackReturn for 10.1 s      (back inside NtUserDestroyWindow)
  0154 not in an NT call; last NtClose returned 10.0 s ago
```

`winemac.so+0x4d88c` is the return from `get_win_data` at the top of `macdrv_DestroyWindow`
(disassembly of the build tree's winemac.so; bytes identical to the installed one).

| Thread | Holds | Waits for |
|---|---|---|
| GameThread `014c`: pump → `CLIENT_SURFACE_PRESENTED` → `client_surface_present` (win32u) → `macdrv_client_surface_present` | win32u `surfaces_lock` | `win_data_mutex` (`get_win_data`) |
| Worker `031c`: `NtUserDestroyWindow` → `macdrv_DestroyWindow` → `CFRelease(d3dmetal_client_surfaces)` (CW HACK 22435) → `client_surface_release` | `win_data_mutex` | win32u `surfaces_lock` (last reference) |

Every other path takes `surfaces_lock` first (present, update, the surface's detach, D3DMetal's
`get_win_data` hook, whose CodeWeavers comment already notes the ordering "to avoid
deadlocks"); only `macdrv_DestroyWindow` releases surfaces under `win_data_mutex`. It fires when
a worker destroys a window with D3DMetal surfaces (a temporary window the game creates and
destroys at transitions — seen in the 05:53 and 09:01 captures) while the GameThread handles a
present event. The earlier "main-thread rendezvous" mechanism was wrong; the 11-21 capture's
`NtUserDestroyWindow` holder is this same worker shape.

**Fix:** `patches/wine-macdrv-destroy-surfaces-lock-order.patch` (`macfinals-174`):
`macdrv_DestroyWindow` takes the surface array out of the window data and releases it after
`win_data_mutex` is dropped and the data is out of `win_datas`. In-game confirmation pending
(no recurrence over normal play; the diagnostics stay on).
