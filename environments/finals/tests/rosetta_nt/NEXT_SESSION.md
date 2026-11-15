# MacFinals: where we are (2026-11-15, end of session)

Start here after a context reset. Details live in the linked files.

- **Goals after this, in order** (performance, then keyboard and controllers):
  `environments/finals/NEXT_GOALS.md`.
- **Crashes and hangs:** `environments/finals/crashes/README.md` lists every known one (origin,
  status, reproducer), with a fix file per confirmed cause. Add new ones there.

## Next session: finish the crash and hang fixes in progress

State at the end of 2026-11-15: runtime **`macfinals-129`** is installed in the Steam tool
(backups `-127`, `-125`, `-119`, `-117`, …; `-127` was the previous installed one). Nothing
is committed. Reproducer suite (`teardown_check.py`, every mode) is at 0 failures on `-129`
(and `-127`, `-125`, `-121`, `-117`, `-119`). In game, quitting and
leaving a round are slow, and quitting sometimes never finishes: C05, C16 and the new
[C17](../../crashes/C17-slow-leave-and-quit-messagecall-ebadf.md).

Do in this order:

1. **Make the lock-owner dump trustworthy** — *done and verified 2026-11-15 on `macfinals-125`
   (installed):* `patches/wine-ntdll-syscall-log-stuck-scan.patch`,
   `patches/wine-win32u-sendmessage-stuck-log.patch`,
   `patches/wine-ntdll-syscall-log-stuck-name.patch` and
   `patches/wine-ntdll-syscall-log-stuck-once.patch` (+ its `…-tagfix` build fix; all appended
   to `WINE_PATCHES`) amend the `-119` stuck-dump:
   - The trigger is the log thread scanning for any thread whose *current* NT call has run
     ≥ 60 s — not the old "returned `STATUS_TIMEOUT` after ≥ 59 s", which fired on ordinary
     60 s timed waits (three false dumps in the 18:02 run, C17).
   - Each stuck *condition* (thread + call) dumps once; when it ends it is forgotten, so the
     same thread sticking again dumps again. A thread parked in `NtTerminateThread` never
     triggers (exited threads print as one `0274 exited` line in the body; a wtid taken over
     by a live thread is not printed twice). The headline prefers a call that is not a plain
     `NtWaitFor*`/`NtDelay*`, so a stuck `NtUserMessageCall` out-ranks idle pools and sleeps.
   - Names come from `syscall_log_name`'s **return value** (its buffer only fills for unnamed
     calls; the first build printed the untouched buffer — `in ?`, garbage bytes).
   - `wait_message_reply` takes the send info: after 5 s one
     `ERR "no reply after %u ms: still waiting for tid %04x (window %p, msg %x (%s))"`, then
     INFINITE as before.
   - Verified under `-125` (`/tmp/probe125`): stuck-send probe shows the ERR line at 10 s,
     dump 1 headlined the owner's wait at 60 s, dump 2 (exactly one 55 s window later)
     headlined **the sender** `0024 in NtUserMessageCall args=10078,d,40,…` with the
     never-pumping owner beside it and a mid-run-exited poller as `0140 exited`; cs-timeout
     probe headlined the INFINITE-waiting owner, quitters rendered `exited`; durations sane
     (the 18446744073 s bug is gone); services dumped once per condition, no repeats.
   - `teardown_check.py` all five modes green on `-125` (0 crashes, 0 abnormal exits, fast
     path 54–58 ns).
   - **First game run on `-125` (2026-11-15 16:25, 192 s, `game exited (0)`, zero
     `no reply` lines):** the freeze did not happen (it is intermittent — one clean session
     is a data point, not a fix; nothing in these patches changes hang behaviour). The
     diagnostics worked in game: benign conditions dumped once each (idle
     `NtUserGetMessage`/pool waits), `exited` and `not in an NT call` lines correct.
   - **Bug found in the game run, fixed by
     `patches/wine-ntdll-syscall-log-stuck-race.patch` (`-126`/`-127`):** a thread entering
     a call between the scan's clock read and its slot read wrapped the duration to ~2^64 —
     a *false* headline (`03cc in NtSetEvent`, a call the game does ~1,700/s) and
     `18446744073.7 s` body lines. `cur_start > now` now means "just started": skip.
   - Probes: `tests/windows_probe/cs_timeout_probe.c` (quitters + CS owner) and
     `tests/windows_probe/stuck_send_probe.c` (ready-event handshake, 140 s watchdog).
