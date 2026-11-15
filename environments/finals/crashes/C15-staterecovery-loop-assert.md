# C15: Rosetta asserts in StateRecovery ("unexpected direct branch in Loop CFG")

| | |
|---|---|
| First seen | 2026-11-14, `teardown_check.py --mode mixed --winedebug err+all` on `macfinals-114`'s hook (2 of 5 rounds hung) |
| Origin | ours: the syscall fast path's divert chain contains a backward branch |
| Status | fixed in `macfinals-117` |
| Reproducer | `python3 tests/finals_bisect/teardown_check.py --mode mixed --winedebug err+all` (intermittent; count `StateRecovery` in the log) |
| Fix | [fixes/C15-fix.md](fixes/C15-fix.md) |

## What you see

In Wine's output:

```
assertion failed [i + 1 == branch_index]: unexpected direct branch in Loop CFG
(StateRecovery.cpp:216 determine_loop_properties)
```

and the thread is dead: the probe round hangs (a suspension never answered, or the exit never
finishes).

## What we think

When a signal lands in translated code, Rosetta rebuilds the thread's x86 state from the block
(StateRecovery). It analyses the block's control flow. The divert chain the hook inserts at each
syscall site (`insert8`) ended with `b #-28` at `X+28`, a backward branch to the real call at
`X`, used by the stub's bails. To StateRecovery that's a loop in a shape its own emitter never
produces, so it asserts.

In mixed mode the workers run a diverted site in loops while being suspended, so signals land in
diverted blocks. In the game, about 32,000 diverted calls a second plus suspensions (anti-cheat,
crash reporter, Wine) make the same thing possible. It was a candidate for
[C04](C04-load-spin-gamethread.md), but the game's logs have no such assertion, so C04 is something
else.

## Fix

No backward branch in the chain: bails go straight to `X` (`lr − 28`), `ctx+0x8` holds `X` for
`runtime_exitljmp`'s no-install return, and the `X+28` word is a `nop`
(`tests/rosetta_nt/payload_arm64.c`: `insert8`, `ntfix_macfast` labels 23, 94 and the deferral
route at 99).
