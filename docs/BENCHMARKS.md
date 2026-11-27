# Benchmarks: fps, frame times and latency

How macSteam measures a stretch of play, what each number means and where it comes from, so the
same benchmark can be run on SteamDriver, on our CrossOver build (SteamDriver on CrossOver,
through NotProton) or on any other Wine, and the results compared. State of 2026-11-26.

## In short

1. macSteam → **Benchmarks…** → Record: **All games** or **Chosen games**. The launch option
   `STEAMDRIVER_BENCH=1` or `0` overrides it for one game.
2. Start the game with the SteamDriver tool. Recording applies from the next launch.
3. In the game, press the hotkey, **Control-Option-B** (⌃⌥B) unless you changed it, where the
   run should start and again where it should end. You hear Tink at the start, Pop at the end,
   and Basso if no game is recording.
4. The run appears in Benchmarks… with its numbers, a frame-time graph and the settings it ran
   with. You can name it ("upscaling on") and delete it.

macSteam has to be running for the hotkey (its window can be closed). The key goes to macSteam,
not to the game, and only while recording is on for some game.

**Changing the hotkey:** Benchmarks… → Hotkey → **Change…**, then press the new combo; **Reset**
goes back to ⌃⌥B. It is saved in `SteamDriver/benchmarks/hotkey` (one combo in the key-override
format, `ctrl+alt+b`). Because it is taken system-wide while recording is on, it needs Control,
Option or Command, unless it is a function key (F1–F20) on its own. If macOS uses the combo for
one of its own shortcuts, macSteam says so.

## What is recorded

Two sources, both written by the game's own process while it runs. Nothing reads game files or
attaches to the process.

### 1. Apple's Metal HUD log (every Wine, every renderer)

Metal itself has a performance HUD. With `MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1` it writes one
line to the process's **stderr** about every 0.7 s:

```
2026-11-26 07:56:09.858 Marvel-Win64-Shipping.exe[24782:4792235] metal-HUD: 99,5.59,329.78,6.25,0.02,12.50,0.02,…
                                                                          │   │    │      └ pairs: frame interval ms, GPU time ms
                                                                          │   └────┴ two values we don't use (not documented)
                                                                          └ frames so far
```

- There is one pair per frame since the previous line. The pair count matches the change in
  the frame counter to within one frame per line, and the intervals in a line add up to the
  time between lines.
- **Frame interval** is the time from one frame to the next as Metal sees it, which gives fps
  and frame times.
- **GPU time** is the GPU work of that frame.
- The HUD is drawn over the game unless `MTL_HUD_OPACITY=0`. Then it is invisible but still logs.
  `MTL_HUD_LOG_ENABLED=1` alone logs nothing (tested 2026-11-26), so the HUD must be enabled and
  hidden.
- The lines appear only in stderr, never in the unified log (`log show` / Console).

This is the same on D3DMetal and DXMT and on every Wine build, because Metal writes it, not the
renderer. It is the part to use when comparing builds.

**Where the lines end up:**

| Wine | File |
|---|---|
| SteamDriver tool | `steamapps/compatdata/<app id>/steamdriver-wine.log` |
| SteamDriver on CrossOver / NotProton's CrossOver | `steamapps/compatdata/<app id>/notproton-run.log` |
| Anything else | wherever that Wine sends the game's stderr |

### 2. The renderer's present log (latency; SteamDriver's builds only)

Each frame's **present-to-display latency**: how long a frame takes from its present call until
Metal reports it on the display. Both logs use the host clock (`CACurrentMediaTime`).

