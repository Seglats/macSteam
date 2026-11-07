# MacVolvo with NotProton

[NotProton](https://github.com/NotProtonNot/NotProton) turns on Steam Play in the macOS Steam
app. It runs Windows games in a copy of CrossOver (a "runner"), with its Steam bridge patched
into the runner's `ntdll`. MacVolvo doesn't replace any of it. **Settings › General ›
NotProton › Add to Steam** adds MacVolvo's Mac driver and mouse settings through NotProton's
own extension points. There's no installer script; the app does it, and **Remove** undoes it.

## What Add to Steam writes

| Path (under `~/Library/Application Support`) | What |
|---|---|
| `MacVolvo NotProton/<build>/winemac.so` | MacVolvo's Mac driver for that CrossOver build |
| `MacVolvo NotProton/settings` | the mouse settings (`WINEMAC_*`), rewritten whenever they change in MacVolvo |
| `notproton/variants/macvolvo.conf` | a NotProton runner variant per build: lays the driver over `lib/wine/x86_64-unix` and names its `build` |
| `Steam/compatibilitytools.d/notproton-macvolvo-26.3/` | the Steam compatibility tool "MacVolvo (CrossOver 26.3)" |

The tool's `run` (template: `app/Resources/NotProton/run.sh`) does three things:
1. adds the variant to `NOTPROTON_RUNNER_VARIANT`, keeping any variant you chose for the game
2. exports the allowlisted mouse settings, unless the game's launch options already set them
3. `exec`s NotProton's own tool (`notproton-26.3/run`), unchanged

NotProton then builds a runner copy with the driver on first launch and does everything else.

**To use it for every game:** in Steam › Settings › Compatibility, choose "MacVolvo" as the
default tool. To use it for one game, choose it in that game's Properties › Compatibility.

## Why it's built this way

- **No full runtime:** MacVolvo's runtime has Recall's own `ntdll`, so it can't carry
  NotProton's Steam bridge. Variants may not replace `ntdll` or `lsteamclient`.
- **Only the Mac driver** is laid over the runner. It's compiled from the same CrossOver 26.3
  sources as runner `26.3.0.39832`:
  - It imports exactly the same 35 `ntdll` and 72 `win32u` symbols as CodeWeavers' own
    `winemac.so`.
  - The patches change only the Unix side, never `dllmain.c` or `unixlib.h`, so CrossOver's
    `winemac.drv` still matches.
  - The runner's Wine loader has `disable-library-validation`, so the ad-hoc driver loads.
- **Why a tool and not just the variant:** NotProton's variant choice and plugin rows are
  saved per game, and its global defaults file only takes a fixed list of keys. A tool you set
  as Steam's default is the one global switch.
- **Folder name:** NotProton's hooks in Steam only handle tools named `notproton…`. On startup
  NotProton removes unlisted `notproton…` folders, but only those holding a `flavor` file or
  its own `run` script (`compat.c`, `written_here`). MacVolvo's folder has neither, as the
  FINALS compatibility package does. `tests/test_notproton_tool.py` checks this.
- **Ownership:** every file carries MacVolvo's marker. Add refuses to overwrite a file without
  it, before writing anything, and Remove deletes only marked files.

## Supporting another CrossOver build

1. Build `winemac.so` against that build's sources (see [UPSTREAM-RECALL.md](UPSTREAM-RECALL.md)).
2. Add the build to `NOTPROTON_BUILDS` in `scripts/build_native_app.py`.
3. Check its imports against that runner's `ntdll.so` and `win32u.so`.

Only Rosetta runners are offered, because the driver is x86_64. FEX runners use arm64 Wine.

To check a variant file the way NotProton reads it, compile NotProton's
`dylib/tests/confdump.c` with `dylib/feats/conf.c`. Run it on the folder that holds
`variants/`; it must print the record with no `!` refusal lines.

## Caveats

- Steam reads compatibility tools at startup, so restart Steam after Add or Remove.
- With High-precision, native Steam starts the games, so macOS asks for Input Monitoring for
  Steam, not for MacVolvo.
- Not yet tried in a game through Steam.
