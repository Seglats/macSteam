# MacVolvo with NotProton

[NotProton](https://github.com/NotProtonNot/NotProton) turns on Steam Play in the macOS Steam
app. It runs Windows games in a copy of CrossOver (a "runner"), with its Steam bridge patched
into the runner's `ntdll`. MacVolvo replaces none of it: no compatibility tool, no runner, no
CrossOver. It adds **extra options to each game's Compatibility settings in Steam**, through
NotProton's own extension points. The main window (`CompanionView`) adds, updates and removes
them, and shows whether the High-precision mouse works. It installs no Steam and launches nothing.

## What Add to Steam writes

| Path (under `~/Library/Application Support`) | What |
|---|---|
| `MacVolvo NotProton/<build>/winemac.so` | MacVolvo's Mac driver for that CrossOver build |
| `MacVolvo NotProton/<build>/dxmt/` | MacVolvo DXMT, bundled with the app (from its runtime) |
| `SteamDriver/GPTK/` | your own Game Porting Toolkit 4: the `redist` folder you copied in from Apple's image |
| `SteamDriver/profiles/<id>/` | the profiles you made: Global DX11, Global DX12, `app-<id>` ([PROFILES.md](PROFILES.md)) |
| `notproton/variants/macvolvo.conf` | two dropdowns, **MacVolvo driver** and **MacVolvo renderer**; choices from both combine |
| `notproton/plugins/macvolvo.conf` | rows in each game's Compatibility page (Developer section) |

The variants:

| Group | Variant | Lays | Over the runner's |
|---|---|---|---|
| MacVolvo driver | MacVolvo driver | `winemac.so` (needs `build`) | `lib/wine/x86_64-unix` |
| MacVolvo renderer | MacVolvo DXMT | `dxmt/<arch>/…` and `wine/x86_64-windows/winemetal.dll` (needs `build`) | `lib` |
| MacVolvo renderer | Apple D3DMetal *version* | Apple's `redist/lib`, unchanged | `lib64/apple_gptk` |

- **MacVolvo DXMT** is Recall's DXMT. Its `winemetal.so` finds the Mac driver by install name,
  so choose it together with MacVolvo driver. CrossOver also loads a copy of `winemetal.dll`
  from `lib/wine`, and its PE half has to match the Unix half, so that copy is replaced too.
- **Apple D3DMetal:** macSteam ships no D3DMetal and installs nothing of Apple's itself. The
  player downloads Game Porting Toolkit 4 from Apple, opens "Evaluation environment for Windows
  games" (macOS shows Apple's license) and copies its `redist` folder into `SteamDriver/GPTK`
  (**Show Folder** under Renderers; `redist/lib` or `lib`'s contents work too, `gptkLib` finds
  them). `gptkStatus` checks the files (the required modules, D3DMetal 4, Apple's signature on
  every native file; cached until they change) before the "Apple D3DMetal" variant offers them
  or the SteamDriver tool uses them. The variant lays them, unchanged, over the runner's
  `lib64/apple_gptk`. NotProton's CrossOver has a D3DMetal of its own; macSteam no longer points
  to it.
- **Graphics backend:** choose NotProton's matching backend for the game (DXMT or D3DMetal).

The rows:

| Row | Sets for that game |
|---|---|
| **MacVolvo mouse** | `WINEMAC_MOUSELOOK=*;!steam.exe;…`, `WINEMAC_INPUT_VARIANT=precise`, `WINEMAC_RAW_STATUS=…/hid-status.json` |
| ↳ **High-precision** | `WINEMAC_RAW_SOURCE=hid`, `WINEMAC_RAW_REQUEST=1` |
| ↳ **Classic timing** | `WINEMAC_INPUT_VARIANT=classic` (a later row's `env` wins) |
| ↳ **Mouse updates** (slider) | `NOTPROTON_ENV_WINEMAC_MOUSELOOK_RAW_INTERVAL_US`; the default writes nothing, which is the driver's own 1 ms |

| **SteamDriver DX11 profile** (Global DX11) | `DXMT_CONFIG_FILE=Z:\…\profiles\global-dx11\dxmt.conf` |
| **SteamDriver profile: *game*** (a DX11 game profile, `apps = <id>`, after the global row so it wins) | `DXMT_CONFIG_FILE=Z:\…\profiles\app-<id>\dxmt.conf` |

**Profiles** ([PROFILES.md](PROFILES.md)): only DX11 profiles become rows (a DX12 profile has no
renderer setting yet). CS2 and Deadlock keep their row ids (`NOTPROTON_MACVOLVO_PROFILE_CS2`,
`_DEADLOCK`), so games keep them switched on. At most 16 profile rows: NotProton shows 32 rows
across all plugin files.
- `scripts/build_native_app.py` `notproton_profiles` generates the settings from the runtime's DXMT
  renderer profile: `cs2.conf` and `deadlock.conf` (with their `[exe]` section) and `dx11.conf`
  (no section, so DXMT applies it to every program) for Global DX11 and other games. The
  canvas-size lines are dropped (NotProton's Steam has no resolution setting), and CS2 gets 4
  shader compiler threads.
- The **Deadlock** profile also switches `boot.vcfg`'s `DefaultRenderSystemOption` from
  `-vulkan` to `-dx11` in every Steam library (`libraryfolders.vdf`), keeping
  `boot.vcfg.macvolvo-backup`. A saved Vulkan choice bypasses DXMT and D3DMetal. It's
  re-applied each time MacVolvo opens.
- `-novid` stays up to you, in Steam's launch options.

Every row names the tools it's for (`tools = notproton-26.3`), so FEX runners never show it.

**Using it:**
1. In a game's Properties › Compatibility, keep NotProton's CrossOver.
2. Under Developer, choose **MacVolvo** as the runner variant.
3. Turn on **MacVolvo mouse**, with High-precision if you want it.

NotProton builds a runner copy with the driver on that game's first launch.

## Why it's built this way

- **No full runtime:** MacVolvo's runtime has Recall's own `ntdll`, so it can't carry
  NotProton's Steam bridge. Variants may not replace `ntdll` or `lsteamclient`.
- **Only the Mac driver.** It's compiled from the same CrossOver 26.3 sources as runner
  `26.3.0.39832`:
  - It imports exactly the same 35 `ntdll` and 72 `win32u` symbols as CodeWeavers' own
    `winemac.so`.
  - The patches change only the Unix side, never `dllmain.c` or `unixlib.h`.
  - The runner's Wine loader has `disable-library-validation`.
- **Plugins, not a wrapper:** in NotProton's `open` launch path, only plugin `env` lines,
  `NOTPROTON_ENV_` options and an allowlist reach the game. A wrapper's own variables would
  not. Plugin `env` lines win over `NOTPROTON_ENV_` options, so no row sets the update rate.
- **Ownership:** both files start with MacVolvo's marker; NotProton never writes to those
  folders. Add refuses before writing anything if a file there isn't MacVolvo's, and Remove
  deletes only marked files. The first version made a Steam tool `notproton-macvolvo-*`;
  Add and Remove delete it.

## The High-precision mouse and Input Monitoring

- **Usually already allowed:** NotProton asks macOS to let Steam control the computer, for Steam
  Input (`input_access.c`, `CGRequestPostEventAccess`). That is Device Control and Data Access
  (Accessibility before macOS 27), and it also lets Steam read input. So `IOHIDCheckAccess`
  answers granted in the game, no prompt shows, and Steam never appears under Input Monitoring.
  This was seen on 2026-11-07: High-precision worked in FPSAimTrainer without that entry.
- **Who needs the permission:** NotProton usually runs the game's Wine directly from its
  script, a child of Steam, so macOS gives Input Monitoring to **Steam**. MacVolvo never asks
  for itself in this mode.
- **No button can ask for Steam:** macOS only adds an app to Input Monitoring after it asks, and
  Steam's real bundle (`Steam.AppBundle/Steam`, the responsible process for games) is a folder
  Settings' + can't pick. The first game started with High-precision asks.
- **The prompt:** with `WINEMAC_RAW_REQUEST=1` the driver calls `IOHIDRequestAccess` while the
  answer is undecided, so macOS shows the prompt for Steam. A new answer applies from the next
  launch.
- **The report:** with `WINEMAC_RAW_STATUS` the driver writes `hid-status.json`: access, open
  result, mice open, whether one has reported, process and time. It writes it whenever one of
  those changes (`cocoa_app.m` `hid_write_status`). The window reads it every few seconds and
  says "High-precision mouse is working" or "Steam isn’t allowed to read your mouse".

## Supporting another CrossOver build

1. Build `winemac.so` against that build's sources (see [UPSTREAM-RECALL.md](UPSTREAM-RECALL.md)).
2. Add the build to `NOTPROTON_BUILDS` in `scripts/build_native_app.py`.
3. Check its imports against that runner's `ntdll.so` and `win32u.so`.

To check the files the way NotProton reads them, compile NotProton's `dylib/tests/confdump.c`
with `dylib/feats/conf.c`. Run it on the folder holding `variants/` and `plugins/`; it must
print every record with no `!` refusal lines. `tests/test_notproton_options.py` covers add,
update, refusal and remove.

## MacVolvo's own Steam

Advanced › MacVolvo's own Steam switches the window to the old setup (`SetupView`), for
troubleshooting: Windows Steam in MacVolvo's own Wine environment, with Open Steam and no game
picker. The **NotProton** tile there switches back.

## Updating and restarting Steam

- **In place:** a newer MacVolvo rewrites its files over the old ones (`NotProton.refresh`, at
  launch and every few seconds while the window is open). Nothing has to be removed first.
  NotProton stamps each variant runner with its files' sizes and times, so it rebuilds it on the
  next launch.
- **Restart:** Steam reads the plugin rows live (NotProton 1.2.7) but the variants, and the
  compatibility tools, only when it starts. So the window asks to restart Steam
  (`NotProton.restartSteam`) only after a change to the variants (`NotProton.Change.restart`):
  `steam://exit` as NotProton does, up to 30 seconds, then it opens NotProton's patched Steam app
  (its `steamAppPath` setting, else `/Applications/Steam.app`). It never forces Steam to quit.
  Profile changes rewrite only the rows (`NotProton.writeRows`); a new driver or DXMT needs
  nothing, since NotProton rebuilds its runner copy at the next launch.
- **Rows removed in NotProton's plugin page** (`plugins/steamdriver.conf` moved to the Trash)
  stay removed: refresh and profile changes leave them out (`NotProton.rowsRemoved`), and the
  window offers Put Rows Back.

## Separate compatibility tool: not possible

NotProton's Compatibility page shows its options, and so MacVolvo's rows, only for the tools in
`notproton/tools`. The NotProton app rebuilds that list from the CrossOver runners it installed,
so any other tool (the first version's `notproton-macvolvo-*`) gets none of them. For every game,
save the MacVolvo choices as NotProton's defaults for that CrossOver version instead.

## Caveats

- Not yet tried in a game through Steam.