2. **Tell dead threads from hung ones — done and shipped 2026-11-15 as `macfinals-128`/`-129`
   (installed):** `patches/wine-ntdll-syscall-log-exited-mark.patch` marks a thread's slot in
   `pthread_exit_wrapper` (thread.c), the one funnel every death passes through (normal exit,
   `NtTerminateThread`, a SIGQUIT kill, a fatal `wine client error` abort), so a thread that
   dies inside a call prints `exited; never returned from <call>` instead of a duration that
   grows forever, and never triggers a dump. Plus an ERR with all four fd numbers when a
   reply read fails fatally (`reply read failed: fds reply %d alert %d wait %d,%d request
   %d`). Why: re-reading the `-119` run against the Wine source showed the old dump could not
   distinguish these — C17's `043c` read as "in `NtSetInformationThread` for 114 s" but had
   exited there (the error hit at ~t+95, *mid-play*, 40 s before the quit); and the wineserver
   provably replies `STATUS_ACCESS_DENIED` to pending sends on any thread death
   (`kill_thread` → `cleanup_thread` → `destroy_thread_windows`/`free_msg_queue` →
   `cleanup_results`), so C17's stuck `WM_GETTEXT` owner is alive and never pumping. Verified
   under `-129` (`/tmp/probe129`): the stuck-send probe's sleeper, killed mid-wait at 65 s
   (the `043c` shape), prints `0144 exited; never returned from NtWaitForSingleObject` in the
   115 s dump and is never a trigger or headline; the normally-returned poller prints plain
   `exited`. `teardown_check.py` all five modes green on `-129` (0 crashes, 0 abnormal exits,
   54–58 ns). **Crab Champions canary skipped this round:** Rasmus's real Steam session was
   running and the canary route launches a second (wine) Steam under the same account, which
   would sign it out; this change touches no graphics or syscall-path code. Run it when
   Steam is free if wanted (`RUNTIME=… APP=774801 tests/finals_bisect/launch_main.sh avx
   msync`).
3. **`tests/finals_bisect/ab_launches.py` fixes — written 2026-11-15, quick check next Steam
   session:** the launch closes at the next `=== run` (helper) block, so the helper's
   `hook=` line (`nt_off=-`) no longer overwrites the game launch's `nt_off`; launches without
   a `game exited` line end at the last counter stamp of their own perf folder; the last
   launch gets `wineserver crashed?` when the Wine log ends with wineserver's death line;
   inner dump loop variable renamed.
