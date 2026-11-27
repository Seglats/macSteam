# C04: GameThread spins after loading into a match ("Hang detected on GameThread")

| | |
|---|---|
| First seen | 2026-11-14 16:43, `macfinals-114`, fast path on, `STEAMDRIVER_PERF=1`; once |
| Origin | unknown |
| Status | open |
| Reproducer | none yet |
| Fix | – |

## What you see

Loading into a match, the game freezes. About a minute later Unreal's crash reporter opens with
`CrashType Hang`, "Hang detected on GameThread". Report:
`<prefix>/.../Discovery/Saved/Crashes/UECC-Windows-AA044A1A…/CrashContext.runtime-xml`.

## Evidence

- `perf_report.py` (session `perf/20261114-164343`): normal play until 80.0 s, then the async
  loading thread works for 0.7 s. At 80.8 s every other thread goes idle and the GameThread loops
  on `NtUserPeekMessage` (about 2 million calls a second) plus `NtYieldExecution` and
  `NtQueryPerformanceCounter`, at 100 % of a core, with rendering at 0 %.
- Crash report stacks: GameThread in `win32u!NtUserPeekMessage` (return address right after the
  stub's `syscall`). Render thread idle in `SleepConditionVariableCS`. Worker pools idle in
  `NtWaitForSingleObject`. Nothing else unusual in the Wine log.
- Wine's PE-side syscall stubs use a real `syscall` (class 0), so `PeekMessage` and every other
  Wine NT call go through the fast path when it's on.

## What we think

A lost wakeup: the GameThread waits in a message-pumping loop for an event or completion that
never comes, while the threads that should deliver it are idle. Whether that's the hook (a lost
signal or notification, like C02/C03) or the game or Wine isn't known. The `-113` launch at 15:47
loaded into a match and played fine.

**Checked and ruled out (2026-11-14):** [C15](C15-staterecovery-loop-assert.md), a Rosetta
assertion that kills a thread when a signal lands in a diverted block, would leave the GameThread
waiting like this. But `steamdriver-wine.log` has no `StateRecovery`/`assertion failed` line in
any launch, and Rosetta's own messages (`rosetta error`) do reach that log. So C04 is something
else.

## Next steps

A/B in game with `ab_launches.py`: `MACFINALS_NT_OFF=syscall` against fast path on, several match
loads each. If it only happens with the fast path on, look at it like C03 (counters, per-thread
pending words in a trace build).

**Update 2026-11-17:** the 2026-11-15 capture of the *same watchdog crash* ("Hang detected on
GameThread", GameThread leaf in `NtUserPeekMessage`) turned out to be a **block, not a spin** —
one `NtUserPeekMessage` that never returned, wedged on the winemac event-queue lock; that is
[C06](C06-gamethread-stop-in-play.md), diagnosed and mitigated there (`macfinals-133`). C04's
signature stays distinct: PeekMessage *returns* ~2 M times a second (a lost wakeup somewhere),
so C04 is not covered by C06's fix. If C04 recurs, its counters should be checked for the same
one-call-never-returns shape first before treating it as the spin again.

**Seen again 2026-11-25 04:09, `macfinals-165`, `STEAMDRIVER_BG_THREADS` off (the A run of the
thread-band A/B), no counters, `WINEMAC_INPUT_STATS` on.** Leaving the lobby: frames stopped at
04:09:12 (≈120/s until 04:09:11) while the game was frontmost; Rasmus switched away ~22 s later;
"Hang detected on GameThread" at 181 s since start (`UECC-Windows-2D1B616541E0267AFF5ADB89A7E498FF_0000`),
GameThread leaf `win32u!NtUserPeekMessage` right after the stub's `syscall`
(`win32u+0x152c4` = `NtUserPeekMessage+0x14`). The process used one full core at 04:10:08 (could be
the spin, or the crash reporter writing its dump). Without counters, spin vs block (C06) is not
decided. Next occurrence: run with `STEAMDRIVER_PERF=1` so `perf_report.py` shows the
PeekMessage rate in the frozen seconds.

**Follow-up (2026-11-25, same occurrence — full crash context read):**

- GameThread (ThreadID 464, IsCrashed, `Misc.IsStuck=true`, `StuckThreadId=464`): leaf
  `NtUserPeekMessage+0x14`, called from a **nested pump at `Discovery-d+0x2eec3d` — the same
  site as C06's 2026-11-21 capture** (a nested pump inside a game handler, not the outer
  `+0x6f28fa` pump of C06's 2026-11-15 capture).
- All 80 other threads parked in normal waits (79 with an ntdll leaf; WindowsRawInputThread in
  `NtUserGetMessage+0x14` — its normal park, or queued on the user lock as in C06's 11-21
  capture). **No user-lock holder visible anywhere** (the 11-21 capture had one in
  `NtUserDestroyWindow`).
- The game's own crash-context fields: `Platform.AppHasFocus=true` (frontmost throughout — no
  alt-tab, not AFK; **third trigger: leaving the lobby while focused**), `IsRequestingExit=false`,
  memory fine (6.3 of 48 GB used, no OOM), `IsRunningOnBattery=true`, `TimeOfCrash` 04:10:15 =
  **63 s after frames stopped** (the 60 s heartbeat, as in both C06 captures). So the 102 % CPU
  observed at 04:10:08 was *during* the freeze — **one core still busy**, which fits C04's spin
  better than C06's block (or an unknown runnable thread).
- The Wine session ended cleanly (`game exited (0)` 04:11:46): no C05/C17 teardown hang.
- Classification: **undecided between C04 (spin) and C06 (block).** The stack shape serves both;
  the caller site matches a C06 capture; the still-busy core leans C04; and the trigger — a level
  transition (leaving the lobby here, loading into a match on 2026-11-14) — matches C04's two
  trigger contexts, while C06's were alt-tab and AFK.
- Next occurrence: `STEAMDRIVER_PERF=1 WINEDEBUG=err+fixme,+macfinals_ui %command%`. The
  counters decide spin vs block within seconds (PeekMessage rate, GameThread CPU); macdrv's
  >10 s main-thread-wait ERRs (on by default) name a rendezvous block. The 60 s stuck dump fires
  at the same moment as the game's watchdog — too late to rely on.
