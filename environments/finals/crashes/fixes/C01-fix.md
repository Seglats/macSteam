# C01 fix: signals inside the hook's native code

Fixed in **`macfinals-113`** (hook only), together with C02. Full investigation:
`tests/rosetta_nt/R25_SIGNAL_IN_STUB.md`.

## Cause

Rosetta's signal handler (`rosetta/runtime`, `signal_handler` → `classify_arm_pc`) looks up the
interrupted ARM pc. It knows translated code (delivers the signal) and its own runtime code
(defers it). The hook's payload (`ntfix_macfast`, the append code, the converters) lived on an
anonymous page of its own, which is neither, so the handler aborted with
`no code fragment associated with the given arm pc`. With about 32,000 fast-path syscalls a
second, a signal often lands there: SIGQUIT to every thread when a process exits (the launcher
handing over at start-up), and SIGUSR1 when a thread is suspended.

## Fix

- **The payload code and the append stub run inside libRosettaRuntime**, in the unused tail of
  its `__TEXT` (`0x65000`–`0x68000` on this UUID). There, `abi_for_address` returns 0 (runtime
  code), so Rosetta defers a signal instead of aborting. The data page stays where it was; the
  `adrp`s that reach it are rebased. The copy is gated (empty gap, readback, branches stay
  inside) and falls back to the old placement. `MACFINALS_NT_OFF=reloc` turns it off.
  (`tests/rosetta_nt/hooklib.c`: `relocate_payload`, `lib_write`; `mkpayload.py` exports
  `P_CODE_START/END`, `P_TEXT_USED`.)
- The rest of the change that makes deferral safe is C02's fix.

## Verified

`teardown_check.py` on the fixed hook: `--mode exit` 0 / 20 (was 20 / 20), `--mode suspendonly`
0 / 10, `--mode suspend` 0 / 10. Fast path 57–61 ns per call (33 ns before; the stock path is
about 16 µs).
