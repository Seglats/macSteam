"""Headphone surround's sound, measured: the Wine patches' fitted model (scripts/surround_render.py
renders it without Wine) against a stored fingerprint and, on Rasmus's Mac, against Dolby.

The fit (scripts/surround_fit/, docs/SURROUND.md) chose the model by comparing measured features of
its output with Dolby Atmos for Headphones. These tests measure the same kind of features so that a
change to the patches or to the baked filters that changes the sound shows up:

- Fingerprint: per hit of macSteam's ball test, the left/right level difference (overall, below
  1 kHz, above 3 kHz), which ear hears the onset first, the level and the brightness, against
  tests/surround_signature.json. Plus what any working model must do: a hit and its mirror image
  come out mirrored, the near ear is louder and first, behind is darker than in front.
- Against Dolby: the 5-minute stress test rendered and scored by the fit's own scorer against the
  local Dolby recording. Only where the fit's data and Python (numpy, scipy) are; Dolby's recording
  never goes into the repository.

A deliberate change to the model: run with SURROUND_SIGNATURE_WRITE=1 to store the new fingerprint,
and say why in the commit."""
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SIGNATURE = ROOT / 'tests/surround_signature.json'
FIT = Path.home() / 'SteamDriver-build/surround-fit'
FIT_PYTHON = FIT / 'venv/bin/python'
RATE = 48000

# How far a hit may move before it counts as a different sound: well under what a listener notices
# (about 1 dB in level difference, a few samples in timing), well over rounding and compilers.
TOLERANCE = {'ild': 0.3, 'ild_low': 0.3, 'ild_high': 0.5, 'onset': 2, 'level': 0.3, 'bright': 0.3}


def features(folder):
    """Per hit of the ball test (test-hits.txt): its features in the headphone render."""
    data = (Path(folder) / 'test-headphones.wav').read_bytes()
    at = data.index(b'data') + 8           # 24-bit stereo

    def lowpass(x, cutoff):
        a, y, out = math.exp(-2 * math.pi * cutoff / RATE), 0.0, []
        for v in x:
            y = v + (y - v) * a
            out.append(y)
        return out

    def energy(x):
        return sum(v * v for v in x) + 1e-12

    def onset(x):
        peak = max(abs(v) for v in x)
        return next(i for i, v in enumerate(x) if abs(v) > 0.2 * peak)

    rows = []
    for line in (Path(folder) / 'test-hits.txt').read_text().splitlines():
        t, az, pattern, *_ = line.split()
        base, n = at + int(float(t) * RATE) * 6, int(0.2 * RATE)
        s = [int.from_bytes(data[base + i * 3:base + i * 3 + 3], 'little', signed=True) / 8388608 for i in range(2 * n)]
        left, right = s[0::2], s[1::2]
        low_l, low_r = lowpass(left, 1000), lowpass(right, 1000)
        high_l = [a - b for a, b in zip(left, lowpass(left, 3000))]
        high_r = [a - b for a, b in zip(right, lowpass(right, 3000))]
        db = lambda a, b: 10 * math.log10(energy(a) / energy(b))
        rows.append(dict(t=float(t), az=float(az), pattern=int(pattern),
                         ild=round(db(left, right), 2), ild_low=round(db(low_l, low_r), 2), ild_high=round(db(high_l, high_r), 2),
                         onset=onset(right) - onset(left), level=round(10 * math.log10(energy(left) + energy(right)), 2),
                         bright=round(10 * math.log10((energy(high_l) + energy(high_r)) / (energy(low_l) + energy(low_r))), 2)))
    return rows


