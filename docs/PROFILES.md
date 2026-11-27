# Profiles with NotProton 1.2.7

macSteam's settings per game and for all games, adapted to what NotProton 1.2.7 (2026-11-19)
added for third-party plugins. This is the plan and its state; `docs/NOTPROTON.md` describes
what is written to NotProton today.

## What NotProton 1.2.7 changed for us

NotProton's commits since the copy macSteam was built against (1.2.4, 2026-11-08). Its format
document moved out of the repository; the parser (`dylib/feats/conf.c`, `compat_run.sh`) and the
fixtures (`dylib/tests/conf-fixtures`) are the reference now.

| Change | What it does | For macSteam |
|---|---|---|
| `apps = <Steam app id>` on a plugin row (up to 16) | The row only appears in those games' Compatibility page | Game profiles become rows restricted to their game, instead of rows that show in every game |
| "Show plugins made for other games" switch in Steam's panel | Overrides `apps` for players who want every row everywhere | Nothing to do |
| Plugins read live | Plugin rows change in Steam's panel without restarting Steam | No "Restart Steam?" prompt when only plugin rows changed (variants still need a restart) |
| Plugin page in NotProton's app | Lists plugin files and can move one to the Trash | A player can remove `plugins/steamdriver.conf` there; macSteam must not put it back on its own |
| `hide_crossover = 1` (needs `tools`) | Hides CrossOver's own options for tools that run a NotProton build | Not for us: NotProton only adopts tools with a `build` file, and SteamDriver's tool has none |
| Game Mode opt-out, CrossOver Preview 27.0.0.41303 | – | macSteam's driver exists for CrossOver 26.3 only, so its rows stay hidden on 27 |

## Profiles

A profile holds **renderer settings**, which differ between DX11 and DX12.

- **Global DX11**: for every DX11 game (SteamDriver DXMT). Optional; added with a button.
- **Global DX12**: for every DX12 game (D3DMetal, the SteamDriver game tools). Optional; added
  with a button. Either can exist alone.
- **Game profiles**: one per game, picked with search from the installed Steam games; it
  overrides the global profile for that game. CS2 and Deadlock keep their current profiles as
  game profiles.

