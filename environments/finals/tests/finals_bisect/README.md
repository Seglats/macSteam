# Crash hunt under CPU stress (2026-11-25)

Started 2026-11-25 (Rasmus): provoke the GameThread hang ([C04](../../crashes/C04-load-spin-gamethread.md)
/ [C06](../../crashes/C06-gamethread-stop-in-play.md), "Hang detected on GameThread") while the
session is fully instrumented, by starving the game of CPU. The 04:09 occurrence (the A run of
the thread-band A/B) had no counters, so spin vs block is undecided; everything here exists to
catch the next occurrence with the evidence that decides it.

## The kit

| Tool | What it does |
|---|---|
| `cpu_spin.c` | N burners in one process (SIGINT/SIGTERM stop all threads). `clang -O2 cpu_spin.c -o cpu_spin && ./cpu_spin 18` |
| `freeze_watch.py` | watches the driver's stats rows; on a real freeze attaches Time Profiler for 70 s |
| `event_flood.c` | posts stationary mouse-moves to load the Cocoa main thread — **parked**, see below |

`cpu_spin` burns at **normal priority on purpose**: it sits in the same scheduler band as the
game's threads, so it takes exactly the CPU time the game would otherwise get (this Mac has 18
cores; the game wants ~6.4, see [R26_CPU_PROFILE.md](../rosetta_nt/R26_CPU_PROFILE.md)).

`freeze_watch.py <stats-prefix> [--rows 20] [--seconds 70]` polls
`WINEMAC_INPUT_STATS`'s `<prefix>-<pid>.jsonl` (one row a second from the Mac driver). A freeze
is: newest row fresh (the driver's stats thread alive), `app_active` (frontmost — a backgrounded
game may legitimately stop presenting, and its freezes are covered by the perf counters and the
game's own crash report instead), and no `present_shown` in the last `--rows` rows. It arms only
after frames have flowed, captures once per freeze into `cpu-profile/freeze-<ts>.trace`, and
re-arms when frames resume. `--replay --no-record` is the offline self-test. It picks the game's
stats file over the prefix's `explorer.exe` one (`WINEMAC_INPUT_STATS` is process-global, and
explorer writes rows too, briefly out-newing the game mid-transition).

## Load stall or the hang — the discriminator

Under heavy CPU stress the game shows 2–8 s frame stops that are *not* the hang. The
`STEAMDRIVER_PERF` counters tell them apart within seconds:

| | Load stall (stress) | The hang (C04/C06) |
|---|---|---|
| Duration | 2–8 s, then frames resume | 60+ s, never resumes |
| GameThread | working: 60–97 ms user per 100 ms, ~25,000 NT calls per 100 ms | dead: zero calls (block) or one looping call (spin, ~2 M `PeekMessage`/s) |
| Outcome | nothing | game's watchdog: "Hang detected on GameThread" ~60 s in, crash report |

That is why the watcher needs `--rows 20` under stress (default 6 s fires on load stalls —
it did, on an 8-second one). The hang is 60+ s and dead; let it run, the watchdog crash is part
of the capture.

## Running a stress session

