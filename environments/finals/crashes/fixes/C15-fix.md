# C15 fix: no backward branch in the divert chain

Fixed in **`macfinals-117`** (hook only; base `-115`).

## Cause

When a signal lands in translated code, Rosetta rebuilds the thread's x86 state from the block
(`StateRecovery.cpp`, `determine_loop_properties`), which analyses the block's control flow. The
divert chain `insert8` puts at each syscall site ended with `b #-28` at `X+28`, a backward branch
to the real call at `X` that the stub's bails returned through. Rosetta read it as a loop in a
shape its own emitter never produces and asserted
(`assertion failed [i + 1 == branch_index]: unexpected direct branch in Loop CFG`), killing the
thread.

## Fix (`tests/rosetta_nt/payload_arm64.c`)

- Bails go straight to `X` = `lr − 28`: label 23 (`sub x30, x30, #28` before parking it, so the
  parked target and `ctx+0x8` hold `X`), the guard bail at 94, and the deferral route at 99.
- `runtime_exitljmp`'s no-install return uses `ctx+0x8` = `X`.
- The `X+28` word is a `nop`.

## Verified

`teardown_check.py --mode mixed --winedebug err+all`, 40 rounds: `-114`'s hook 5 hung with 5
`StateRecovery` assertions, the new hook 0 / 0. Regression suite on the new hook: waitexit 0 / 30,
exit 0 / 20, suspendonly 0 / 10, suspend 0 / 10. Installed `-117`: mixed 0 / 10. Fast path 58 ns
per call.
