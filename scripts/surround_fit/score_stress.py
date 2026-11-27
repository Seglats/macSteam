"""Score a binaural render of the stress test against a reference (the Dolby recording), section by
section. Both are stereo f32 at 48 kHz; the reference may be offset and its clock may drift, which
is measured and undone first.

usage: score_stress.py REF.f32 MODEL.f32 [MODEL2.f32 ...]"""
import json
import os
import sys

import numpy as np
from scipy.signal import butter, sosfilt, fftconvolve

R = 48000
HERE = os.path.dirname(os.path.abspath(__file__))
EVENTS = json.load(open(os.path.join(HERE, 'stress-events.json')))
CENTERS = [125, 250, 500, 1000, 2000, 4000, 8000, 16000]
LP = butter(4, 1500, fs=R, output='sos')
TARGET = {'left/right dB': 1, 'tone by direction dB': 1, 'timing ms': 0.02, 'moving left/right dB': 1, 'room dB': 1}


def load(p):
    v = np.fromfile(p, '<f4').astype(np.float64)
    return np.stack([v[0::2], v[1::2]], 1)


def source_mono():
    x = np.fromfile(os.path.join(HERE, 'stress-7.1.f32'), '<f4').reshape(-1, 8).astype(np.float64)
    return x.sum(1)


