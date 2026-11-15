# C09 fix: decline the fast path while TF (single-step) is live

Fixed in **`macfinals-42`**. Source: `tests/rosetta_nt/NOTES.md`, "r21e (2026-11-09)".

## Cause

The game single-steps parts of its own start-up (on a new account's first login). When the fast
path resumed into Wine's dispatcher with the guest's TF flag live, the step trap fired after one
dispatcher instruction (target + 4). The stock SIGSYS path handles TF correctly.

## Fix

A guard in the stub (label 18): if TF is live (x17 bit 4, or Rosetta's TF byte at `ctx+0x444`),
count it (`ctl[40]` byte 7) and bail to the stock path. While building it, a liveness bug was
found and fixed: the first version read the TF byte into `w23`, which still held `_g_ctl`, so
every fast-path entry wrote through a garbage pointer.

## Verified

Probe runs on `macfinals-41`'s Wine with the new hook (NOTES.md "Verification"), then shipped in
`-42`. The same crash hasn't been seen since.
