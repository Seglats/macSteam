# C02: a suspended thread never answers (deferred signal never delivered)

| | |
|---|---|
| First seen | 2026-11-14, local hook builds while fixing C01; never in a shipped runtime |
| Origin | ours: the first version of C01's fix |
| Status | fixed before shipping (in `macfinals-113`) |
| Reproducer | `python3 tests/finals_bisect/teardown_check.py --mode suspendonly` (`TEARDOWN_TRACE=1` shows where it stops) |
| Fix | [fixes/C02-fix.md](fixes/C02-fix.md) |

## What you see

After moving the payload into libRosettaRuntime (C01), every `--mode suspendonly` round hung: the
main thread's first `GetThreadContext` never returned. No crashes.

## Evidence

- `TEARDOWN_TRACE=1`: stuck at `context 0.0`, the first suspension.
- Hook counters in the hung child: 169.8 million fast-path resumes, one busy bail, `c38 2` (the
  pending word showed a deferred signal, bit 1).
- `sample` of the child: the worker kept running, the main thread waited.

## Cause (confirmed)

When Rosetta defers a signal it blocks all signals until one of its exit routines restores the
mask. The stub's own copy of the page epilogue left that step out. Details:
[fixes/C02-fix.md](fixes/C02-fix.md).
