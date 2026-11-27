# Headphone surround (experimental)

Games' surround sound turned into 3D sound for headphones, in games that run with the
SteamDriver compatibility tool. State of 2026-11-27, runtime `macfinals-205`.

## Why

A Windows game asks the audio device how many speakers it has. On a Mac with headphones (or any
stereo output) Wine's Core Audio driver says two, so the game mixes its sound to plain stereo
itself. Even a game that plays 7.1 gets it folded down by macOS without any 3D processing. Either
way a sound behind you sounds the same as one in front. On Windows, Dolby Atmos for Headphones,
Windows Sonic, or HeSuVi on Equalizer APO fill this gap. On Linux, a PipeWire spatializer does.

## What it does

Two patches to `dlls/winecoreaudio.drv/coreaudio.c`,
`environments/finals/patches/wine-coreaudio-headphone-surround.patch` and
`wine-coreaudio-headphone-surround-fitted.patch` after it. They do nothing unless
`WINE_HEADPHONE_SURROUND=1` is set:

1. **The device is offered as 7.1.** A render device with one or two output channels reports a
   7.1 mix format (8 channels, `KSAUDIO_SPEAKER_7POINT1_SURROUND`, 0x63F) instead of stereo. Games,
   XAudio2 (FAudio) and DirectSound then mix to 7.1.
