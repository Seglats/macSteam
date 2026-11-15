# C12: "Conflicting software detected" (E111000D)

| | |
|---|---|
| First seen | October 2026, runtimes up to `macfinals-17` |
| Origin | before (toolchain: Wine's PE side built with llvm-mingw) |
| Status | fixed in `macfinals-19` |
| Reproducer | build Wine's Windows-side modules with llvm-mingw (clang + lld) |
| Fix | [fixes/C12-fix.md](fixes/C12-fix.md) |

## What you see

The game shows "Conflicting software detected" and exits (`NtTerminateProcess(-1, e111000d)` in a
debug log).

## Evidence

Swapping only `ntdll.dll` between an llvm-mingw build and CrossOver's own decided pass or fail.
Found by bisecting against CrossOver 26.3's prebuilt Wine in a throwaway setup. Source:
`tests/rosetta_nt/(removed)`, `(removed)`.

## Cause (confirmed)

See [fixes/C12-fix.md](fixes/C12-fix.md). Many earlier theories (NT flag, `%gs`, TLS) were not the
cause; see `CLAUDE.md` "Current status".