def align(ref, src):
    """Map source time -> reference sample: offset + drift, from cross-correlating envelopes of the
    mono sums in windows across the whole test."""
    def env(x):
        e = np.abs(x)
        k = 48
        return e[:len(e) // k * k].reshape(-1, k).mean(1)       # 1 ms resolution
    er, es = env(ref.sum(1)), env(src)
    # coarse: the whole first minute
    n = min(60000, len(es))
    c = fftconvolve(er[:n + 5000], es[:n][::-1], mode='valid')
    coarse = int(np.argmax(c))
    pts = []
    for start in range(5000, len(es) - 8000, 15000):           # every 15 s, fine at sample level
        s0, s1 = start * 48, (start + 6000) * 48
        a = src[s0:s1]
        b0 = s0 + coarse * 48 - 4800
        b = ref.sum(1)[max(b0, 0):b0 + (s1 - s0) + 9600]
        if len(b) < (s1 - s0) + 9600 or np.abs(a).max() < 1e-4:
            continue
        cc = fftconvolve(b, a[::-1], mode='valid')
        lag = int(np.argmax(np.abs(cc)))
        pts.append((s0, b0 + lag - s0))
    pts = np.array(pts, float)
    slope, inter = np.polyfit(pts[:, 0], pts[:, 1], 1)
    resid = pts[:, 1] - (slope * pts[:, 0] + inter)
    return lambda s: int(round(s + slope * s + inter)), (inter / R * 1000, slope * 1e6, np.abs(resid).max())


def octave_levels(x):
    """Level per octave of a stretch (Welch average of 4096-point frames), dB."""
    n = 4096
    if len(x) < n:
        x = np.pad(x, (0, n - len(x)))
    w = np.hanning(n)
    frames = [x[i:i + n] * w for i in range(0, len(x) - n + 1, n // 2)]
    p = np.mean([np.abs(np.fft.rfft(f)) ** 2 for f in frames], 0)
    f = np.fft.rfftfreq(n, 1 / R)
    return np.array([10 * np.log10(p[(f >= c / 2 ** .5) & (f < c * 2 ** .5)].sum() + 1e-20) for c in CENTERS])


def itd(l, r):
    a, b = sosfilt(LP, l), sosfilt(LP, r)
    lags = np.arange(-72, 73)
    c = np.array([np.dot(a[max(0, -k):len(a) - max(0, k)], b[max(0, k):len(b) - max(0, -k)]) for k in lags])
    j = int(np.argmax(c))
    frac = 0.0
    if 0 < j < len(c) - 1:
        den = c[j - 1] - 2 * c[j] + c[j + 1]
        frac = 0.5 * (c[j - 1] - c[j + 1]) / den if den else 0.0
    return (lags[j] + frac) / R * 1000


def alone(i):
    e = EVENTS[i]
    a, b = e['t'], e['t'] + min(e['len'], 1.0) + 0.1
    return all(j == i or EVENTS[j]['t'] >= b or EVENTS[j]['t'] + EVENTS[j]['len'] <= a for j in range(len(EVENTS)))


ALONE = [i for i in range(len(EVENTS)) if alone(i) and EVENTS[i]['az_end'] is None]


def features(sig, at):
    """at(source sample) -> sample in sig."""
    out = {}
    for i in ALONE:
        e = EVENTS[i]
        s = at(int(e['t'] * R))
        n = int(min(e['len'], 1.0) * R)
        l, r = sig[s:s + n, 0], sig[s:s + n, 1]
        if len(l) < n or (l ** 2).sum() < 1e-12:
            continue
        L, Rr = octave_levels(l), octave_levels(r)
        tail = sig[s + n:s + n + int(0.3 * R)]
        out[i] = {'ild': L - Rr, 'tone': 10 * np.log10(10 ** (L / 10) + 10 ** (Rr / 10)),
                  'itd': itd(l[:4800], r[:4800]) if e['section'] not in ('noise', 'sweeps') else itd(l[:24000], r[:24000]),
                  'level': 10 * np.log10((l ** 2 + r ** 2).sum()),
                  'tail': max(10 * np.log10((tail ** 2).sum() + 1e-20) - 10 * np.log10((l ** 2 + r ** 2).sum()), -60)}
    # moving and busy sections: left/right per octave in 50 ms frames
    for sec in ('moving', 'busy', 'busy-loud'):
        ev = [e for e in EVENTS if e['section'] == sec]
        a, b = min(e['t'] for e in ev), max(e['t'] + e['len'] for e in ev)
        frames = []
        for t in np.arange(a, b - 0.05, 0.05):
            s = at(int(t * R))
            l, r = sig[s:s + 2400, 0], sig[s:s + 2400, 1]
            frames.append([10 * np.log10((band(l, c) + 1e-20) / (band(r, c) + 1e-20)) for c in (500, 2000, 8000)]
                          + [10 * np.log10((l ** 2 + r ** 2).sum() + 1e-20)])
        out[sec] = np.array(frames)
    return out


def band(x, c):
    X = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / R)
    return X[(f >= c / 2 ** .5) & (f < c * 2 ** .5)].sum()


def compare(f, g):
    """Per section: distances of f from g."""
    secs = {}
    keys = [i for i in ALONE if i in f and i in g]
    off = np.mean([f[i]['tone'] for i in keys], 0) - np.mean([g[i]['tone'] for i in keys], 0)
    for i in keys:
        e = EVENTS[i]
        d = secs.setdefault(e['section'], {'ild': [], 'tone': [], 'itd': [], 'level': [], 'tail': []})
        # octaves where the sound has energy (within 30 dB of its loudest octave in the reference)
        use = g[i]['tone'] > g[i]['tone'].max() - 30
        d['ild'].append(np.abs(f[i]['ild'] - g[i]['ild'])[use].mean())
        d['tone'].append(np.abs(f[i]['tone'] - off - g[i]['tone'])[use].mean())
        d['itd'].append(abs(f[i]['itd'] - g[i]['itd']))
        d['level'].append((f[i]['level'], g[i]['level']))
        d['tail'].append(f[i]['tail'] - g[i]['tail'])
    rows = {}
    for sec, d in secs.items():
        lv = np.array(d['level'])
        lv = lv - lv.mean(0)
        rows[sec] = {'left/right dB': np.mean(d['ild']), 'tone by direction dB': np.mean(d['tone']),
                     'timing ms': np.median(d['itd']), 'level spread dB': np.abs(lv[:, 0] - lv[:, 1]).mean(),
                     'room dB': abs(np.mean(d['tail'])), 'n': len(d['ild'])}
    for sec in ('moving', 'busy', 'busy-loud'):
        a, b = f[sec], g[sec]
        n = min(len(a), len(b))
        ok = (b[:n, 3] > b[:n, 3].max() - 40)
        rows[sec] = {'moving left/right dB': np.abs(a[:n, :3] - b[:n, :3])[ok].mean(), 'n': int(ok.sum())}
    rows['overall tone'] = {'offset per octave': off - off.mean()}
    return rows


def show(rows):
    for sec, d in rows.items():
        print('  %-11s' % sec, '  '.join(('%s %s' % (k, ' '.join('%+.1f' % x for x in v)) if isinstance(v, np.ndarray)
                                          else '%s %.2f' % (k, v) if k != 'n' else 'n=%d' % v) for k, v in d.items()))


def summary(rows):
    keys = ['left/right dB', 'tone by direction dB', 'timing ms', 'level spread dB']
    tot = {k: [] for k in keys + ['moving left/right dB']}
    for sec, d in rows.items():
        for k in tot:
            if k in d:
                tot[k].append(d[k])
    return {k: float(np.mean(v)) for k, v in tot.items() if v}


if __name__ == '__main__':
    ref = load(sys.argv[1])
    src = source_mono()
    at_ref, (off_ms, ppm, res) = align(ref, src)
    print('reference: offset %.1f ms, clock %+.1f ppm, worst alignment error %.0f samples; %d events alone'
          % (off_ms, ppm, res, len(ALONE)))
    g = features(ref, at_ref)
    for p in sys.argv[2:]:
        sig = load(p)
        at_m, _ = align(sig, src)                  # the model's own latency (its filters) taken out too
        f = features(sig, at_m)
        rows = compare(f, g)
        print('==', os.path.basename(p), '| average:', '  '.join('%s %.2f' % kv for kv in summary(rows).items()))
        show(rows)