2. **Each multichannel stream is rendered for two ears.** For a stream with more than two channels,
   in 16-, 24- or 32-bit integer or 32-bit float, on a device with at most two channels, the audio
   unit gets stereo float. The render callback turns every frame into a left and a right sample:
   - **Each speaker is heard through measured head responses** (since `macfinals-205`): MIT's KEMAR
     dummy head, reshaped with settings fitted to Dolby Atmos for Headphones
     ([below](#the-fitted-model-2026-11-27)). Each speaker position has a finished filter per ear,
     304 taps at 48 kHz, in `surround_ir.h`, which the fitted patch adds. At other rates the filters
     are resampled when the stream opens (windowed sinc).
   - Positions, as fitted:
     - fronts ±42.6° at −18° elevation, center 0°, front-of-center ±21.3°;
     - sides ±85.3° at +17° with back speakers present;
     - backs ±136.8° at +3° with side speakers present;
     - with only one surround pair (5.1, sides or backs), ±110° at +10°;
     - back center 180°. Height speakers use the position below them.
   - A short, quiet reverb tail: four feedback delay lines, 0.44 s, damped above 6.3 kHz, starting
     5 ms after the direct sound.
   - LFE goes to both ears at 0.25. Every other speaker gets about 0.5.
   - About 1.5 % of one core at 48 kHz (6.5 % at 96 kHz), measured on an x86-64 build under
     Rosetta as Wine runs it.

   Before `macfinals-205` the first patch alone rendered a spherical head (Brown and Duda, "A
   structural model for binaural sound synthesis", 1998): a per-ear time delay and head-shadow
   filter, and a darker tone behind. The fitted patch replaces that model and keeps everything
   else (formats, the 7.1 offer, the stereo audio unit).

A stereo stream stays as it was. A device with real surround speakers (more than two channels)
is left alone, so the switch can stay on when you change outputs. The decision is made per
stream, so switching to headphones applies from the game's next audio restart, usually its next
launch.

The sphere model (before `macfinals-205`), measured alone at 48 kHz:

| Speaker | Delay, near / far ear | 4 kHz, near / far ear |
|---|---|---|
| Front left | 6.1 / 18.7 samples | −0.8 / −11.4 dB |
| Center | 12.2 / 12.2 | −5.5 / −5.5 dB |
| Side left | 0 / 31.5 | +2.6 / −11.4 dB |
| Back left | 6.1 / 18.7 | −3.2 / −13.8 dB |

The time differences match Woodworth's formula (0.26 ms at 30°, 0.66 ms at 90°).

## Switching it on

- **macSteam › Experimental › Headphone surround** writes `SteamDriver/headphone-surround`
  (`Surround.swift`). Only then do profiles show the setting, and only then does the run script
  read it.
- **A profile's Settings…** gets a **Headphone surround** switch:
  - Global DX12 and DX12 game profiles: `surround = True` in `d3dmetal.conf`.
  - Global DX11 and DX11 game profiles: `steamdriver.surround = True` in `dxmt.conf`, a key DXMT
    ignores. It is written only once used.
- **Launch option** `STEAMDRIVER_SURROUND=1` or `0` wins over both, switch file or not.

The run script (`environments/finals/compat-tool/run`) reads the same profile as upscaling:
- the game's own profile if it has one;
- else Global DX11 for a game last detected as DX11;
- else Global DX12.

It exports `WINE_HEADPHONE_SURROUND=1` and logs "headphone surround on (from the …)" in
`steamdriver-run.log`. Benchmark sessions record `"surround"`. Turning the experimental switch off
keeps the profiles' settings; they count again when it's back on.

Only the SteamDriver tool has it. NotProton's rows run CrossOver's own Wine, which this patch
isn't in.

## Settings (your own version of the model)

Experimental › Headphone surround › **Settings…** opens a window with a live preview and the
model's settings. Moving one is heard at once; **Save** makes games use them from their next
launch. One set for every game.

- **Settings:** speaker positions (front, sides, back), tilt (elevation of each pair), width (how
  much louder and earlier the near ear is), room (amount and length of the reverb tail), tone (bass
  and treble shelves) and the side and back levels. Each starts at the fitted value and has its own
  reset arrow; **Reset to Fitted** puts them all back. Tilting the front also changes the overall
  tone a little: the fitted model evens out the front direction's tone for every speaker.
- **Live preview:** `Contents/Helpers/surround_preview` (`scripts/surround_preview.c`) runs the
  fit's own model (`scripts/surround_fit/render2.c`, refactored into functions; its bake is still
  byte-identical to `fitted_ir.h`) with the head table `scripts/surround/kemar.head` (MIT KEMAR,
  free to use with credit: THIRD_PARTY_NOTICES.md), and plays on the Mac's output with an
  AudioQueue. A change fades the sound out and in over about 40 ms. On and Off switch at once in
  the same process (Off: the plain stereo mix, as in Wine). Play a File… plays a WAV with them.
- **Save:** the preview bakes the 12 speaker filters, gains and reverb tail into
  `SteamDriver/surround-settings.sdsurr` (29,272 bytes: `SDSURR1`, rate, taps, positions, then the
  values) and keeps the values in `surround-settings.json`. At the fitted values both files are
  removed, so Wine plays its built-in filters.
- **In games:** SteamDriver's run script sets `WINE_HEADPHONE_SURROUND_FILTERS` to the file when
  surround is on and the file exists; `wine-coreaudio-headphone-surround-settings.patch` (runtime
  macfinals-206 on, shipped in 212) loads it when a stream starts. A file that isn't exactly right
  (size, rate, taps, a value that isn't finite) is refused with a warning and the fitted filters
  play.
- **Tested:** a bake at the fitted values matches the stored fingerprint; moving the front speakers
  changes the front hits and not the back; a broken file plays exactly the built-in filters (all
  through the driver's own C in the offline renderer); Wine 212 loads a file and refuses a broken
  one; the preview's commands while playing; the app saving, loading and resetting
  (`tests/test_surround_settings.py`, `tests/test_surround_signature.py`, `tests/test_surround.py`).

## Sound tests

`tests/test_surround_signature.py` measures the fitted model's sound as the fit did:
- **Fingerprint:** per hit of the ball test, the left/right level difference (overall, below
  1 kHz, above 3 kHz), which ear hears it first, level and brightness, against
  `tests/surround_signature.json`. Also what any working model does: mirrored hits come out
  mirrored, the near ear is louder and first, behind is darker than in front. A gain change of
  +0.85 dB on the side speakers fails it. After a deliberate change, run it with
  `SURROUND_SIGNATURE_WRITE=1` to store the new fingerprint.
- **Against Dolby:** on Rasmus's Mac only (the fit's data and `~/SteamDriver-build/surround-fit/venv`
  with numpy and scipy), the stress test rendered and scored by the fit's scorer; it must stay at
  the fitted scores (left/right 0.95, tone 1.33, timing 0.01, level spread 0.60, moving 1.02).

## The Test in macSteam

Beside the switch in Experimental › Headphone surround (`SurroundTest.swift`):
- **The microphone prompt:** Core Audio files a microphone request when a program starts playback
  on an output device that also has inputs: an audio interface (a Focusrite Scarlett), a headset,
  Loopback. On an output-only device (MacBook speakers, a display's speakers) it files none.
  Measured 2026-11-27 with a silent AudioQueue player per device and `log stream` on TCC:
  Scarlett and Loopback each one request with `preflight=no`, MacBook speakers and a monitor
  none. Neither Wine nor the surround processing opens an input; the request comes with the
  device. The players are children of macSteam, so the request is macSteam's: macOS asks once
  (macSteam declares microphone use for voice chat), and either answer stands for the test and
  the games. An ad-hoc signed dev build is a new app to macOS, so it asks again per build.
  Tried and reverted the same day: starting the players as their own processes (the spawn
  attribute Terminal uses) made Wine an app of its own, so macOS asked about "wine" and held
  playback until it was answered (the test stayed at Starting…).
- **Volume:** 20 % each time macSteam starts, so nobody gets blasted; not saved. It changes the
  test while it plays.
- **Play** (▶, ⏸ while playing) plays one ball bounce (`scripts/surround/ball-bounce.wav`, cut from a Pixabay
  recording, Pixabay Content License: `scripts/surround/README.md`) at different spots around you,
  in five patterns with a short pause between them (`scripts/surround_signal.h`):
  1. a slow lap clockwise (16 hits, 22.5° apart);
  2. two fast laps counterclockwise;
  3. jumps clockwise to 8 spots, three hits at each;
  4. front and back;
  5. right and left.

  Each hit is panned between the two nearest 7.1 speakers, as a game's audio engine pans a sound.
  The diagram shows where each hit is, and the status line names the pattern. About 37 s, 88 hits.
- **Surround On / Off** swaps between Wine's headphone rendering and a plain stereo mix at once,
  without a gap, a restart or a pause. Off is what a game plays on headphones without the setting:
  it sees a stereo output and mixes its 7.1 down itself. The Off player does the same (each side:
  front + 0.7071 x centre, side and back; no LFE). Until 2026-11-27 it handed the stereo device
  7.1, and macOS, which matches channels by speaker and doesn't mix, dropped centre, sides and
  backs, so Off sounded empty in the middle and behind. No game does that. Two
  players run at the same time, one with `WINE_HEADPHONE_SURROUND` and one without, on one
  timeline (`-sync`, a shared memory block). Every change (play, pause, jump, which one is heard)
  takes effect a fixed time after it's asked for, longer than the queue, and each player places
  it at the sample heard then from its own stream clock. Only the chosen player is heard; the
  swap is a 5 ms crossfade. Measured in Wine: both players stay within a few milliseconds of the
  timeline (the stream position moves in 5 ms steps).
- **Replay** (↺) plays the test, or the last file, again from the start.
- **The position bar** shows where in the sequence it is (0:00 / 0:36); drag it to jump there.
  Paused, it stays paused; stopped, Play starts from there. Paused for 5 minutes, the players end
  and Play starts them again at the same place.
- **Surround sound only:** Wine renders a stream with more than 2 channels (5.1, 7.1) and only
  when the Mac's output is stereo (`binaural_wanted`); stereo and mono pass through as they are.
  With it on the stereo output is offered as 7.1, so most games choose 7.1 themselves; a game with
  its own audio option needs 7.1 or surround there, not Stereo or Headphones. The test always plays
  7.1, so a stereo file comes from front left and right only, and the status line says so.
- **Play a File…** plays a WAV file of your own the same way:
  - 16-, 24- or 32-bit PCM, or 32-bit float;
  - mono, stereo, 5.1, 7.1 or anything with a channel mask;
  - mapped to 7.1 and resampled to the output's rate.

  **Back to the Test** returns to the ball.

The player's commands on standard input: `gain X`, `seek S`, `pause`, `play`, `surround 1|0`
(macSteam sends them to the On player, which sets the shared timeline for both). It reports
`ready`, `length`, `time` four times a second, `hit` and `done`. Wine's stream position comes with
a time from another clock than `QueryPerformanceCounter`, so the player takes the time itself.

`scripts/surround_test.c` does the playing. It's built into the app as
`Resources/SurroundTest/surround_test.exe` and run with the installed SteamDriver tool's Wine, in
a prefix of its own: `SteamDriver/surround-test/prefix`, a clone of the runtime's shared template
when there is one. It always opens a shared 7.1 float stream, as a 7.1 game does, so the only
difference between the On and Off players is `WINE_HEADPHONE_SURROUND`.

A commercial demo (Dolby's Atmos trailers and the like) only shows the difference as a
multichannel file. Their YouTube versions are already mixed down to stereo, and they play outside
Wine.

## Testing ladder

Each step has a known truth, so a problem can be placed:

1. **The model alone, as files** (`scripts/surround_render.py test`): writes macSteam's test to
   `~/Library/Application Support/SteamDriver/surround-test/files`:
   - `test-7.1.wav`, what a game plays, and `test-7.1.m4a`, the same as AAC 7.1 for Music and
     QuickTime (they render it themselves: macOS's mix, or Apple's spatial audio on AirPods);
   - `test-headphones.wav`, what headphone surround makes of it;
   - `test-plain.wav`, a plain stereo downmix;
   - `test-hits.txt`, each hit's time, direction and pattern.

   All are 24-bit PCM (players skip float WAV). Play them in QuickTime or Music with headphones.
   The model's C and filters are taken from both patches at run time, so the files are exactly what
   the runtime does. `surround_render.py render IN OUT` renders any
   multichannel WAV.
2. **Through Wine:** the Test in macSteam, with the tool updated to a runtime that has the patch
   (`macfinals-198` for the sphere, `macfinals-205` for the fitted model). With runtime
   `macfinals-191` On and Off are the same. That was the first test on 2026-11-26.
3. **A game** set to 7.1 or surround.

**Reference** (local only, not shared): Dolby's demo, which was on YouTube, kept as
`surround-test/reference.wav` beside those files. Measured at 0:18–0:38:
- 0:18–0:30: left and right within ±1.2 dB with no timing difference. The motion there is sound
  design, not placement.
- 0:30–0:35: placement by level, 3–13 dB, with about 1 ms of timing difference at 0:35.5.

An earlier test sound (pink noise circling the listener), measured the same way:
- timing differences from −0.65 to +0.65 ms, following the sound;
- level differences of at most about 5 dB over the whole band, because pink noise is mostly low
  pitches, where the sphere's shadow is small;
- almost the same cues behind as in front, since the model is symmetric front to back apart from
  the rear tone cut.

Front/back is the weak point. Measured HRTFs (below) are the fix.

## Measured head responses (files only, 2026-11-26)

History: the first measured-response experiments, before the fit. `scripts/surround_hrtf.h` was a
second model in the file renderer (no longer used by it), to compare by ear:
- **Measured head responses:** each speaker is convolved with MIT's KEMAR head-related impulse
  responses for its direction (`scripts/surround/kemar_elev0.h`, made by
  `scripts/surround_kemar.py` from the compact set). The data is free to use with the authors
  credited: Bill Gardner and Keith Martin, MIT Media Lab, 1994. The responses are 128 taps at
  44.1 kHz, resampled to the output's rate with a windowed sinc.
- **Optional room:** three early reflections per speaker (near side wall 6.5 ms, far side wall
  12.5 ms, front or back wall 9 ms), each from its own direction through the same responses and
  with the highs taken off. Then a 0.4 s reverb tail at a low level.

`surround_render.py test` wrote all of them then (it now writes only the runtime's model):

| File | What |
|---|---|
| `test-plain.wav` | plain stereo downmix (surround off) |
| `test-A-sphere.wav` | the runtime's model today (spherical head) |
| `test-B-hrtf.wav` | measured head responses, as measured |
| `test-C-hrtf-eq.wav` | B with front equalization: a sound straight ahead keeps its own tone |
| `test-D-hrtf-eq-room.wav` | C plus weak early reflections and a reverb tail from 25 ms |
| `test-E-hrtf-eq-tail.wav` | C plus the tail only |
| `test-H0-control.wav` | E split into each ear's onset and a minimum-phase response, then rebuilt with nothing scaled |
| `test-H1-timing-1.3.wav` | E with the timing difference 1.3× (0.67 → 0.85 ms at 90°) |
| `test-H2-level-1.3.wav` | E with the level differences 1.3× (in dB, every octave) |
| `test-H3-front-back-1.5.wav` | E with each direction's tone 1.5× as far from the all-directions average |
| `test-H4-all.wav` | H1, H2 and H3 together |
| `test-J-dry.wav` | H0's responses without the room (like C) |
| `test-K-light-room.wav` | H0's responses with the tail at a third of E's |
| `test-ref-dolby.wav` | Dolby Atmos for Headphones (local only, below) |

### Dolby Atmos for Headphones as the reference

Recorded in a Windows VM (Parallels, the Focusrite passed through to it) with
`surround_test.exe -record` (`record.bat` in `~/Desktop/surround-test-windows`). It plays the same
7.1 test and records the output by WASAPI loopback. With Spatial sound on, the recording differs
from the one with it off, so it holds Dolby's processing. It stays on this Mac, kept only to
compare against; nothing of Dolby's goes into macSteam.

Measured per octave (250 Hz to 16 kHz) from FFTs of each hit (an earlier measurement with
one-pole "bands" was wrong, because those bands overlap too much for a narrow-band sound):

| Measure | Dolby | A sphere | B KEMAR | C/E KEMAR, equalized |
|---|---|---|---|---|
| Ear difference at 90°, dB | −2 −4 −5 −6 −10 −23 −21 | 0 −2 −4 −10 −13 −17 −21 | −3 −5 −5 −8 −7 −18 −30 | as B |
| Tone ahead against plain, dB | +1 +1 +1 0 +2 −3 | about 0 | −8 −7 −7 +5 +2 −8 | about 0 (level) |
| Room: 60–190 ms after each hit against its first 30 ms | −17.1 dB | | −31.5 dB | C −19.0, E −17.2, D −18.1 |

- Measured head responses give ear differences close to Dolby's; the sphere's are too small in the
  lows and too large around 2–4 kHz.
- Unequalized, KEMAR is thin with a peak at 2 kHz (the dummy head's ear canal). Front
  equalization (`hrtf_front_eq`) fixes that the way Dolby keeps the front sound neutral.
- Dolby's timing difference at 90° is 0.67 ms, as KEMAR's (cross-correlation under 1.5 kHz; an
  earlier rough estimate of 1.1 ms was wrong). So Dolby does not widen the head.
- Listening (Rasmus, 2026-11-26): **H0 sounds best.** Making the timing, level or front/back
  differences stronger (H1–H4) didn't help: the measured values, which match Dolby's, are right.
  H0 is E with each ear's response rebuilt as onset plus minimum phase.
- Dolby adds very little room. **Correction:** "E matches Dolby's room" came from one broadband
  number dominated by the highest octave. Measured per octave (the tail from 60 ms after each hit
  against the hit), Dolby is within about 1 dB of the plain mix from 250 Hz to 4 kHz, while E and H0
  add 5–7 dB more reverb at 500 Hz and 2 kHz. Strong early reflections (an earlier D) also washed
  out direction: 45° and 135° measured almost the same.

Compared with Dolby over all 88 hits (2026-11-26), objectively rather than by ear:

| Measure | C | E | H0 | J (H0, dry) | K (H0, light room) | A sphere |
|---|---|---|---|---|---|---|
| Left/right distance to Dolby, average dB | 2.0 | 2.0 | 1.8 | 1.8 | 1.8 | 2.8 |
| Tone distance to Dolby, average dB | 1.4 | 1.4 | 1.4 | 1.4 | 1.4 | 1.7 |
| Sharpness (first 3 ms against next 30 ms; Dolby −13.8) | −15.3 | −15.3 | −14.9 | −14.8 | −14.9 | −15.1 |
| Room at 500 Hz / 2 kHz (Dolby −25.8 / −35.5) | −25.7 / −36.7 | −20.1 / −28.6 | −20.1 / −29.1 | −25.9 / −36.7 | −24.5 / −35.4 | |

The minimum-phase responses (H0, J, K) are closest on left/right and sharpness; the drier rooms
(C, J, K) are closest on room. K is the closest on every measure.

## The fitted model (2026-11-27)

Instead of tuning by ear, the model was fitted to Dolby Atmos for Headphones by measurement.
The tools and how to rerun them: `scripts/surround_fit/README.md`.

- **A harder test.** A 5-minute 7.1 stress test of 533 sounds in 12 sections: every speaker,
  footsteps, pitch, loudness from −10 to −50 dB, angles, moving sounds, busy scenes, reverb tails,
  noise, sweeps and voices. Kenney's CC0 samples, local only. Rasmus recorded it with Dolby on
  and off in the Windows VM.
- **Scored by hearing thresholds.** Per event: left/right level per octave (target 1 dB), tone by
  direction (1 dB), timing difference below 1.5 kHz (0.02 ms), level spread (1 dB), room (1 dB),
  moving sounds' left/right over time. Cost = the mean of every measure divided by its threshold,
  plus a quarter of the worst, so no section is given up for another.
- **Every head, every knob.** 324 measured heads (single people and group averages from six SOFA
  sets) at neutral settings, then CMA-ES on the best 20 plus the averages and dummy heads, then
  10 hours of restarts on the best four: 46,292 renders. 28 knobs: timing and level-difference
  scale per frequency range, front/back contrast, smoothing, front EQ amount, two shelves, each
  speaker group's angle, elevation, gain and delay, early reflections and the tail.
- **The ball test is never fitted**, only checked, so a fit that just learned the stress test
  would show there.

Result, MIT KEMAR (normal pinna) at cost 1.41 (the runner-up, the LISTEN average, 1.47; the best
at neutral settings 2.18):

| Against Dolby | Stress test | Ball test (not fitted) | Old L fit (ball) |
|---|---|---|---|
| Left/right, dB | 0.94 | 1.17 | 1.17 |
| Tone by direction, dB | 1.34 | 0.67 | 0.97 |
| Timing, ms | 0.01 | 0.03 | 0.02 |
| Level spread, dB | 0.60 | | |
| Moving sounds' left/right, dB | 1.02 | | |
| Room, dB | 0.15 | 0.96 | 0.75 |
| Overall tone, dB per octave | 2.16 | 1.56 | |

What Dolby looks like through this:
- the speakers wider than the standard layout: fronts at about 43°, backs at 137°, sides at 85°;
- the fronts a little below ear height and the sides above it;
- level differences between the ears weaker than the raw head (×0.75 around 2 kHz);
- a smoothed, darker tone (−9 dB above 5.6 kHz) and about two thirds of the front equalization;
- next to no early reflections (dropping them costs 1 %, so the patch has none) and a short
  tail;
- no compression or leveling: linear from −10 to −50 dB.

The knobs, as baked:
`itd=0.980094 ild=0.747875 ild_lo=0.834084 ild_hi=1.0258 contrast=0.434911 smooth=0.9529
xover=18.1179 eq=0.663981 lo_g=-3.53549 lo_f=171.974 hi_g=-9.11723 hi_f=5578.91 front=42.6106
side=85.3018 back=136.767 el_front=-17.9653 el_side=17.3703 el_back=3.02427 g_side=0.136804
g_back=-0.535721 d_side=0 d_back=0 early=0 tail=0.716398 room_lp=1033.36 tail_lp=6271.11
rt=0.43569 tail_ms=5.04038`

The patch's own C, rendered on the stress test by `surround_render.py render`, matches the fitting
renderer to −123 dB and scores 1.414.

**Licence:** the SOFA file's own licence field: "This HRTF data is provided without any usage
restrictions. It is requested that you cite Gardner and Martin (1995) when using this data for
research or commercial applications." Gardner, W. G., and Martin, K. D. (1995), "HRTF measurements
of a KEMAR," J. Acoust. Soc. Am. 97, 3907–3908 (MIT Media Lab; SOFA conversion by the Acoustics
Research Institute, OeAW). The patch and `surround_ir.h` credit them. No Dolby filter
or recording is in the patch: only the 28 numbers above, which a measurement produced.

## Tested

- `tests/test_surround.py`: the run script's decision (switch file, game profile over global, DX11
  key, launch option both ways) and the patch being last in `WINE_PATCHES`.
- `tests/test_notproton_options.py`: the profile keys in both files and the summary line.
- In Wine (`macfinals-198`, Focusrite USB output, 2 channels, 48 kHz), a small Windows program
  (IMMDevice → IAudioClient):
  - off, the mix format is 2 channels, mask 0x3;
  - on, it is 8 channels, mask 0x63F;
  - a shared 7.1 float stream initializes and plays 1.5 s through the binaural path.
- `tests/test_surround.py` also runs the test player quietly (`-quiet`, 60 dB down) in the
  `macfinals-198` runtime:
  - all seven speakers, in order, with surround off and on;
  - a 5.1 44.1 kHz WAV, mapped to 7.1 and resampled;
  - a file that isn't a WAV, which gets an error.
- `macfinals-205` (the fitted model), 2026-11-27: the same tests pass; `WINEDEBUG=trace+coreaudio`
  shows each 7.1 channel at its fitted position with 304-tap filters at 48 kHz. App build
  `runtime/build/dev74/macSteam.app`.
- Not yet: listening, in the test or in a game.

## Ideas, not done

- **Room size (Rasmus, 2026-11-26):** stays at E, about Dolby's default. What size feels right
  depends on the situation, so judging it with nothing to follow lacks context; it may matter
  once there are objects or something the room should match. `hrtf_create` keeps a `size` (1 = E)
  for that. A first try at 1.8 and 2.6 was far too heavy-handed.

- Rendering a stereo game's two channels as virtual speakers at ±30°, which also moves the sound
  out of the head.