Not part of a profile:
- the **mouse**: one global setting (the High-precision setting in macSteam, the "SteamDriver
  mouse" rows, `STEAMDRIVER_MOUSE` for the game tools);
- **key combo overrides** (macOS shortcuts that go to the game, `KeyOverrides.swift`): a Global
  list and one per game, whatever the Direct3D (`key-overrides/global.txt`, `app-<id>.txt`). A game
  gets Global's plus its own. Their own panel and sheet in macSteam; the game picker is the same
  as for game profiles.

### Renderer settings

| | DX11 (SteamDriver DXMT) | DX12 (D3DMetal) |
|---|---|---|
| Frame latency | `dxgi.maxFrameLatency` (CS2/Deadlock: 1) | – (Apple's, no setting) |
| Present pacing | `dxgi.presentPacing`, mode 3, target 1000 µs (CS2/Deadlock: on; input sampled later each frame, −16 % latency in play-tests) | – |
| Frame-rate cap | `d3d11.preferredMaxFrameRate` (0 = uncapped) | – |
| Shader compiler threads | `d3d11.shaderCompilerThreads` (CS2 4, Deadlock 8) | – |
| Native fullscreen, sharp presentation | `dxgi.nativeFullscreen`, `dxgi.sharpPresentation` | – |
| MetalFX upscaling | `steamdriver.upscaling` in `dxmt.conf` (DXMT ignores it; the SteamDriver tool reads it): DXMT's own DLSS stand-in, the game stays on DXMT with the settings above (runtime `macfinals-184` on) | `upscaling` in `d3dmetal.conf`: DLSS on Apple's MetalFX (`D3DM_ENABLE_METALFX`) |

Upscaling in either kind: D3DMetal presents an NVIDIA GPU to the game, and it works with the
SteamDriver tool only (not the NotProton rows). THE FINALS gets it like any game (Rasmus,
2026-11-25). The tool reads the game's own profile, else
Global DX11 for a game last detected as DX11, else Global DX12 (a DX11 game's first launch has
nothing detected yet, so it reads Global DX12).

A DX11 profile is a DXMT configuration file (`DXMT_CONFIG_FILE`), as the CS2 and Deadlock
profiles are today. Each DX11 profile's row in macSteam lists its settings, and **Settings…**
(`ProfileSettingsSheet`, `Profiles.Settings`) edits them in place: frame latency (1, 2, 3 or the
game's), late input sampling (present pacing mode 3, 1000 µs), frame-rate cap, shader compiler
threads and priority, sharp scaling, freeing shader memory. Only those keys' lines change;
comments and an `[exe]` section stay. They apply from the game's next launch. Reset goes back to
macSteam's file. `dxgi.nativeFullscreen` isn't shown: DXMT only uses it with a fixed canvas size,
which NotProton profiles leave out.

A DX12 profile is a `d3dmetal.conf` (2026-11-25), edited with the same **Settings…** button:
MetalFX upscaling (DLSS), off by default. It writes no row: the
SteamDriver tool's run script reads it at launch, from the game's own profile if the game has
one (a DX11 game profile has none, so off), else from Global DX12. The launch option
`STEAMDRIVER_UPSCALE=1` or `0` wins over both. Switching a game profile between DX11 and DX12
keeps the other kind's file aside (`dxmt.conf.dx11`, `d3dmetal.conf.dx12`).

### How a profile reaches a game (this round)

Through NotProton's rows, as now, since that is what NotProton supports:

- Global DX11 is a row without `apps` ("SteamDriver DX11 profile");
- a game profile is a row with `apps = <app id>`, so it only shows in that game;
- the rows are switched on per game in Steam's Compatibility page; a game profile's row is
  written after the global ones, so its `env` wins where both set the same name;
- the SteamDriver tool (D3DMetal) reads DX12 profiles itself (`d3dmetal.conf`, above), and
  the key combo lists.

Where the files live: `~/Library/Application Support/SteamDriver/profiles/<id>/`
(`global-dx11`, `global-dx12`, `app-<app id>`): `profile.json` (name, DX11/DX12, app id),
`dxmt.conf` (DX11) or `d3dmetal.conf` (DX12).

### Fixes for NotProton 1.2.7 (done 2026-11-24)

1. Profile rows with `apps =` (game profiles) and without (Global DX11); buttons in macSteam's
   Profiles panel: + Global DX11, + Global DX12, + Game Profile… (searchable: installed Steam
   games, games started with a game tool, or a typed app id; DX11/DX12, preset for known games).
   `Profiles.swift`, `NotProtonExtras.profileRows`. Only DX11 profiles become rows; at most 16.
2. No Steam restart prompt when only plugin rows changed (`NotProton.Change`; restart only for
   variants and compatibility tools).
3. A `plugins/steamdriver.conf` the player removed in NotProton's plugin page stays removed
   (`NotProton.rowsRemoved`): refresh and profile changes leave it out, and the status panel
   offers Put Rows Back.

Migration (`Profiles.migrate`): `profiles/cs2.conf`/`deadlock.conf` became `app-730`/`app-1422450`;
`key-overrides/games/` became `games/`. The key combo lists stay in `key-overrides/` (builds
dev19–21 kept them in the profiles as `keys.txt`; those move back, Global DX11's and DX12's into
Global).

## To do

- **DX11/DX12 detection** (done 2026-11-24, SteamDriver tool only). After each game exits, the
  tool's run script reads which Direct3D that launch loaded from Wine's own DLL-load log
  (`+loaddll`, on by default; no game file is read): `d3d12.dll` means DX12, else `d3d11.dll`
  DX11, `d3d9.dll` alone nothing. It is kept as `direct3d=dx11|dx12`, line 4 of
  `SteamDriver/games/<app id>`. macSteam (`Profiles.detected`) shows it in the game pickers,
  presets a new game profile to it, and flags a game profile of the other kind with a "Switch to
  DX12/DX11" button (`Profiles.setKind`, 2026-11-25). The switch is never automatic, because a game
  can run either way (KovaaK's with `-dx12`); switching away from DX11 keeps the DX11 settings in
  `dxmt.conf.dx11`, and switching back restores them. Still to do: the
  tool applying Global DX11 or DX12 by it (today neither has a setting the tool uses: D3DMetal has
  none, and DX11 games run on D3DMetal there, not DXMT), and detection for SteamDriver on
  CrossOver, where NotProton runs the game and our script doesn't.
- **MetalFX upscaling.** Full write-up, including why Deadlock gets no DLSS: [`UPSCALING.md`](UPSCALING.md).
  - DX11: Recall's DXMT MetalFX patch (`patches/dxmt-metalfx-upscaling.patch`,
    `docs/METALFX.md`) exists, but it was made for Overwatch's DLSS option and nothing in the
    NotProton rows turns it on.
  - DX12 (done, 2026-11-25): D3DMetal's DLSS on MetalFX, the DX12 profile setting above
    (runtime `macfinals-176` on; `environments/finals/README.md`, `add_nvngx.py`). Off by
    default; D3DMetal then presents an NVIDIA GPU. Works in ARC Raiders and THE FINALS (2026-11-25).
  - DX11 (2026-11-25): DXMT's own DLSS stand-in (`add_dxmt_dlss.py`, runtime `macfinals-184`),
    so the game stays on DXMT. Apple's DLSS on D3DMetal was tried first; Deadlock never offered
    DLSS there ("UPSCALER_NVIDIA_DLSS is not available") with every driver check passing. Not
    yet confirmed in a game.
- **Keyboard overrides in NotProton games.** The driver support
  (`environments/finals/patches/wine-macdrv-game-shortcuts.patch`) is in the THE FINALS
  runtime only; NotProton games need it ported to the driver built for NotProton's CrossOver
  26.3 (`Resources/NotProton/<build>/winemac.so`).
- **Settings without rows.** Later, the driver could read the profiles itself (by Steam app id),
  so nothing has to be switched on in Steam.