| Renderer | Switch | File | From → to |
|---|---|---|---|
| D3DMetal | `WINEMAC_INPUT_STATS=<prefix>` (SteamDriver's Mac driver, `wine-winemac-present-frames.patch`) | `<prefix>-frames-<pid>.csv`: `requested`, …, `presented` in ms after the `# t0` host time | the game asked for its drawable → on the display |
| DXMT | `DXMT_DISPLAY_LOG=<prefix>` (our DXMT, `tests/display_log/README.md`) | `<prefix>-<pid>.csv` (+ `.1.csv`, `.2.csv`): `request` and `presented` rows joined by pid + sequence | the game reached Metal's present call → `presentedTime` |

This is not input-to-photon latency. It covers the end of the frame (GPU work, compositor, the
wait for a refresh), not the time from your mouse movement to the frame that shows it. Frames
never shown are left out.

## What the SteamDriver tool does when recording

`environments/finals/compat-tool/run`, for a game whose app id is in
`~/Library/Application Support/SteamDriver/benchmarks/record` (a line `all` or one app id per
line, written by macSteam), or with `STEAMDRIVER_BENCH=1`:

- sets `MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1`, plus `MTL_HUD_OPACITY=0` unless the launch
  options already set `MTL_HUD_ENABLED`, in which case the visible HUD stays;
- turns on the renderer log for the renderer the game runs on: D3DMetal `WINEMAC_INPUT_STATS`,
  DXMT `DXMT_DISPLAY_LOG`;
- writes `SteamDriver/benchmarks/sessions/<app id>-<date>/session.json`: game, runtime,
  renderer, upscaling, the Wine log and its size at launch (so only this launch's lines are
  read), and the script's pid (a session is live while that process runs). `ended` is written
  when the game exits. The newest 10 sessions are kept.

## What the hotkey does

`app/Sources/BenchmarkRecorder.swift`, `Benchmarks.swift`:

1. **First press:**
   - Finds the game being played. That is the newest live SteamDriver session, or else a
     NotProton game whose `notproton-run.log` got a HUD line in the last 10 s.
   - Notes the time on two clocks (Unix time for the HUD lines, the host clock for the present
     logs) and the log's current size.
2. **Second press:** notes the times again, waits 1.5 s for the HUD's last line, then reads the
   log from the first press on.
3. **Which frames count:**
   - Each HUD line's last frame ends at the line's time, and each earlier frame ends one
     interval before the next.
   - Frames ending between the two presses count. The process with the most of them is the
     game (Steam's helpers and launchers can log too).
4. **Latency:** present-to-display times of presents between the two presses, from the session
   folder.
5. **Saving:** the run is saved to `SteamDriver/benchmarks/runs/<app id>/<date>.json`: the
   summary, every frame's interval and GPU time, and the settings.

## The numbers

| Number | Definition |
|---|---|
| Average fps | frames ÷ sum of frame intervals |
| 1% low / 0.1% low | 1000 ÷ the mean of the slowest 1% (0.1%) of frame intervals, at least one frame |
| Frame time p50 / p95 / p99 | percentiles of the frame intervals (linear between ranks) |
| Longest frame | the largest interval |
| Stutters | frames longer than twice the run's median. On a fixed-refresh display, one missed refresh is exactly twice the median and isn't counted; frames held longer are |
| GPU time p50 / p95 | percentiles of the per-frame GPU time |
| GPU-bound frames | share of frames whose GPU time is at least 90% of their interval |
| Present → display p50 / p95 / p99 | percentiles of the present-to-display latency, when a present log was recorded |

The display's refresh rate (macOS's maximum for the main screen) is stored with the run, because
frame times cluster at multiples of it under vsync.

## The same benchmark on our CrossOver build

SteamDriver on CrossOver runs the game through NotProton, so the SteamDriver tool's run script
doesn't run and macSteam's Record setting doesn't apply. Put the switches into the game's
launch options instead:

```
MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1 MTL_HUD_OPACITY=0 %command%
```

and for latency on our DXMT (the "SteamDriver renderer"), with a folder that exists:

```
DXMT_DISPLAY_LOG=/Users/<you>/bench/display MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1 MTL_HUD_OPACITY=0 %command%
```

- **Hotkey:** recording has to be on for at least one game in macSteam, because the hotkey is
  only registered then. macSteam then finds the game by its `notproton-run.log`.
- **Latency there:** macSteam doesn't know where DXMT's display log went, so the run has fps,
  frame times and GPU time only. Read the display log yourself with
  `scripts/summarize_display.py`.
- **Comparing builds:** compare the fps, frame-time and GPU numbers, which come from the HUD on
  both. Compare latency only between runs on the same renderer.

## Making runs comparable

- **Same stretch:** the same map, route and length (30–60 s or more), started at a recognisable
  moment. Use a practice range or a replay where the game has one.
- **Settings:** the same resolution and in-game settings, the same display, power adapter
  connected, other heavy apps closed.
- **Warm cache:** play a minute first, or discard the first run after an install or a runtime
  change. Shader compiling makes first runs stutter (D3DMetal's cache, `d3dm/<exe>/shaders.cache`).
- **Repeat:** three runs per setting, and compare the medians.
- **Name runs** after what changed ("DXMT, latency 1", "upscaling on").

## Limits

- **Recording isn't free.** The HUD and the present logs each add a little per-frame work, not
  measured yet. Leave recording off when you're not measuring, and keep it on for all runs you
  compare.
- **Anti-cheat:** the HUD is Apple's own Metal feature, loaded by Metal into the game. Whether a
  game's anti-cheat minds it hasn't been tested (THE FINALS included). Recording is off by
  default.
- **Timing:** the HUD's line times are stderr timestamps in milliseconds, so a frame's position
  in the window is accurate to about a millisecond. A frame per line may be missing or doubled
  (see above).
- **Frame-time edge case:** frame intervals as Metal sees them include frames presented and then
  replaced before a refresh. On a variable-refresh display that is what you see; on a
  fixed-refresh one, a frame time below the refresh period means the frame may never have been
  shown.