class Fingerprint(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='surround-signature-')
        cls.addClassCleanup(cls.temp.cleanup)
        subprocess.run(['python3', ROOT / 'scripts/surround_render.py', 'test', cls.temp.name], check=True, capture_output=True)
        cls.rows = features(cls.temp.name)
        if os.environ.get('SURROUND_SIGNATURE_WRITE') == '1':
            SIGNATURE.write_text(json.dumps(cls.rows, indent=0) + '\n')

    def test_every_hit_sounds_as_stored(self):
        stored = json.loads(SIGNATURE.read_text())
        self.assertEqual([(r['t'], r['az'], r['pattern']) for r in self.rows], [(r['t'], r['az'], r['pattern']) for r in stored])
        for now, then in zip(self.rows, stored):
            for key, limit in TOLERANCE.items():
                self.assertLessEqual(abs(now[key] - then[key]), limit,
                                     f'hit at {now["t"]} s ({now["az"]}°): {key} {now[key]} was {then[key]}')

    def test_mirrored_hits_come_out_mirrored(self):
        lap = {r['az']: r for r in self.rows if r['pattern'] == 1}        # a slow lap, every 22.5°
        for az, r in lap.items():
            mirror = lap.get((360 - az) % 360)
            if mirror is None or az in (0.0, 180.0):
                continue
            self.assertAlmostEqual(r['ild'], -mirror['ild'], delta=0.5, msg=az)
            self.assertAlmostEqual(r['onset'], -mirror['onset'], delta=2, msg=az)
            self.assertAlmostEqual(r['bright'], mirror['bright'], delta=0.3, msg=az)

    def test_the_near_ear_is_louder_and_first(self):
        for r in self.rows:
            if 0 < r['az'] < 180:          # right of the listener (clockwise from the front)
                self.assertLess(r['ild'], -2, r)
                self.assertLess(r['onset'], 0, r)
            elif 180 < r['az'] < 360:
                self.assertGreater(r['ild'], 2, r)
                self.assertGreater(r['onset'], 0, r)

    def test_in_front_and_behind_are_centred_and_behind_is_darker(self):
        front = [r for r in self.rows if r['az'] == 0.0]
        back = [r for r in self.rows if r['az'] == 180.0]
        self.assertTrue(front and back)
        for r in front + back:
            self.assertLess(abs(r['ild']), 0.5, r)
        self.assertLess(max(r['bright'] for r in back), min(r['bright'] for r in front) - 0.5)


FIXED = ('ild_lo=0.834084 ild_hi=1.0258 contrast=0.434911 smooth=0.9529 xover=18.1179 eq=0.663981 lo_f=171.974 hi_f=5578.91 '
         'd_side=0 d_back=0 early=0 room_lp=1033.36 tail_lp=6271.11 tail_ms=5.04038').split()
FITTED_KNOBS = ('front=42.6106 side=85.3018 back=136.767 el_front=-17.9653 el_side=17.3703 el_back=3.02427 ild=0.747875 itd=0.980094 '
                'tail=0.716398 rt=0.43569 lo_g=-3.53549 hi_g=-9.11723 g_side=0.136804 g_back=-0.535721').split()