4. **C17: the stuck `SendMessage` and the lost wineserver connection.**
   - Thread `0274` sent `WM_GETTEXT` (msg `0xd`) to window `0x20096` and waited 69 s+ after the
     quit. Earlier in the same run that call took 2.1 s, 0.3 s and 1.1 s, so the window's owner
     answers slowly even in play. Find the owner (the dump from step 1) and why it stops pumping
     messages.
   - Thread `043c`: `wine client error:43c: read: Bad file descriptor`, then 114 s+ in
     `NtSetInformationThread(0x698, 3, …)`. Its wineserver socket was closed under it. Find who
     closes that fd (a double `close` in Wine's unix code, or fd reuse), and why the thread didn't
     die (Wine's `abort_thread` after a protocol error).
5. **A/B in game, with the fixed dump:** fast path on (`STEAMDRIVER_PERF=1 %command%`) against
   `MACFINALS_NT_OFF=syscall,reloc STEAMDRIVER_PERF=1 %command%`. Quit from inside a match, and
   leave a round, each run. Wait at least 70 s at any freeze. Then
   `python3 tests/finals_bisect/ab_launches.py --since "<date> <time>"`. Optionally the same on
   `macfinals-112` to see whether the quit hangs are older than this week's work.
6. **Open entries to update from that data:** C04 (GameThread spin after loading in), C05 (quit
   hang), C16 (load hang with loader and winmm locks), C17. **C07** (nested-exception noise): the
   quit-handler fix is in `-119`; confirm at game quits and write `crashes/fixes/C07-fix.md`. A
   second variant with a garbage (non-NULL) frame address was seen once (C05) and isn't covered.

## Newest (2026-11-14): the start-up Rosetta crash reproduced without the game

- **Cause: a signal that lands while a thread is in the hook's syscall stub** (`ntfix_macfast`,
  native ARM code on our own page). Rosetta finds no x86 location for that pc and aborts
  (`rosetta error: no code fragment associated with the given arm pc`).
- In the game it comes when a process is torn down at start-up: wineserver sends SIGQUIT to all of
  its threads (the "nested exception ... 0xa8" lines). The H.264/AAC decoders load at that moment
  in every launch, but they are not the cause.
- Reproducer: `tests/finals_bisect/teardown_check.py` (`tests/windows_probe/teardown_probe.c`),
  no Steam and no game, about a minute. On `macfinals-112`:

  | Run | Rounds that crashed |
  |---|---|
  | Hook, exit while 16 threads are in syscalls | 20 / 20 |
  | Same, `MACFINALS_NT_OFF=syscall` (stub still entered, bails at once) | 0 / 20 (1 Rosetta error) |
  | No hook | 0 / 20 |
  | Hook, suspensions only (`--mode suspendonly`, SIGUSR1), clean exit | 10 / 10 |

- So suspension hits it too: crash reporters, the anti-cheat and Wine itself suspend threads, so
  this can also crash the game later on, not only at start-up.
- **Fixed in `macfinals-114`** (installed; `-113` and `-112` kept beside it): the payload code
  runs inside libRosettaRuntime, where Rosetta defers signals; the fast path resumes through
  Rosetta's own `runtime_exitljmp`, which delivers them; and syscall sites in the macOS shared
  cache are no longer diverted. `-113` had all but the last step, and THE FINALS hung on quit once
  (a SIGUSR1 lost for good); `teardown_check.py --mode waitexit` reproduces that and passes on
  `-114`. Every reproducer mode is 0 failures, and the fast path costs 61 ns per call. Details:
  `R25_SIGNAL_IN_STUB.md`. Not yet run in game.
- Separate gap, not the crash: no H.264/AAC decoding. Moved to `../../NEXT_GOALS.md` ("Later").

## Newest (2026-11-13): microstutter, and Game Mode for every Wine program

- **Runtime `macfinals-106`** is installed in the Steam tool by hand.
- **The micro-jumps of moving targets** come from the macOS compositor sometimes showing a
  finished frame one refresh late. The game, D3DMetal and the GPU deliver on time. DXMT has the
  same holds. Measured in KovaaK's (no anti-cheat), frame cap 130.
- **Game Mode is now on for every Wine program** (`WINE_GAME_MODE=*`, `Game.app`): frames reach
  the screen 1.4 ms sooner, held frames 1.9 % → 1.5 %. On by default; `STEAMDRIVER_GAME_MODE=0`
  turns it off. **Not yet tried with THE FINALS' anti-cheat.**
- Fixed: Recall's game-app start crashed every launch on this runtime (missing 32-bit loader).
- Full account, tools and open items: `R24_MICROSTUTTER_GAME_MODE.md`.

## Newest (2026-11-12): thread stacks, the popup, the Rosetta start-up crash

- **Runtime `macfinals-92`** is installed in the Steam tool by hand. The rope fix from `-65` is
  confirmed in game.
- **Thread stacks:** Wine tries the last stack's place first. In an A/B, the VM lock is held half
  as long and about 20k failed mappings a second are gone, but FPS and hitches don't change.
  `MACFINALS_STACK_HINT=0` turns it off.
- **The minimum-spec popup** is shown on the first launch in a prefix, then clicked OK
  automatically (`WINE_MSGBOX_ACCEPT`, marker file, `STEAMDRIVER_GPU_WARNING=1` always shows it).
- **Open: Rosetta crashes the game during start-up** (`rosetta error: no code fragment associated
  with the given arm pc`). It shows up as Denuvo's "taking longer to start than expected". It
  hits about 1 launch in 9, and nearly half with `STEAMDRIVER_PERF=1`.
- **GameThread is the limit** (about 86 % of a core in play). Little left to gain on the Wine
  side.
- Full account: `R23_STACKS_POPUP_ROSETTA_CRASH.md`.

## Newest (2026-11-10): the missing rope, and new debug tools

- **Runtime `macfinals-65`** is installed in the Steam tool by hand. It adds Metal capture
  (`STEAMDRIVER_CAPTURE=1`), and the d3d12 layer now computes what each discard depends on with
  exact math. That restored the missing rope on Suspended Structures in a narrower test; the
  default-on rule still needs a run in the game. `SteamDriver-2.app` still bundles `-53`.
- Full account, ruled-out causes, tools and open items: `R22_MISSING_ROPE.md`.
- New: `STEAMDRIVER_PERF=1` from Steam and `tests/finals_bisect/perf_report.py`.

## Newest (2026-11-09, later): inside SteamDriver, and launched from the macOS Steam app

- **This is now the `finals` environment of SteamDriver** (repository
  `/Users/rasmus/Documents/Kod/Annat/MacVolvo`, branch `environments`; paths here are relative to
  `environments/finals/`). Its Wine has SteamDriver's newer Mac driver patches (Recall 1.6.2:
  precise/classic input, High-precision HID mouse). Workspace `~/SteamDriver-build/finals/phase-2`.
