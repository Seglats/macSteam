# C09: STATUS_SINGLE_STEP crash at the fast-path target + 4

| | |
|---|---|
| First seen | 2026-11-08 19:20, `macfinals-41`, first run on a new account |
| Origin | ours (syscall fast path) |
| Status | fixed in `macfinals-42` |
| Reproducer | not reproduced by the probes (needs a step-mode entry the probes don't produce) |
| Fix | [fixes/C09-fix.md](fixes/C09-fix.md) |

## What you see

The game's (Embark's) crash reporter, minidump `THEIA_89989837_1424.dmp`: exception `0x80000004`
(STATUS_SINGLE_STEP) at `0x20880b698`, the published fast-path target (`prolog_end`) + 4, on the
main thread, 2.3 s after the inner `Discovery.exe` started.

## Cause (confirmed)

See [fixes/C09-fix.md](fixes/C09-fix.md). Source: `tests/rosetta_nt/NOTES.md`, section "r21e".
