# Microstutter of moving targets, and Game Mode for every Wine program (2026-11-12/13)

Runtime `macfinals-106` is installed in the Steam tool. Results first, details below.

| Topic | Result |
|---|---|
| Where the micro-jumps come from | The macOS compositor (WindowServer) sometimes shows a finished frame one refresh late. The game, D3DMetal and the GPU deliver on time. |
| DXMT (D3D11) instead of D3DMetal (D3D12) | Same holds, same cause. D3DMetal is the faster path (half the GPU time, 0.8 ms less to the screen). |
| VSync | Removes almost all holds (99.7 % of frames exactly one refresh), at the cost of queueing. |
| macOS Game Mode | Now on for every Wine program. Frames reach the screen 1.4 ms sooner; held frames 1.9 % → 1.5 %. On by default. |
| Direct to display (bypassing the compositor) | Refused for this monitor ("display rejected"), for native apps too. Not available. |
| Inherited game-app bug | Recall's game-app start crashed every launch on this runtime. Fixed in `macfinals-106`. |

## The problem

In THE FINALS and KovaaK's, turning the camera looks smooth, but moving enemies and targets make
small jumps, as if animated at a lower rate. KovaaK's has no anti-cheat, so it is the test game:
the same simple 3D shapes (ovals) moving steadily, frame cap **130**, VSync off unless stated, on
the external LG 38GN950 (3840×1600, fixed 160 Hz). (Some earlier notes said 140: the cap was set
back to 130 while swapping settings, so every measurement here ran at 130.)

## 1. The game and D3DMetal deliver on time

Per-frame file from the Mac driver (`present_frames.py`, about 110 s of play at 130 FPS):

| Step | Median | Note |
|---|---|---|
| Game frame interval | 7.69 ms | almost exactly that every frame |
| Wait for a drawable | 0.02 ms | D3DMetal never makes the game wait |
| Drawable acquired → D3DMetal presents it | 0.37 ms | D3DMetal takes the drawable at the last moment |
| Present → on screen | 5.8 ms | where all the variation is |

All late frames lost their time **after** D3DMetal's present. 

## 2. GPU or compositor? The compositor

Instruments recordings with the GPU and Metal Application instruments (`gpu_frames.py`), 5 s
each in the scenario. Each frame followed from present request → GPU done → WindowServer's
composite → on screen:

| | D3DMetal | DXMT (`-dx11`) | D3DMetal + Game Mode |
|---|---|---|---|
| GPU work per frame | 2.1 ms | 4.0 ms | 2.0 ms |
| GPU done → composited | 1.0 ms (p99 4.5) | 1.0 ms (p99 4.4) | **0.07 ms** (p99 2.7) |
| Composite → on screen | 2.3 ms | 2.3 ms | 2.0 ms |
| Present request → on screen | 5.5 ms | 6.3 ms | **4.1 ms** |
| Held frames (≥ 1.4 × median on screen) | 1.9 % | 2.3 % | **1.5 %** |

- The GPU is about 27 % busy; it has plenty of headroom.
- A held frame is the frame *before* a late one. In all three, most late frames are late at the
  compositor: WindowServer picks the finished frame up 3–8 ms late instead of at once.
- The game itself was late for only 1–2 holds per recording.
- WindowServer can't be changed or prioritised (System Integrity Protection; its threads already
  run real-time). What we can change is how our frames reach it.

Earlier recordings (Display instrument only) of VSync and of the direct-presentation overlay
(`WINEMAC_DIRECT_PRESENT=1`, from when the cap was 160): VSync on gave 99.7 % of frames exactly one
refresh; the overlay with VSync off 89 % (held 1.1 %) against 70 % plain (held 13.6 %). A native
test program showed direct presentation is refused on this monitor, for any app, even in a
fullscreen Space, so the overlay only changes how the compositor treats the window. Its open
problems (sometimes 60 FPS only, alt-tab) were left, since Game Mode does more.

## 3. Game Mode for every Wine program

macOS turns Game Mode on only for a process whose executable is the main executable of an app
bundle that declares itself a game. Recall's Wine already started named programs from such a
bundle (`WINE_GAME_MODE=Overwatch.exe` → `<dll_dir>/game-mode/Overwatch.app`). Our Wine runs games
only, so now every program gets one:

