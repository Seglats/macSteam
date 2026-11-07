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
| `notproton/variants/macvolvo.conf` | runner variant "MacVolvo" per build: lays the driver over `lib/wine/x86_64-unix` and names its `build` |
| `notproton/plugins/macvolvo.conf` | rows in each game's Compatibility page (Developer section) |

The rows:

| Row | Sets for that game |
|---|---|
| **MacVolvo mouse** | `WINEMAC_MOUSELOOK=*;!steam.exe;…`, `WINEMAC_INPUT_VARIANT=precise`, `WINEMAC_RAW_STATUS=…/hid-status.json` |
| ↳ **High-precision** | `WINEMAC_RAW_SOURCE=hid`, `WINEMAC_RAW_REQUEST=1` |
| ↳ **Classic timing** | `WINEMAC_INPUT_VARIANT=classic` (a later row's `env` wins) |
| ↳ **Mouse updates** (slider) | `NOTPROTON_ENV_WINEMAC_MOUSELOOK_RAW_INTERVAL_US`; the default writes nothing, which is the driver's own 1 ms |

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

- **Who needs the permission:** NotProton usually runs the game's Wine directly from its
  script, a child of Steam, so macOS gives Input Monitoring to **Steam**. MacVolvo never asks
  for itself in this mode.
- **The prompt:** with `WINEMAC_RAW_REQUEST=1` the driver calls `IOHIDRequestAccess` while the
  answer is undecided, so macOS shows the prompt for Steam. A new answer applies from the next
  launch.
- **The report:** with `WINEMAC_RAW_STATUS` the driver writes `hid-status.json`: access, open
  result, mice open, whether one has reported, process and time. It writes it whenever one of
  those changes (`cocoa_app.m` `hid_write_status`). The window reads it every few seconds and
  says "High-precision mouse is working" or "Steam needs Input Monitoring".

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

## Caveats

- Steam reads plugin and variant files when it starts, so restart it after Add or Remove.
- Not yet tried in a game through Steam.