Launch options (counters, driver stats, light logging — never more than `err+fixme` plus our
own channels for the game; see the debug-output rules in the environment's CLAUDE.md):

```
STEAMDRIVER_PERF=1 WINEMAC_INPUT_STATS=/tmp/finals-stress WINEDEBUG=err+fixme,+macfinals_ui %command%
```

The run script restarts the prefix's Wine session when WINEDEBUG comes from the launch options,
so winedevice.exe logs too. Then, while playing (transitions, occasional slow Cmd-Tab — the
known triggers are alt-tab, AFK and leaving the lobby while focused):

```sh
./cpu_spin 18 &
python3 freeze_watch.py /tmp/finals-stress --rows 20
```

If the watcher reports `FREEZE`: leave the game alone ~70 s — Time Profiler records through the
watchdog crash. Then read, in order:

1. `perf_report.py` on `perf/<session>/wine.<pid>` — the PeekMessage rate and GameThread CPU in
   the frozen seconds **decide spin (C04) vs block (C06)**.
2. `python3 cpu_profile.py analyze <compatdata>/cpu-profile/freeze-*.trace` — where the frozen
   threads actually are.
3. The game's new `CrashContext.runtime-xml` (`…/Discovery/Saved/Crashes/UECC-Windows-…`).
4. `steamdriver-wine.log`: macdrv's >10 s main-thread-wait ERRs and the stuck dump's
   `ui state` / `ui wait` lines — a rendezvous block names itself there.

Operational note: the watcher died once to its supervising monitor's 30-minute expiry, which
killed the xctrace child mid-capture (empty trace dir). Re-arm the watcher before expiry when a
capture could be in flight.

## Findings (2026-11-25)

- **The hang did not reproduce.** ~50 minutes of play with burners raised 7 → 10 → 12 → 18; at
  18 the game kept ~2.9 of its normal 6.4 cores (~45 %) and stayed playable. Every stall seen
  (six short ones, one 8 s at 05:19) was a load stall by the table above — GameThread working,
  ~25 k NT calls per 100 ms throughout.
- **Logging is not the cause.** Full diagnostics logging writes 2–3 KB/s on the same cores, and
  the 04:09 crash reproduced with the light default logging already.
- **A session end at 05:22 was a clean quit, not the hang**: game backgrounded, no crash report
  anywhere (game's Crashes folder and macOS DiagnosticReports both empty for the day), Wine
  session ended normally. Not every game exit during the hunt is the crash — check before
  counting one.
- **One transition showed bursts of `NtUserEnumDisplaySettings`** (display-mode enumeration) — a
  GameThread → mac-driver rendezvous candidate, not yet analysed.

## What the stress run got wrong

The instrumentation worked; the reproduction strategy didn't fit the bug.

1. **Too short to mean anything.** The hang has shown up about four times in eleven days
   (11-14, 11-15, 11-21, 11-25). Fifty quiet minutes is no evidence either way.
2. **Uniform CPU starvation doesn't widen a race.** The suspect is a lost wakeup or a lock
   handoff between the GameThread and the Mac main thread. Burners slow every thread by about
   the same factor, so the order of events barely changes. Starved to ~45 %, the game also ran
   fewer frames, pump iterations and transitions per minute, which means *fewer* chances for
   the race per minute. Stress probably lowered the odds.
3. **The targeted stress never ran.** `event_flood` (load the Cocoa main thread, the C06
   suspect) was blocked on Accessibility trust, so every event was silently dropped.
4. **We counted minutes when we should have counted triggers.** The 04:09 crash happened on an
   unloaded system the moment Rasmus left the lobby. The triggers are transitions (leaving the
   lobby, loading into a match, alt-tab, AFK), and nobody counted how many happened during the
   50 minutes.
5. **Unwatched tail.** The watcher ran under a 30-minute monitor and was killed at expiry, so
   the 05:23 relaunch went unwatched. Run it as a plain background job instead.

## Trigger drill result (2026-11-25 05:53)

**Reproduced on the first drill, ~12 minutes in, no burners.** The watcher fired 6 s into the
freeze and captured it; the counters and a stuck dump at 33.8 s show a **block (C06 family), not
a spin**. GameThread stops dead in `NtUserPeekMessage` right after a worker creates and destroys
a window, with Worker #0 blocked inside `NtUserDestroyWindow` and a third thread (0154) blocked
outside NT calls. The Cocoa main thread stayed responsive (lag probe max 0.2 ms), which rules out
C06's main-thread theory for this one. Full write-up and the deadlock hypothesis are in
[C06's fourth-occurrence entry](../../crashes/C06-gamethread-stop-in-play.md). Next: instrumentation
that names the user-lock and `win_data_mutex` holders at the freeze, then drill again.

## Performance findings (2026-11-25)

Details and numbers in [R26](../rosetta_nt/R26_CPU_PROFILE.md).

- **Thread-band switch (`STEAMDRIVER_BG_THREADS=1`): no effect.** 10-minute A/B in the
  practice range: Background Workers on the fastest cores 40 % → 39 %, frame rate 138.7 →
  139.1, p95/p99 frame times identical. Wine threads have no QoS class, and base priority alone
  does not steer cluster placement. Stays off.
- **`NtProtectVirtualMemory` bursts happen only at transitions** (start-up/loading, entering
  and leaving the range: ~19 s in a 15-minute session; none during play). During a burst the
  GameThread makes up to ~28,000 one-page protects a second on the game's own image; Wine's
  handling of them is ~2 % of the GameThread's time. The rest is Rosetta: exception delivery,
  fresh translation of newly-executable pages, translated execution. Capture with
  `burst_watch.py`.
- **Maybe in the future:** a fault fast path (access faults straight into Wine's handler,
  like the syscall fast path) would cut the exception-delivery part. Not pursued: it only
  shortens loading/menu time, and it would widen the Rosetta scope onto the exception path
  (Rasmus's decision; C10's panic history). See R26, "Maybe in the future".
- What limits in-match frame rate is the GameThread itself (~0.9 cores of translated game code
  on the fastest cores) plus GPU submission.

## Passive capture — the current approach (from 2026-11-25 08:20)

Both hangs are rare (one each in ~2 h of drilling), so active drilling is parked. The logging
stays on during ordinary play, and the next occurrence of either hang is read off it.

**Two different hangs, both caught once fully instrumented:**

| | C06 (05:53) | C16 (07:54) |
|---|---|---|
| Shape | deadlock: 3 threads blocked in Unix code, process idle | livelock: ~16 threads bounce in Rosetta's exception server, which runs a full core |
| GameThread | in `NtUserPeekMessage`, never returns | "not in an NT call"; its next syscall never reaches Wine |
| Game's watchdog | crashes the game at 60 s ("Hang detected on GameThread") | stuck itself; no crash report, the game stays frozen |
| Main thread | idle, responsive | responsive |
| Next question | who holds which lock | what the last memory-protection call changed; did an `abort()` (Metal assertion) happen |

**What's in place (runtime `macfinals-171`):**

- `wine-diag-lock-holders.patch` (`-168`): the stuck dump names the holder (thread, how long,
  call site) of win32u's user lock and winemac's `win_data_mutex` (`lock:` lines), and every
  contended wait on them (`ui wait: tid A waiting for the user lock; tid B`). The dump fires 10 s
  into a stuck non-waiting win32u call (PeekMessage, DestroyWindow, …) instead of only at 60 s.
- `wine-diag-vm-ring-abrt.patch` (`-171`): the dump lists calls still in flight and the last 32
  `NtProtectVirtualMemory` / `NtFreeVirtualMemory` / exec-region registrations (`vm:` lines:
  thread, range, protection, status, enclosing view, image flag, `STILL IN THE CALL`), and the
  last SIGABRT (`SIGABRT:` line: thread, name, rip, time).
- Everything is diagnostics only; no lock, protection or signal behaviour changed.

**Play with these launch options (keep them on):**

```
STEAMDRIVER_PERF=1 WINEMAC_INPUT_STATS=/tmp/finals-stress WINEDEBUG=err+fixme,+macfinals_ui %command%
```

Optionally run `python3 freeze_watch.py /tmp/finals-stress` for a Time Profiler capture at the
freeze; the dumps don't need it.

**When it happens:** leave the game alone ~70 s (C06 crashes by itself at 60 s; C16 stays frozen
— force-quit it after that). Then read the newest `perf/<session>/wine.<pid>`:

1. `grep -n stuck-dump` — the first dump after the freeze is the one that matters.
2. Its `lock:`, `ui wait:`, `vm:` and `SIGABRT:` lines, and the GameThread's line (`014c …`).
3. C16 check: per-thread CPU (`com.apple.rosetta.exceptionserver` at a full core =
   C16); see the capture entry in [C16](../../crashes/C16-load-hang-loader-winmm.md).
4. C06 check: a new `UECC-*` crash report with GameThread in `NtUserPeekMessage`; see
   [C06](../../crashes/C06-gamethread-stop-in-play.md).

## Next session (parked plan)

### 1. Trigger drill (no burners)

Same launch options as above. Watcher at the default 6 s threshold, because without burners a
6-second frame stop while frontmost is already abnormal:

```sh
python3 freeze_watch.py /tmp/finals-stress
```

Then loop, keeping a tally: **leave the lobby → rejoin**, **load into a match**,
**alt-tab away ~10 s → back**, and every few cycles **AFK 2–3 min while frontmost**. Aim for
20–30 lobby-leave cycles. Write the cycle count down at the end. A clean run of N transitions is
a result; a clean run of N minutes isn't.

If `event_flood` gets Accessibility trust (`./event_flood --ask` prints
`accessibility trusted: 1`, and `mac_moves` climbs in the stats rows), run it at 500/s during
half of the drill cycles. That's the one stress that targets the C06 suspect.

### 2. Thread-band A/B (performance cores)

**Done 2026-11-25 08:27–08:34: no effect.** Background Workers' share of the fastest cores went
37 % → 40 % with the switch on; GameThread/submission unchanged. Results and reasoning in
[R26](../rosetta_nt/R26_CPU_PROFILE.md) ("Thread-band A/B result"). The plan below is kept for
reference.

Paused since 04:09. Context from [R26](../rosetta_nt/R26_CPU_PROFILE.md): the game uses ~6.4
cores. The **S cluster (the 6 fastest cores) runs 84 % busy at 3.68 GHz while the P clusters sit
at 34 %**. GameThread (0.90 cores) and GPU submission (0.88 cores) are the limit, competing with
~2.5 cores of background workers on the same fast cores. Wine threads carry no QoS class (Mach
policies from `apply_thread_priority` opt them out; QoS can't be set back from outside), so
macOS can't steer workers off the S cluster by itself.

`STEAMDRIVER_BG_THREADS=1` (macfinals-165, default off) has wineserver clamp below-normal-priority
threads to importance ≤ −12, the background band. The hope is that workers drift to the P
clusters and leave the S cores to GameThread and submission.

- **Run A (switch off)** is the 04:09 session (trace `cpu-profile/20261125-040742.trace`). It
  crashed, so redo it rather than trust it as the baseline.
- **Run B:** `STEAMDRIVER_BG_THREADS=1 STEAMDRIVER_PERF=1 WINEMAC_INPUT_STATS=/tmp/finals-ab WINEDEBUG=err+fixme,+macfinals_ui %command%`.
- Same practice-range spot and view, same length (~3 min), Time Profiler via
  `cpu_profile.py record`. Compare the per-cluster split (`cpu_profile.py analyze` core column:
  S vs P busy %), GameThread ms per 100 ms (`perf_report.py`), and frame pacing from the stats
  rows (`present_stats.py`). Win condition: GameThread and submission land on S cores more
  often, and frame time p95 drops. Watch that background-band workers don't starve loading
  (longer transition stalls would show in the stats rows).
- Keep it separate from the drill. If the hang hits during an A/B run, the run is void for
  performance, but the crash evidence still counts.

### 3. "Running on battery" under Wine

The 04:09 crash context says `IsRunningOnBattery=true`, but `pmset -g log` shows this Mac on AC
without a break since 11-24 00:47. The game's view of power isn't truthful, and Unreal may scale
behaviour on battery.

What the code does:

- Wine's macOS `fill_battery_state` (`dlls/ntdll/unix/system.c`) reads IOKit's power sources
  and sets `AcOnLine` from `kIOPSPowerSourceStateKey == "AC Power"`. That looks right.
- `GetSystemPowerStatus` (`dlls/kernel32/powermgnt.c`) builds `BatteryFlag` from the
  percentage: `HIGH` (>66 %), `LOW`/`CRITICAL`, plus `CHARGING` only while actually charging.
- Unreal's Windows `IsRunningOnBattery` (as far as known, from the engine source) switches on
  `BatteryFlag` alone, not `ACLineStatus`: exactly 1/2/4/8 → "on battery". On AC at >66 % with
  macOS **holding the charge (optimized charging, the 80 % limit), it's not charging**, so
  `BatteryFlag = HIGH = 1` → "on battery". A Windows laptop would report the same in that
  state, so this may be Unreal's quirk, not a Wine lie. That needs confirming before changing
  anything.
- Separate upstream bug in `fill_battery_state`: `CFRelease(blob)` runs **before** the loop's
  `source` dictionary (owned by `blob`) is read. That's a use-after-free that could read garbage
  for any field. Worth a look regardless.

To check: a small probe (`GetSystemPowerStatus`, print every field) run in the prefix, in three
states: charging, AC-not-charging (at the limit), battery. If Wine reports `ACLineStatus=1` on
AC, the values are truthful and nothing should change, because hiding the battery would make
the environment less truthful. Only a real mismatch (wrong `ACLineStatus`, garbage from the
use-after-free) gets a fix. Not a hang suspect; logged so it isn't forgotten.

## Parked

- **`event_flood`**: `CGEventPost` to the HID tap is silently dropped unless the binary is
  trusted in System Settings → Privacy & Security → **Accessibility**. "Device Control" and
  "Data Access" grants did nothing (`./event_flood --ask` still printed
  `accessibility trusted: 0`, and the driver's `mac_moves` counter never moved at 500 events/s
  with and without wiggle). Build:
  `clang -O2 event_flood.c -framework CoreGraphics -framework ApplicationServices -framework CoreFoundation -o event_flood`.

## Open

- Catch one real freeze with the counters on. That's the whole point of the kit; the launch
  options above are the next-occurrence recipe.
- Spin vs block for the 04:09 crash remains undecided ([C04's 2026-11-25
  entry](../../crashes/C04-load-spin-gamethread.md) has the full analysis).
- The `NtUserEnumDisplaySettings` bursts during transitions.
- The battery-state probe (section 3 above).