- `wine-ntdll-game-mode-all.patch`: `WINE_GAME_MODE=*` lists every program; a program without a
  bundle of its own starts from `lib/wine/game-mode/Game.app` ("SteamDriver Game",
  `app.steamdriver.game`). THE FINALS keeps its own `Discovery.app`.
- `add_nt_hook.py` adds `Game.app` next to `Discovery.app` (`GAME_APPS`, same signing).
- `compat-tool/run` sets `WINE_GAME_MODE='*'` by default; `STEAMDRIVER_GAME_MODE=0` turns it off.

**Bug found on the way (`wine-ntdll-game-mode-loader-check.patch`).** For a 64-bit program, ntdll
first tries the other architecture's loader, `lib/wine/i386-unix/wine`, which this runtime doesn't
have; that attempt normally fails quietly and ntdll falls back to its own loader. Recall's
`game_mode_exec` didn't check that the loader exists: it started the game app pointing at the
missing one, which then found no `ntdll.so` and exited ("could not load ntdll.so" in
`steamdriver-wine.log`, every launch on `macfinals-104`). It now starts the game app only in place
of a loader that is there.

**Verified.**
- `game_mode_check.py` on `macfinals-106` (stand-in window, no Steam): Game Mode came on about 8 s
  after the window went fullscreen and stayed on; the stand-in was frontmost. Wine's borderless
  fullscreen is enough; a macOS fullscreen Space is not needed.
- KovaaK's on `macfinals-106`: the game process runs as "wine" (the game app's executable)
  instead of "FPSAimTrainer", and the compositor numbers in section 2 changed as expected.
- **Not yet tried with THE FINALS** and its anti-cheat: the game's process now runs from
  `Discovery.app`. If its start misbehaves, try `STEAMDRIVER_GAME_MODE=0` first.

## 4. Builds

| Runtime | Adds |
|---|---|
| `macfinals-98` | `wine-winemac-present-stats.patch`: present_* fields in `WINEMAC_INPUT_STATS` |
| `macfinals-100` | `wine-winemac-present-latency.patch`: drawable → screen delay and the game's frame interval |
| `macfinals-102` | `wine-winemac-present-frames.patch`: per-frame file `<prefix>-frames-<pid>.csv` |
| `macfinals-104` | `wine-ntdll-game-mode-all.patch`, `Game.app` (crashed every launch: the bug above) |
| `macfinals-106` | `wine-ntdll-game-mode-loader-check.patch`. **Installed.** `-104` and `-102` kept beside it. |

The per-frame file wraps each drawable's present methods in memory (a subclass of the drawable's
own class, as key-value observing does) to time D3DMetal's present. Nothing in D3DMetal is
modified, and it is only active with `WINEMAC_INPUT_STATS` set.

## 5. Tools (`tests/finals_bisect/`)

| Tool | Does |
|---|---|
| `game_perf_trace.sh LABEL [SECONDS]` | Instruments recording of KovaaK's (refuses THE FINALS). Waits 30 s, records 15 s by default; Glass sound at start, Hero when the time is up. `GPU=1` adds the GPU instrument, `FULL=1` Metal Application. |
| `frame_pacing.py TRACE` | On-screen times and direct-to-display status from a recording |
| `gpu_frames.py TRACE...` | Each frame from present to screen, and which step made held frames late (needs `GPU=1 FULL=1`) |
| `present_stats.py --newest PREFIX` | Per-second present statistics from `WINEMAC_INPUT_STATS` |
| `present_frames.py --newest PREFIX` | The per-frame file: where late frames lost their time |
| `game_mode_check.py [RUNTIME] [--off]` | Does macOS give a runtime's programs Game Mode? Stand-in window, no Steam; takes over the screen for about 25 s |

Recording at all disturbs the measurement a little; with holds this rare (about 2 a second),
those from the recording itself are hard to tell apart.

## 6. Open

- The remaining 1.5 % of holds, still at the compositor. VSync together with Game Mode is
  untested and may now cost less delay than before.
- THE FINALS with Game Mode (anti-cheat untested).
- The direct-presentation overlay's open problems, only if it is ever needed again.
