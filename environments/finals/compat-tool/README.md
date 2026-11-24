# SteamDriver — compatibility tool for macOS Steam

A Steam Play compatibility tool that runs games from the **macOS Steam app** in the finals
environment's Wine. Made for THE FINALS; since 2026-11-24 it is macSteam's global tool,
"SteamDriver" (id `notproton-steamdriver`; until then "SteamDriver: THE FINALS",
`notproton-steamdriver-finals`, which adding the new tool removes). NotProton switches Steam Play on and supplies its Steam bridge, and our
tool replaces the runner. Everything runs inside the game's process, from our own build: no
outside memory patching and no developer mode.

| File | What |
|---|---|
| `run` | Steam calls `run <verb> <game> <args>`. It makes the prefix (`<compatdata>/pfx`), copies NotProton's bridge into its Steam folder, sets the finals environment (D3DMetal, d3d12 layer, Rosetta syscall hook, Mac driver mouse) and starts the game through NotProton's `steam.exe`. |
| `compatibilitytool.vdf`, `toolmanifest.vdf` | Steam's standard tool description (`notproton-steamdriver`; the prefix makes NotProton hand the launch to the shell, see `run`; shown as "SteamDriver"). |
| `runtime/` (installed only) | the finals runtime (`macfinals-N` with the hook), unpacked by the app. |

## How the game reaches Steam

NotProton's bridge (`~/Library/Application Support/notproton/bridge`, used unchanged) holds:
- `lsteamclient.dll` and `lsteamclient.so`, Proton's Steam bridge ported to macOS;
- `steam.exe`, Proton's steam helper;
- Steam's Windows client DLLs.

`run` copies them into `C:\Program Files (x86)\Steam` in the prefix and puts that folder on
`WINEDLLPATH`. Our Wine's `wine-steamclient-lsteamclient.patch` then sends `steamclient64.dll`'s
exports to `lsteamclient.dll`, which does what NotProton's binary `ntdll` detour does for
CrossOver. The patch looks for `lsteamclient.dll` next to `steamclient64.dll`, because CrossOver's
Wine only loads a builtin it finds on the search path (CW HACK 20810). Without the bridge it
changes nothing, so the own-Steam prefix behaves as before.

Checked with `tests/windows_probe/steamclient_probe.c` through `run runinprefix` (2026-11-09):
`steamclient64.dll` redirected, 40 exports, `CreateInterface` jumps into `lsteamclient.dll`.

## Using it

1. Build the app with the tool: `build_native_app.py ... --game-tool finals=<macfinals-N.tar.gz>`.
   The app's main window, **Add to Steam** (top, or Game runtimes › SteamDriver), installs it into
   `~/Library/Application Support/Steam/compatibilitytools.d/notproton-steamdriver`. Then restart Steam.
2. macOS Steam has to see the game. On Rasmus's Mac (2026-11-09) it was moved from the MacFinals
   prefix's Steam library to macOS Steam's (`~/SteamProfiles/main/data/steamapps/common/The Finals`,
   the main Steam profile), its `appmanifest_2073850.acf` copied there, and a symlink left at the old
   place so the own-Steam setup and older runtimes still find it.
3. THE FINALS › Properties › Compatibility: turn on "Force the use of a specific Steam Play
   compatibility tool" and choose **SteamDriver**.
4. Options go in the launch options, in front of `%command%`, for example
   `STEAMDRIVER_MOUSE=hid %command%`. The full list is in `run`'s header.

Logs: `<library>/steamapps/compatdata/2073850/steamdriver-run.log` (the tool) and
`steamdriver-wine.log` (Wine and the game).
