# Wine environments

SteamDriver's own-Steam mode runs Windows Steam and its games in a Wine environment that
the app ships and builds itself. Different games need different Wines, so each environment
has its own folder here: its Wine patches, build scripts, extra components and notes.
SteamDriver's app side (the window, NotProton options, the mouse settings) is shared by all of them.

| Environment | For | Wine | Graphics | Status |
|---|---|---|---|---|
| [`base`](base/) | Counter-Strike 2, Deadlock, everything else | CrossOver 26.3 + Recall/MacVolvo patches | DXMT (Direct3D 11) | what SteamDriver ships today |
| [`finals`](finals/) | THE FINALS | CrossOver 26.3 + MacFinals patches (MinGW-w64 GCC PE side) | Apple D3DMetal, Direct3D 12 | runs and plays; merged from MacFinals 2026-11-09 |

## What every environment shares

- **The Mac driver's mouse and window work** (`winemac.so`: mouselook, High-precision HID
  mouse, precise/classic input, activation and canvas fixes). Its patches are maintained in
  the base environment (`patches/wine-v1-canvas.patch`, `wine-winemac-activation.patch`,
  `wine-mouselook.patch`); other environments carry the same files in their own patch list.
  When the driver changes, update every environment's copy.
- The app, the Steam setup worker and the NotProton companion.

## Adding an environment

1. Make `environments/<name>/` with its own `scripts/` and `patches/`, laid out like the
   repository root, so its scripts' `ROOT` (two levels up from a script) is the environment
   folder. Shared, untracked inputs (`runtime/toolchains`, `gptk/`) and shared tracked ones
   (`config/`, `licenses/`) are found through `REPO`, the repository root.
2. Carry the shared Mac driver patches, in the same order as base.
3. Give its runtimes their own version prefix (`macfinals-N` for finals) so artifacts from two
   environments can never be confused.
4. Add a row to the table above and a README that says how to build and test it.
