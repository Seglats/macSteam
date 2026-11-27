# Known crashes and hangs

One file per crash or hang in this folder: what it looks like, the evidence, what we think causes
it, and how to reproduce it. When a fix is done, `fixes/<id>-fix.md` holds the confirmed cause and
the fix, and the crash file links to it. Paths are relative to `environments/finals/`.

**Origin:** *ours* = caused by our changes (hook, Wine patches, runtime); *before* = there
before our changes (Wine, Rosetta, the game, the toolchain); *unknown*.
**Status:** *fixed* (in the runtime named) / *open* / *mitigated* (avoided, not fixed) / *cosmetic*.

| ID | What you see | Origin | Status | Reproducer | Fix |
|---|---|---|---|---|---|
| [C01](C01-rosetta-startup-crash.md) | Crash during start-up: `rosetta error: no code fragment associated with the given arm pc`; Denuvo "taking longer to start" | ours | fixed, `macfinals-113` | `teardown_check.py --mode exit`, `--mode suspendonly` | [C01-fix](fixes/C01-fix.md) |
| [C02](C02-suspend-hang-deferred-signal.md) | Thread never answers a suspension (`GetThreadContext` waits forever); found while fixing C01, never shipped | ours | fixed before shipping (`macfinals-113`) | `teardown_check.py --mode suspendonly` | [C02-fix](fixes/C02-fix.md) |
| [C03](C03-quit-hang-lost-sigusr1.md) | Quitting hangs; ~25 threads log `wait timed out` on loader/threadpool/winmm locks | ours | fixed, `macfinals-114` (reproducer; in game untested) | `teardown_check.py --mode waitexit` | [C03-fix](fixes/C03-fix.md) |
| [C04](C04-load-spin-gamethread.md) | After loading into a match the GameThread spins on `PeekMessage` (~2 M returns/s, lost wakeup — not C06's block); Unreal reports "Hang detected on GameThread" | unknown | open | none yet | – |
| [C05](C05-quit-hang-silent.md) | Quitting hangs right after the window is destroyed; no thread kills, no lock timeouts | unknown, probably before | open | none yet (A/B in game pending) | – |
| [C06](C06-gamethread-stop-in-play.md) | GameThread stops at 0 % CPU during play or at transitions; after 60 s Unreal's hang watchdog crashes the game ("Hang detected on GameThread") | ours/CrossOver (winemac lock order: `macdrv_DestroyWindow` releases D3DMetal client surfaces under `win_data_mutex`, the present path takes `surfaces_lock` then `win_data_mutex`) | cause found 2026-11-25; fix built (`wine-macdrv-destroy-surfaces-lock-order.patch`, `macfinals-174`), in-game confirmation pending | play; transitions (leaving the lobby/range) | – |
| [C07](C07-nested-exception-noise.md) | `nested exception on signal stack ... 0xa8` lines when threads are killed | before (Wine) | fix installed in `macfinals-117`; confirm at the next game quits | any quit, any process exit | – |
| [C08](C08-game-app-ntdll.md) | Every launch fails: "could not load ntdll.so" (`macfinals-104`) | ours | fixed, `macfinals-106` | launch anything on `-104` | [C08-fix](fixes/C08-fix.md) |
| [C09](C09-tf-single-step-crash.md) | Game crash `STATUS_SINGLE_STEP` at the fast-path target + 4 (first run, new account) | ours | fixed, `macfinals-42` | not reproduced by probes | [C09-fix](fixes/C09-fix.md) |
| [C10](C10-kernel-panic-thunk-scanner.md) | macOS kernel panic (twice) | ours | mitigated: scanner quarantined behind `MACFINALS_THUNK_PATCH=1` | the scanner itself; don't run it | – |
| [C11](C11-first-launch-tos-minidump-hang.md) | First launch after a change sometimes freezes; Steam's TOS assert and a minidump | before (Wine) | assert fixed `macfinals-31`; the freeze itself unexplained | none | [C11-fix](fixes/C11-fix.md) |
| [C12](C12-e111000d-conflicting-software.md) | "Conflicting software detected" (E111000D), game quits | before (toolchain) | fixed, `macfinals-19` | build Wine's PE side with llvm-mingw | [C12-fix](fixes/C12-fix.md) |
| [C13](C13-disallowed-tool-heap-logging.md) | Anti-cheat quits with "disallowed tool" | ours (logging setup) | mitigated: never log with heap warnings | `WINEDEBUG=warn+heap` (don't) | – |
| [C15](C15-staterecovery-loop-assert.md) | Rosetta assertion `unexpected direct branch in Loop CFG` (StateRecovery); thread dies | ours | fixed, `macfinals-117` | `teardown_check.py --mode mixed --winedebug err+all` | [C15-fix](fixes/C15-fix.md) |
| [C16](C16-load-hang-loader-winmm.md) | Freeze while loading in; loader lock and winmm lock held, Rosetta exception server at 100 % | unknown, not today's hook changes | open | none yet | – |
| [C17](C17-slow-leave-and-quit-messagecall-ebadf.md) | Leaving a round and quitting are slow; the quit never finishes (a `SendMessage(WM_GETTEXT)` waits forever; one thread lost its wineserver connection) | unknown, not the fast path | open | none yet | – |
| [C14](C14-menu-buttons-shader-preload.md) | Main-menu buttons don't open menus while shaders preload | unknown | open | none | – |
| [C18](C18-alt-tab-input-steal.md) | After Cmd-Tab away, the game takes the mouse and keyboard back; stops only after switching to an app on the same screen, then moving to another monitor | ours (REC-45 menu bar hiding changed presentation options from the background → WindowServer re-fronted the game) | cause confirmed; fix built (`wine-macdrv-c18-presentation-inactive.patch`), in-game test pending | borderless game; Cmd-Tab to an app on another monitor | – |
| [C19](C19-min-spec-dialog-hang.md) | ARC Raiders: start hangs with the "Minimum Hardware Requirements Not Met" box open; a second launch hangs behind it | unknown | maybe, infrequent (seen once); mitigated by the box's auto-answer | none yet | – |

## Tools

| Tool | For |
|---|---|
| `tests/finals_bisect/teardown_check.py` | C01–C03 without the game: modes `exit`, `suspend`, `suspendonly`, `mixed`, `waitexit`; `--hook`, `--nt-off`, `--debug`, `--winedebug` |
| `tests/finals_bisect/startup_crashes.py` | C01 in the game: start-up crashes per launch from the Wine log |
| `tests/finals_bisect/ab_launches.py` | One line per game launch: runtime, `nt_off=`, run time, how it ended, Unreal crash reports |
| `tests/finals_bisect/perf_report.py` | `STEAMDRIVER_PERF=1` sessions: per-thread NT calls, timeline (where a freeze starts) |
| Unreal crash reports | `<prefix>/drive_c/users/crossover/AppData/Local/Discovery/Saved/Crashes/*/CrashContext.runtime-xml` |

## Adding one

Copy this into `Cnn-short-name.md` and add a row above.

```markdown
# Cnn: <what you see>

| | |
|---|---|
| First seen | date, runtime, launch options |
| Origin | ours / before / unknown (why we think so) |
| Status | open / fixed in ... / mitigated / cosmetic |
| Reproducer | command, or "none yet" |
| Fix | link to fixes/Cnn-fix.md, or – |

## What you see
## Evidence
## What we think
## Next steps
```

A fix file (`fixes/Cnn-fix.md`) is written once the cause is confirmed: the cause, the change (files,
patches, runtime), and how it was verified (before/after numbers).
