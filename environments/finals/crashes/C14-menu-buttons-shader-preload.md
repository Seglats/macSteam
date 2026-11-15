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

## Next steps

A run with `WINEDEBUG=warn+macfinals_dx12` and `STEAMDRIVER_PERF=1`, clicking while preloading,
noting the times; compare the GameThread's NT calls and the long pipeline creations at those
times.
