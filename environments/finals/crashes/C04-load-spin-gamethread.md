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
