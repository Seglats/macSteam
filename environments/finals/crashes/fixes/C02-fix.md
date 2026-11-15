# C02 fix: deliver deferred signals the way Rosetta does

Fixed in **`macfinals-113`**, with C01. Details: `tests/rosetta_nt/R25_SIGNAL_IN_STUB.md`.

## Cause

Rosetta's deferral (`rosetta/runtime` `0x1d7e0`) parks the thread's signal mask at `ctx+0x1cc`,
sets bit 1 in the pending word `ctx+0x1c8`, re-queues the signal and resumes the thread with
**every signal blocked**. Each of Rosetta's exit routines back into translated code
(`runtime_exit_ret`, `_exitljmp`, `_exit_sys`, …) calls `table[0]`, ORs the flags it returns with
the pending word, and on bit 1 restores the mask with `__pthread_sigmask`. The signal is then
delivered there, at a pc the handler can deliver from. The stub's copy of the page epilogue
(`runtime_exitljmp`) left out that pending branch, and its `ldr`/`str` of the pending word could
wipe bit 1. The stock invalid-syscall path (brk #5, exception server) doesn't restore the mask
either.

## Fix (`tests/rosetta_nt/payload_arm64.c`, `ntfix_macfast`; `hooklib.c`)

1. **Resume through Rosetta's own `runtime_exitljmp`** (`runtime+0x22560`) instead of a copy.
   hooklib checks 7 of its words for this UUID and publishes the address in `ctl[96]`
   (`publish_exitljmp`); unpublished means the stub bails to the stock path.
2. **Queue bit 9 with a compare-and-swap** (0 → `0x200`) so a deferral's bit 1 is never erased.
3. **A deferred signal found at entry** (pending word exactly 2) also goes through
   `runtime_exitljmp`, with nothing queued: no install, the mask is restored, and the `ret` lands
   on the bail word, i.e. the stock path.
4. **The guard's early bail** checks the same and routes through `runtime_exitljmp` (`ctl[97]`
   counts it). It hasn't been seen to fire, but it's correct on its own.

## Verified

`--mode suspendonly`: 10 / 10 hung after C01's relocation alone, 0 / 10 after this; 1,600
suspend/context/resume cycles complete in 2 rounds. `--mode exit` and `--mode suspend` stay
0 failures.
