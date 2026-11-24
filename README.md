# SteamDriver

**Windows games from the macOS Steam app on Apple Silicon: a better mouse and display for every
game, and a Wine of their own for games that need one.**

SteamDriver is a fork of [MacVolvo](https://github.com/haeli05/MacVolvo), which is built on
[Recall](https://github.com/AsherJN/recall). It works together with
[NotProton](https://github.com/NotProtonNot/NotProton), which turns on Steam Play in the macOS
Steam app. The app is still called **MacVolvo** inside; the rename hasn't happened yet.

> **Status: in development.** This repository has no releases of its own; build it from source.
> THE FINALS runs from the macOS Steam app on the developer's Mac. Expect rough edges.

## What it does

### For every game: options in Steam's Compatibility settings

With NotProton installed, the app adds options to each game's **Properties › Compatibility** page
in macOS Steam, through NotProton's own extension points. It replaces nothing of NotProton's.

- **MacVolvo driver**: Recall's Mac driver for Wine, adding:
  - raw mouse input, read up to 2,000 times a second;
  - the **High-precision** mouse, which reads the mouse directly;
  - precise or classic timing;
  - fullscreen and display fixes.
- **MacVolvo renderer**: MacVolvo's DXMT (Direct3D 11 to Metal), or Apple's D3DMetal from your own
  Game Porting Toolkit.
- **Profiles**: Global DX11, Global DX12 and per-game profiles with renderer settings and key combo overrides ([docs/PROFILES.md](docs/PROFILES.md)).

Details: [docs/NOTPROTON.md](docs/NOTPROTON.md).

### For games that need more: a Wine environment of their own

Some games need Wine changes that NotProton's CrossOver runner can't take. Those games get their
own environment in [`environments/`](environments/): one main Wine, plus a folder for each game
that needs its own. A game's environment ships as a **Steam Play compatibility tool**, which the
app installs into macOS Steam and you select per game.

| Environment | For | Graphics |
|---|---|---|
| [`main`](environments/main/) | Counter-Strike 2, Deadlock, everything else | DXMT (Direct3D 11) |
| [`finals`](environments/finals/) | **THE FINALS** | Apple D3DMetal (Direct3D 12) |

**SteamDriver**, the compatibility tool, is the finals environment's Wine, offered for every game
since 2026-11-24 (it was "SteamDriver: THE FINALS"; CS2 and Deadlock may not work on it yet, and
"SteamDriver on CrossOver", the options on NotProton's CrossOver above, stays for them). It runs
CrossOver 26.3 Wine built from source with these additions:
- an in-process fix for Rosetta's slow handling of the game's direct system calls (~32,000 a
  second);
- a fix for missing character limbs in Apple's D3DMetal;
- the same mouse driver as above.

The game reaches the Steam client through NotProton's Steam bridge, which our Wine loads itself.
Nothing is patched from outside the game, and macOS developer mode isn't needed. Details:
[environments/finals/compat-tool](environments/finals/compat-tool/README.md).

The plan is to build environments ahead of time and download them per game. For now the app
build bundles them.

## Using it

| You need | |
|---|---|
| Mac | Apple Silicon (M1 or newer) |
| macOS | macOS Tahoe 26.5 or later |
| Also | the macOS Steam app, [NotProton](https://github.com/NotProtonNot/NotProton) (install it and open it once), Rosetta |
| For THE FINALS and D3DMetal | your own copy of Apple's [Game Porting Toolkit 4](https://developer.apple.com/games/game-porting-toolkit/) (macSteam includes no D3DMetal), about 90 GB for THE FINALS |

1. Build the app (below) and open it.
2. **For every game:** choose **Add to Steam**. Then, in a game's Properties › Compatibility, keep
   NotProton's CrossOver, choose the MacVolvo driver and renderer under Developer, and turn on
   MacVolvo mouse.
3. **Apple's Game Porting Toolkit** (for SteamDriver, and D3DMetal in NotProton games):
   1. Download Game Porting Toolkit 4 from
      [Apple](https://developer.apple.com/games/game-porting-toolkit/) (free Apple developer
      account).
   2. Open the Game Porting Toolkit, then open **Evaluation environment for Windows games** in it
      (macOS shows Apple's license first).
   3. In macSteam, under **Renderers**, choose **Show Folder** and copy the image's **redist**
      folder into it (`~/Library/Application Support/SteamDriver/GPTK`).

   macSteam checks the files (every module there, D3DMetal 4, Apple's signature) and shows a ✓
   when they're ready. A file that isn't Apple's is never used.
4. **SteamDriver:** choose **Add to Steam** (at the top, or under **Game runtimes**) and let the app
   restart Steam. In a game's Properties › Compatibility, turn on "Force the use of a specific Steam
   Play compatibility tool" and choose **SteamDriver**. Options go in the launch
   options, for example `STEAMDRIVER_MOUSE=hid %command%` for the High-precision mouse.

Advanced › MacVolvo's own Steam still offers the original setup, for troubleshooting: Windows
Steam inside the app's own Wine.

## Anti-cheat

The games run unmodified through Wine, the way Linux players use Proton. Nothing hides Wine
from a game or its anti-cheat.
- **Counter-Strike 2 and Deadlock** use VAC.
- **THE FINALS** uses its own anti-cheat. In a new Wine prefix its first launch can show "issue
  with starting Denuvo Anti-Cheat"; the second launch goes in.

None of these publishers support playing on macOS this way. Use an account you're prepared to take
that risk with.

## Known issues

- **THE FINALS:** short freezes (0.3–2 s), mostly while loading and in the first minutes of play.
  The cause isn't known yet; shader compilation is ruled out. See
  [PROFILING.md](environments/finals/tests/finals_bisect/PROFILING.md).
- **Counter-Strike 2 and Deadlock:** see the [v1.1 release notes](docs/releases/v1.1.md) from
  MacVolvo. They still apply to the main environment.

## Building from source

- **The app:**

  ```sh
  DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 scripts/build_native_app.py \
    --identity - --runtime-archive <main runtime .tar.gz> \
    --game-tool finals=<macfinals-N.tar.gz> --output runtime/build/SteamDriver-N.app
  ```

  Leave out `--game-tool` for an app without THE FINALS.
- **The main environment** (the main runtime, DXMT, the Mac driver): [docs/BUILDING.md](docs/BUILDING.md).
- **The finals environment** (its Wine, the d3d12 layer, the Rosetta hook, the runtime archive):
  [environments/finals/README.md](environments/finals/README.md). It needs MinGW-w64 GCC.
- **What broke while this was put together:**
  - [environments/LESSONS.md](environments/LESSONS.md)
  - [environments/finals/compat-tool/LESSONS.md](environments/finals/compat-tool/LESSONS.md)
- **Architecture:** [app/ARCHITECTURE.md](app/ARCHITECTURE.md).

## Credits and license

SteamDriver builds on:
- [MacVolvo](https://github.com/haeli05/MacVolvo) by haeli05;
- [Recall](https://github.com/AsherJN/recall) by Joshua Asher Nelson;
- Wine from CodeWeavers' published CrossOver sources;
- DXMT by Feifan He through NerRobDog's fork;
- Soju's runtime build recipe;
- the design of THE FINALS' Rosetta hook, which draws on
  [rosettax87_jit](https://github.com/Lifeisawful/rosettax87_jit) by Lifeisawful.

THE FINALS' route uses NotProton's Steam bridge (GPL-3.0; Valve's lsteamclient and steam-helper
from Proton) unchanged, from your NotProton install. Apple's D3DMetal comes from your own Game
Porting Toolkit (`SteamDriver/GPTK`); the THE FINALS runtime only links to it
(`environments/finals/scripts/unbundle_gptk.py`), and macSteam ships no D3DMetal (the main DXMT runtime from Recall still carries Apple's `libd3dshared.dylib`).

This route was prompted by seeing
[zzoko's compatibility setup run THE FINALS](https://www.youtube.com/watch?v=_2PVUeov3_A).
This was promted by it having low fps aswell as the requirment with developer mode on.

The original code is licensed under [Apache 2.0](LICENSE). Dependencies and patches keep their own
licenses: see the [third-party notices](THIRD_PARTY_NOTICES.md) and [NOTICE](NOTICE).

SteamDriver is an independent community project, not affiliated with or endorsed by Valve
Corporation, Embark Studios, Apple, CodeWeavers, NotProton's authors or Blizzard Entertainment.
Counter-Strike, Deadlock, Steam, THE FINALS, Mac, CrossOver and other product names are trademarks
of their respective owners.
