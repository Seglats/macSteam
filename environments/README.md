# Wine environments

SteamDriver's own-Steam mode runs Windows Steam and its games in Wine environments. There is
one **main** Wine, and a folder for each game that needs a Wine of its own. The app side (the
window, NotProton options, the mouse settings) is shared by all of them.

| Folder | Wine for | Wine | Graphics | Status |
|---|---|---|---|---|
| [`main`](main/) | Counter-Strike 2, Deadlock, and every game without a folder of its own | CrossOver 26.3 + Recall/MacVolvo patches | DXMT (Direct3D 11) | what SteamDriver ships today |
| [`finals`](finals/) | THE FINALS | CrossOver 26.3 + MacFinals patches (MinGW-w64 GCC PE side) | Apple D3DMetal, Direct3D 12 | runs and plays; merged from MacFinals 2026-11-09 |

## Where this is going

Environments are **built ahead of time and downloaded per game**: the app ships no runtime of
its own, and when a player picks a game it fetches that game's environment (or main) and
installs it. Nobody downloads the Wine for a game they don't play. Until that exists, each
environment is built locally from its folder (see its README).

## What every environment shares

- **The Mac driver's mouse and window work** (`winemac.so`: mouselook, High-precision HID
  mouse, precise/classic input, activation and canvas fixes). Its patches are maintained in
  main (`patches/wine-v1-canvas.patch`, `wine-winemac-activation.patch`,
  `wine-mouselook.patch` at the repository root); every game environment carries identical
  copies in its own patch list. When the driver changes, update every copy.
- The app, the Steam setup worker and the NotProton companion.

What broke while this was set up, and how to avoid it: [`LESSONS.md`](LESSONS.md).

## Adding a game's environment

1. Make `environments/<game>/` with its own `scripts/` and `patches/`, laid out like the
   repository root, so its scripts' `ROOT` (two levels up from a script) is the game's folder.
   Shared, untracked inputs (`runtime/toolchains`, `gptk/`) and shared tracked ones
   (`config/`, `licenses/`) are found through `REPO`, the repository root.
2. Carry the shared Mac driver patches, in the same order as main.
3. Give its runtimes their own version prefix (`macfinals-N` for finals) so artifacts from two
   environments can never be confused.
4. Add a row to the table above and a README that says how to build and test it.
