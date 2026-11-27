# Headphone surround: fitting the model

How the filters in `wine-coreaudio-headphone-surround-fitted.patch` (`surround_ir.h`, also
`scripts/surround/fitted_ir.h`) were made. docs/SURROUND.md has the results.

The tools take their data from the folder they run in. Only the scripts are kept here; the data
stays local, never in the repository or the app:
- **Head responses:** SOFA files from sofacoustics.org (MIT KEMAR, SADIE II, CIPIC, LISTEN,
  HUTUBS, RIEC) in `../hrtf/`.
- **Reference:** Dolby Atmos for Headphones recorded in a Windows VM, `stress-dolby-on.f32` and
  `dolby-on.f32` (local only: nothing of Dolby's goes into macSteam).
- **Stress test samples:** Kenney's CC0 packs in `../kenney/`.

On Rasmus's Mac all of it is in `~/SteamDriver-build/surround-fit/` (`fit/`, `hrtf/`, `kenney/`).
Copy these scripts into `fit/` to run them there. They need numpy, scipy, h5py and cma: the venv
is `~/SteamDriver-build/surround-fit/venv` (also used by `tests/test_surround_signature.py`).

| File | What |
|---|---|
| `heads.py`, `build_heads.py` | each SOFA file (and each group's average) as a head table: level per FFT bin and onset per ear, azimuth every 5°, elevation −30° to 60° |
| `stress.py` | the 5-minute 7.1 stress test (533 events in 12 sections) and `stress-events.json` |
| `score.py` | features and comparison for macSteam's ball test |
| `score_stress.py` | the same for the stress test: alignment, per-event left/right per octave, timing, tone, level, room |
| `render2.c` | the model with every knob; `bake` writes the filters header instead of rendering |
| `fit2.py` | the unattended fit: every head screened, CMA-ES on the best, the ball test as an overfitting check |

Build and bake:

```sh
clang -O3 -w -I .. render2.c -o render2 -lm          # in fit/: -I the repository's scripts/
./render2 heads/mit-mit_kemar_normal_pinna.head fitted_ir.h bake itd=0.980094 ...   # knobs: docs/SURROUND.md
```

The fit itself used `-ffast-math` for speed. The patch's C matches render2 to −123 dB.