- **THE FINALS starts from the macOS Steam app** through our compatibility tool "SteamDriver: THE
  FINALS" (`compat-tool/`, folder `notproton-steamdriver-finals`), runtime `macfinals-53`. It
  uses NotProton's Steam bridge, which our Wine loads from source
  (`wine-steamclient-lsteamclient.patch`). It has no outside patching and needs no developer mode.
  Installed by `SteamDriver-2.app` (Game runtimes). Rasmus: "everything seems to work".
- **What broke on the way, and why:** `compat-tool/LESSONS.md` (Steam tool),
  `../LESSONS.md` (merge and builds), `tests/finals_bisect/PROFILING.md` (freeze tooling).
- The game files moved into macOS Steam's library (main profile); a symlink at the old place
  keeps `launch_main.sh` and older runtimes working.

## Runtime `macfinals-47` — storm fixed in game, debug output follows Wine's model

- **The syscall storm is fixed in THE FINALS** (r21f, runtime -43): 99.99 % fast-path hits,
  stock SIGSYS 30k/s → 1-2/s, Rosetta exception thread 0 %. NOTES.md "r21f in the game".
- **Freezes: investigation PAUSED, not done** (Rasmus, 2026-11-09: not needed right now).
  0.3-2.2 s GameThread freezes, mostly while loading and in the first minutes of play. Ruled
  out: shader compilation (D3DMetal's cache persists between runs and stopped growing; slow
  pipeline builds only at startup) and Wine syscall cost. Cause unknown. Resume with
  `tests/finals_bisect/freeze_profile.py` (Time Profiler, no developer mode; works on probes,
  never yet captured a game freeze). NOTES.md "Freeze hunt, continued and PAUSED".
