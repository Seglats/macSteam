# C06: GameThread stops at 0 % CPU during play

| | |
|---|---|
| First seen | 2026-11-13 18:12, `macfinals-106`, `STEAMDRIVER_PERF=1` |
| Origin | unknown (before the 2026-11-14 hook changes) |
| Status | open |
| Reproducer | none |
| Fix | – |

## What you see

The game freezes during play; Rasmus quit it with Wine's quit.

## Evidence

- Counters: the GameThread stopped making NT calls and sat at 0 % CPU.
- The `nested exception on signal stack` lines that followed came from quitting (Wine killing
  threads), not from the freeze ([C07](C07-nested-exception-noise.md)); that theory was
  retracted.

## What we think

Unknown. Shares the "GameThread blocked at 0 %" shape with [C05](C05-quit-hang-silent.md), which
happened at quit with the fast path off.

## Next steps

If it recurs with `STEAMDRIVER_PERF=1`, note the time. The GameThread's last calls in the counters
(as done for C05) show what it blocked on.