class BakedSettings(unittest.TestCase):
    """Headphone surround › Settings…: macSteam's preview bakes the player's knobs into the file Wine's
    driver loads (WINE_HEADPHONE_SURROUND_FILTERS). The renderer runs the driver's own C, so it reads
    the file as Wine does."""
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='surround-baked-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.folder = Path(cls.temp.name)
        cls.preview = cls.folder / 'surround_preview'
        subprocess.run(['/usr/bin/clang', '-O2', '-I', ROOT / 'scripts', ROOT / 'scripts/surround_preview.c', '-framework', 'AudioToolbox',
                        '-o', cls.preview], check=True, capture_output=True)

    def bake(self, name, *knobs):
        path = self.folder / f'{name}.sdsurr'
        out = subprocess.run([str(self.preview), ROOT / 'scripts/surround/kemar.head', ROOT / 'scripts/surround/ball-bounce.wav',
                              *FIXED, *FITTED_KNOBS, *knobs, f'--bake={path}'], capture_output=True, text=True, check=True).stdout
        self.assertIn('baked', out)
        return path

    def render(self, name, filters=None):
        folder = self.folder / name
        env = {k: v for k, v in os.environ.items() if k != 'WINE_HEADPHONE_SURROUND_FILTERS'}
        if filters: env['WINE_HEADPHONE_SURROUND_FILTERS'] = str(filters)
        subprocess.run(['python3', ROOT / 'scripts/surround_render.py', 'test', folder], check=True, capture_output=True, env=env)
        return folder

    def test_the_fitted_settings_baked_sound_as_the_built_in_filters(self):
        baked = self.bake('fitted')
        self.assertEqual(baked.stat().st_size, 8 + 12 + 4 * (12 + 5) + 12 * 2 * 304 * 4)
        stored = json.loads(SIGNATURE.read_text())
        for now, then in zip(features(self.render('fitted', baked)), stored):
            for key, limit in TOLERANCE.items():
                self.assertLessEqual(abs(now[key] - then[key]), limit, f'{now["t"]} s ({now["az"]}°): {key}')

    def test_changed_settings_change_the_sound_where_they_should(self):
        # The front angle only: tilting the front also changes every speaker's tone (the fitted front
        # EQ evens out the front direction's tone and applies to all).
        wide = features(self.render('wide', self.bake('wide', 'front=65')))
        built_in = {(r['t'], r['az']): r for r in json.loads(SIGNATURE.read_text())}
        front = [r for r in wide if r['az'] in (22.5, 337.5)]
        self.assertTrue(front)
        self.assertTrue(any(abs(r['ild'] - built_in[(r['t'], r['az'])]['ild']) > 1 for r in front))
        back = [r for r in wide if r['az'] == 180.0]
        for r in back:          # the back speakers didn't move, nor did the tone
            self.assertAlmostEqual(r['bright'], built_in[(r['t'], r['az'])]['bright'], delta=0.3)

    def test_a_broken_file_plays_the_fitted_filters(self):
        broken = self.folder / 'broken.sdsurr'
        broken.write_bytes(self.bake('whole').read_bytes()[:1000])
        a = (self.render('broken', broken) / 'test-headphones.wav').read_bytes()
        b = (self.render('none') / 'test-headphones.wav').read_bytes()
        self.assertEqual(a, b)


@unittest.skipUnless(FIT_PYTHON.exists() and (FIT / 'fit/stress-dolby-on.f32').exists() and (FIT / 'fit/stress-7.1.wav').exists(),
                     "needs the fit's local data and Python (~/SteamDriver-build/surround-fit)")
class AgainstDolby(unittest.TestCase):
    # The fitted model's scores (docs/SURROUND.md, "The fitted model"), with a little room.
    LIMITS = {'left/right dB': 1.0, 'tone by direction dB': 1.4, 'timing ms': 0.05, 'level spread dB': 0.65, 'moving left/right dB': 1.08}

    def test_the_stress_test_still_scores_as_fitted(self):
        with tempfile.TemporaryDirectory(prefix='surround-dolby-') as folder:
            wav, raw = Path(folder) / 'model.wav', Path(folder) / 'model.f32'
            subprocess.run(['python3', ROOT / 'scripts/surround_render.py', 'render', FIT / 'fit/stress-7.1.wav', wav], check=True, capture_output=True)
            data = wav.read_bytes()
            raw.write_bytes(data[data.index(b'data') + 8:])        # 32-bit float stereo, as the scorer reads it
            out = subprocess.run([str(FIT_PYTHON), 'score_stress.py', 'stress-dolby-on.f32', str(raw)], cwd=FIT / 'fit',
                                 check=True, capture_output=True, text=True).stdout
        line = next(l for l in out.splitlines() if 'average:' in l).split('average:')[1]
        scores = {key: float(line.split(key)[1].split()[0]) for key in self.LIMITS}
        for key, limit in self.LIMITS.items():
            self.assertLessEqual(scores[key], limit, f'{key}: {scores[key]} (fitted model within {limit})')


if __name__ == '__main__':
    unittest.main()
