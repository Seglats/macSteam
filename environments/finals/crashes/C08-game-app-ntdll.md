# C08: every launch fails with "could not load ntdll.so"

| | |
|---|---|
| First seen | 2026-11-13, `macfinals-104` |
| Origin | ours (Game Mode for every Wine program) on top of an inherited Recall bug |
| Status | fixed in `macfinals-106` |
| Reproducer | launch anything on `macfinals-104` |
| Fix | [fixes/C08-fix.md](fixes/C08-fix.md) |

## What you see

Nothing starts; `steamdriver-wine.log` says `could not load ntdll.so` from the game app
(`Game.app`).

## Cause (confirmed)

See [fixes/C08-fix.md](fixes/C08-fix.md). Found with a compiled argv-logging stand-in in a scratch
copy of the runtime.
