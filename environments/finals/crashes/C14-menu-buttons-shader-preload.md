# C14: main-menu buttons don't open menus while shaders preload

| | |
|---|---|
| First seen | noted on `macfinals-31` (2026-11-02); Rasmus's observation 2026-11-13: only while shaders preload, gone afterwards |
| Origin | unknown |
| Status | open |
| Reproducer | none |
| Fix | – |

## What you see

Clicking main-menu buttons sometimes does nothing. Once shader preloading has finished, they work.

## What we think

Probably the GameThread or the UI waiting behind pipeline creation while shaders compile
(`macfinals_dx12` warn logs pipelines taking ≥ 50 ms). Not investigated yet.

## Data points

- **2026-11-24 06:46 (`macfinals-137`, no `STEAMDRIVER_PERF`):** buttons stopped responding at
  about minute 4–5 and recovered by themselves; the game process was at **536 % CPU** at the
  time (shader preload is the likely load). No `no reply`/lock errors in that stretch; the one
  `no reply` (`WM_GETTEXT` to `015c`) was earlier, during start-up loading. No dumps (perf off).

## Next steps

A run with `WINEDEBUG=warn+macfinals_dx12` and `STEAMDRIVER_PERF=1`, clicking while preloading,
noting the times; compare the GameThread's NT calls and the long pipeline creations at those
times.