- **`macfinals-47`** = 46 (assembled with the reworked layer) + the reworked hook. Hook
  payload unchanged since r21f. 44/45 are BROKEN (layer imported libwinpthread-1.dll and did
  not load — never use them; installed 45 was removed). 46/47 verified: d3d12 probe loads the
  layer, channel output correct.
- **Debug output now follows Wine's model** (CLAUDE.md "Debug output"): channels
  `macfinals_hook`, `macfinals_dx12`, `macfinals_scan`; default err+fixme; release builds
  with `--no-debug-msgs` / `DEBUG_CFLAGS`. The nt-ring file / MACFINALS_HOOK_RING /
  MACFINALS_SYSC_DUMP / MACFINALS_D3D12_LAYER_LOG are retired.

## Current build: macfinals-42 (TF guard; the 2026-11-08 game crash fixed at probe level)

- **Newest runtime `macfinals-42`** (= unhooked 38 + r21e-2 hook), installed in
  `~/Library/Application Support/MacFinals/runtimes/`. Tree hooklib and the
  embedded hook are sha256-identical. ais gate on the embedded hook: done,
  ~50 ns/call, counters healthy.
- **r21e-2 = r21d + fail-closed TF guard** in the fast-path stub: live x17
  bit4 or ctx+0x444 → stock, counted at ctl[40] byte 7. This answers THE
  FINALS' first-run crash (2026-11-08 19:20: STATUS_SINGLE_STEP at published
  target+4 — guest TF live at the dispatcher entry; anti-debug stepping on
  the new-account path). Stock sigsys is TF-correct; the guard declines
  TF-live sites instead of fast-pathing them. Verified: ais parity with r21d
  (entered 220043 identical), TF probe accounting (`entered 33 = hits 29 +
  g2 2 + TF 2` vs r21d taking the armed call into the fast path).
- **Rules added this session** (NOTES.md "r21e"): insertions into
  ntfix_macfast need a register-liveness audit at the insertion point (the
  x23 bug — first build destroyed the live `_g_ctl` pointer and Wine spun);
  the stub-change regression gate is **ais in Wine**, capsite is vacuous
  (carrier entries all bail at guard 2); a process that can die needs the
  poll thread (`MACFINALS_HOOK_RING=1` or `touch /tmp/rosetta-re/nt-ring`)
  or its counters never print — a missing line is not entered=0.
- **2026-11-09 07:40 game run on -42: in game, no crash, but the fast path
  did NOT engage in Discovery.exe** — 26k-56k stock SIGSYS/s (rax
  0xffffffff), Rosetta exception thread 20-34 % CPU, low FPS. No hook
  counters for the game process (no poll thread). NOTES.md "First game run
  on macfinals-42".
- **07:50 rerun with the nt-ring switch: the chains DO run in the game**
  (entered 11.2M, ~28k/s) but all bailed at guard 7: THE FINALS has a
  process instrumentation callback installed. **r21f** handles it like
  sigsys_handler (restore_flags |= 0x10000) instead of bailing; probe
  `instr_cb_probe.c` shows identical callback behaviour to stock at 75 ns vs
  16.5 µs. **Newest runtime: `macfinals-43`** (installed). NOTES.md "r21f".
- **Next: THE FINALS on -43** (`RUNTIME=macfinals-43 PERF=1
  tests/finals_bisect/launch_main.sh avx msync`, keep
  /tmp/rosetta-re/nt-ring): expect hits ≈ entered, perf sigsys/s ~0,
  Rosetta exception thread idle — then judge FPS.

## ⛔ Read this first: the r20 kernel panics (2026-11-07)

