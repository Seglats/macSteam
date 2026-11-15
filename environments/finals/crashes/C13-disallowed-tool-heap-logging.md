# C13: anti-cheat quits with "disallowed tool" when heap warnings are logged

| | |
|---|---|
| First seen | 2026-11-02 |
| Origin | ours (logging setup) |
| Status | mitigated: never log the game with heap warnings |
| Reproducer | `WINEDEBUG=warn+heap` or `trace+heap` (don't) |
| Fix | – (a rule, not a code fix) |

## What you see

The anti-cheat stops the game with "disallowed tool".

## Cause

Heap warnings make Wine set heap validation flags (`dlls/ntdll/heap.c`, `heap_set_debug_flags`),
which look exactly like a process started under a debugger.

## Rule

Never log the game with `warn+heap`, `trace+heap`, `relay` or `snoop`; so never plain `warn+all`
(use `warn+all,warn-heap`). `+seh` also changed the game's behaviour once. `launch_main.sh
DEBUG=max` is the safe maximum (`CLAUDE.md`).