The syscall-fastpath work ("r20": patch Rosetta's finished 16K syscall thunks
in place) **panicked the kernel twice** — two ntstub gate runs, two forced
reboots (18:20:00, 18:21:16). The fatal call was `mach_vm_read_overwrite` on a
translated anonymous exec region (the scanner's READ; the write path never
executed). Kernel data abort far=0x14, panicked task = the hooked process.
**Everything r20 is quarantined in hooklib.c behind `MACFINALS_THUNK_PATCH=1`
(default OFF).** Treat `mach_vm_*` on translated mappings as panic-capable
(read included); reads must use direct user-space loads behind a SIGBUS/SIGSEGV
handler. Full evidence and rules: NOTES.md sections "⛔ THE CRASH" and
"r20 (2026-11-07)".

**Rasmus's decision, 2026-11-07: fixing the syscall storm is REQUIRED for
success** ("the project may fail, but it can't be a success without fixing
this").

## ✅ r21 (2026-11-08): the stream insert WORKS in the carrier — v6 geometry

The r19 revival succeeded. Ground truth first (capsite carrier + ring), then
the mechanism that killed every earlier insert (a Rosetta fixup pass patches
whatever word sits at the pre-insert cursor X with a 26-bit immediate,
top-6 bits preserved → any live word there becomes undefined → the
translator assert), then the fix: **never execute X** (`insert8` v6 — a
sacrificial `bl #0` at X absorbs the patch and becomes the real relocated
bail target; the live chain starts at X+4 behind a `b #+8` at X-4).

- capsite ×8: exit 0, all markers exact, stub entered 11/12, no asserts.
- capsite3 scan-0: 36/36 intact chain copies; X holds a valid relocated bl
  in every copy (targets page+0x200 ×2, +0x204 ×1).
- Full mechanism, evidence, the fixup-table side-discovery and the scanner
  caution: NOTES.md "r21 (2026-11-08)"; design doc R19_SYSCALL_FASTPATH.md
  (implementation section rewritten for v6).
- hooklib.dylib/payload in the tree carry v6; nothing is in a runtime yet.

**Next steps, in order:**
1. ~~In-Wine test~~ **DONE 2026-11-08 (r21b) — result: chains placed but NOT
   executed.** The insert fires at Wine's syscall sites and complete, correctly-
   addressed v6 chains exist at exit — but ALL of them sit in one fixed RWX
   staging region (`0x200014000`, same address every process) and `entered=0`.
   Uncorrupted stock copies sit in a DIFFERENT region family (~`0x20869xxxx`,
   prot 3 = RW-not-exec). No scanned exec region contains either. Where the
   executed TBs live is now THE open question. Full record: NOTES.md
   "r21b (2026-11-08, afternoon)" — including the crash ledger (zero crashes
   this session) and the suspicious-but-plausible published target
   (0xc723a96/0xc722ede ≈ PE-ntdll base + offset — verify with
   read-dispatch-page.exe before any engagement).
2. Next moves are in NOTES.md r21b "Next session's first moves": region-list
   dump to find executed TBs; fix the STOCK census words-print bug; verify the
   published target. If executed TBs bypass the append-hook path entirely, the
   insert must move — **surface to Rasmus before building a new mechanism.**
3. If engagement is achieved: timing (~16 µs → ~2 µs), runtime `macfinals-41`
   (add_nt_hook as always), Crab Champions canary, then THE FINALS
   (`PERF=1 … avx msync`) — compare against R17_PERFORMANCE.md numbers.

Still-true constraints: no `mach_vm_*` copy/protection call on translated
mappings (the r20 panic class — r21's insert path uses only the emitter's
own buffer writes, nothing in that class); region walks allowed but NOT
concurrent with heavy JIT churn (r21 scanner caution — the walker can wedge
uninterruptibly; that is a tool problem, not a payload one).

## 🎯 r21d (2026-11-08, night): THE FAST PATH WORKS IN WINE — 33 ns/call

The r21c blocker chain is fully decoded and fixed (NOTES.md "r21d"). Every
fix stayed inside the approved design (guard corrections + one missing
precondition; no mechanism changes):

- **PE-image sites execute their chains** (ais5/ais6) — the r21b
  "second emitter" applies to ANON mappings, not PE images. Hot loops
  engage every call; no tier-up.
- **Guard 5b fixed:** TPIDRRO is the pthread TEB (mac_gsbase_switching=0),
  [pthread+0x30] = the Windows TEB → two-hop resolution, fail-closed
  (canonical + page-aligned + self-pointer before trust).
- **Guard 7 fixed:** [TEB+0x330] is never null under Wine (it holds
  &instrumentation_callback); the dispatcher derefs TWICE — so does the
  stub now; bails only when a real callback is installed.
- **Resume precondition found:** table[0]'s install is gated on bit9 of the
  32-bit pending word at ctx+0x1c8 (0x5908 `swp`s it and returns the old
  value). The stub queues bit9 itself — 32-bit ONLY: ctx+0x1cc is a
  separate live payload field (64-bit access = false-busy reads / payload
  zeroing writes; ais8-ais10).
- **ais11/ais12 result:** `entered 220043 hits 220038 bail5 0`, rax
  0xC000001C every call, markers intact, **33 ns/call** (stock 16-19 µs,
  Proton ~2 µs). The syscall storm fix works.

**Next, in order:**
1. Runtime `macfinals-41` via `add_nt_hook.py` (the tree's hooklib now
   carries r21d; assemble as always, never skip the hook step).
2. Crab Champions canary (`APP=774801`) — runtime soundness after the
   hook change.
3. THE FINALS with `PERF=1 … avx msync` (R17 setup): expect the
   exception-server load to collapse; compare GameThread kernel time and
   FPS against R17_PERFORMANCE.md. Watch `sysc` counters for in-game
   engagement (the game's sites are PE-image — they should engage like
   site1 did).
4. The 5-guard-2 pre-publish window (first ~100 ms stock) is accepted
   (matches today's behaviour); revisit only if the game trips on it.

## 2026-11-07 current build (supersedes the 2026-11-02 facts below)

- **Runtime `macfinals-40` installed** in `~/Library/Application Support/MacFinals/runtimes/`
  (= 38 + nt-hook; 38 = 33 + Wine syscall-fastpath publish + SIGSYS diagnostics,
  `R19_SYSCALL_FASTPATH.md` for the Wine side). macfinals-40's hook contains NO
  r20 scanner code (verified) — the shipped runtime cannot trigger the panic.
- Probe iteration uses the local `tests/rosetta_nt/hooklib.dylib` via
  DYLD_INSERT on macfinals-40; no runtime rebuild needed for hook-only changes.
- The 2026-11-02 "Current build" section below (macfinals-33 etc.) is history;
  the "What works" table and d3d12-layer paragraph remain accurate.

## Current build

- **Runtime `macfinals-33`** (2026-11-02, = 31 + AVX signal-context fix, `R18_AVX_CONTEXT.md`;
  in-game check with `avx` pending). Before it: `macfinals-31` (installed in `~/Library/Application Support/MacFinals/runtimes/`).
  Workspace `~/MacFinals-build-gcc/phase-2`. Not yet built into an app bundle; test with
  `tests/finals_bisect/launch_main.sh`.
- Nothing from this session is committed (main is at `81b0293`). Commit only when Rasmus asks.

## What works

| | Since | Where |
|---|---|---|
| Past the startup check (E111000D): PE side built with **MinGW-w64 GCC**, never llvm-mingw | `macfinals-19` | `(removed)` |
| **Skins have arms and neck**: d3d12 layer, Proton-equivalent constant-buffer rewrite | `macfinals-27` (confirmed in game) | `R15_SKIN_CURVE.md` |
| **No Steam TOS assert / in-game minidumps**: Wine fix for macOS `IP_RECVTOS` | `macfinals-29` | `R16_FIRST_LAUNCH_HANG.md` |
| Max logging that the anti-cheat accepts (`DEBUG=max`) | `macfinals-31` | below |

### The d3d12 layer in one paragraph

`d3d12_layer/` is a Wine builtin `d3d12.dll` in front of GPTK's d3d12.dll, which ships
byte-identical as `lib/wine/d3d12_gptk.dll` (outside the arch dir, because Wine matches
builtins by the PE's internal export name; a Wine placeholder reserves the name in system32).
It wraps the device (generic 16-byte forwarding stubs for all 128 vtable slots, so
ID3D12Device11/12 work; the game uses Device12) and rewrites DXIL at pipeline creation:
a local float array filled from consecutive constant-buffer floats and read with a dynamic
index becomes a direct `cbufferLoadLegacy` read, 0 outside the buffer's declared size, which is
what vkd3d-proton does. Structural matching, no shader list. Logs through Wine's debug
output (`pid:tid:macfinals-d3d12:`). `MACFINALS_SKIN_FIX=0` turns the rewrite off. Built by
`scripts/assemble_portable_runtime.py` via `d3d12_layer/build.sh`.

## Rules learned this session (do not relearn)

- **Never log the game with `warn+heap`, `trace+heap`, `relay` or `snoop`.** Plain `warn+all`
  made Wine set debugger-style heap flags and the anti-cheat quit with "disallowed tool". Use
  `warn+all,warn-heap` (that is what `DEBUG=max` does). `+seh` also changed behaviour once.
- `launch_main.sh` clears the environment: only variables it lists are passed on
  (`MACFINALS_SKIN_FIX` is; debug output is chosen with `DEBUG=` or a verbatim `WINEDEBUG=`).
- `launch_main.sh` defaults are the old CrossOver-like bisect baseline: **no MSync, no AVX**
  unless you pass `msync` / `avx`. The app enables both. Measure performance with both.
- The layer's log only shows in the game if it goes through Wine's debug output; a process
  started by Steam has no Windows stderr.
- Probes that start Wine against the real prefix join a running session; make sure no
  MacFinals wineserver is running first.

## Testing

```sh
# quit the launcher, Steam and the game first
RUNTIME=macfinals-31 DEBUG=max tests/finals_bisect/launch_main.sh          # everything safe to log
RUNTIME=macfinals-31 PERF=1 tests/finals_bisect/launch_main.sh avx msync   # performance counters
```

Quit Steam normally at the end: the script then collects Steam logs, new dumps, macOS crash
reports, the D3DMetal cache listing and the macOS log into `logs/<session>/`. `PERF=1` writes
`logs/<session>/perf/wine.<pid>`; find the game's file by its Windows thread ids.

`APP=steam` starts Steam only, `APP=774801` Crab Champions. `tests/finals_bisect/kwine.sh` stops
every Wine process on the Mac, any install (`-n` lists them).

Probes in `tests/windows_probe/`: `d3d12_factory_probe.c` (layer: device paths, Device12,
stubs), `recvtos_probe.c` (TOS control message; Proton gives `IP_TOS 0x88`),
`suspend_probe.c` (suspend/context of new threads), `qpc_probe.c` (QPC cost, RDTSC),
`avx_state_probe.c` (YMM state through exceptions and thread contexts; needs AVX advertised).

## Open problems and next steps (2026-11-02, superseded)

The performance items that stood here (low FPS, the syscall cost, `HOOK=0`, the app bundle) are in
`../../NEXT_GOALS.md`. The crash items are in `../../crashes/`: main-menu buttons (C14), the
crash reporter hang (C11), the first-launch Denuvo dialog (`NEXT_GOALS.md`, "Later"). Still to
confirm in game: the AVX crash spawning into the tutorial (null read at 0x8; Wine didn't carry YMM
state in signal contexts on macOS), fixed in `macfinals-33` (`R18_AVX_CONTEXT.md`).

## Boundaries that applied this session

Game files never touched (crash reports and logs the game writes are read only); the
anti-cheat's protected code is not analysed: count its effects, do not ask why; GPTK ships
unchanged; Rosetta is not modified on disk.
