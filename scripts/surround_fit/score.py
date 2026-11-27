"""Score a rendered bounce test (stereo f32 at 48 kHz) against the Dolby recording, hit by hit.

Features per hit: level per octave of each ear (tone and left/right difference), timing difference
(low frequencies), sharpness (first 3 ms vs the next 30), and room per octave (60 ms on vs the hit).
The noise floor is how much Dolby differs from itself between repeated hits at the same spot."""
import sys
import numpy as np
from scipy.signal import butter, sosfilt

R = 48000
N = 2048
CENTERS = [250, 500, 1000, 2000, 4000, 8000]
HITS = '/Users/rasmus/Library/Application Support/SteamDriver/surround-test/files/test-hits.txt'
REF = '/private/tmp/claude-501/-Users-rasmus-Documents-Kod-Annat-MacVolvo/e3080bce-75a9-4c7e-b80a-992eb10a48d9/scratchpad/cmp/dolby-on.f32'

hits = [l.split()[:3] for l in open(HITS).read().splitlines()]
AZ = np.array([float(h[1]) for h in hits])
PAT = np.array([int(h[2]) for h in hits])
WIN = np.hanning(N)
FREQ = np.fft.rfftfreq(N, 1 / R)
BANDS = [(FREQ >= c / np.sqrt(2)) & (FREQ < c * np.sqrt(2)) for c in CENTERS]
LP = butter(4, 1500, fs=R, output='sos')


def load(path):
    v = np.fromfile(path, '<f4')
    return v[0::2].astype(np.float64), v[1::2].astype(np.float64)


def onsets(L, Rr):
    env = np.maximum(np.abs(L), np.abs(Rr))
    idx = np.nonzero(env > env.max() * 0.08)[0]
    gaps = np.diff(idx, prepend=-10 ** 9)
    return idx[gaps > int(0.1 * R)]


def octs(x):
    seg = np.zeros(N)
    seg[:len(x)] = x[:N]
    p = np.abs(np.fft.rfft(seg * WIN)) ** 2
    return np.array([10 * np.log10(p[b].sum() + 1e-18) for b in BANDS])


def features(path):
    L, Rr = load(path)
    on = onsets(L, Rr)
    if len(on) != len(hits):
        raise ValueError(f'{path}: {len(on)} onsets, want {len(hits)}')
    Ll, Rl = sosfilt(LP, L), sosfilt(LP, Rr)
    f = {k: [] for k in ('ild', 'tone', 'itd', 'sharp', 'room')}
    for s in on:
        a = s - int(0.003 * R)
        l, r = octs(L[a:a + N]), octs(Rr[a:a + N])
        both = 10 * np.log10(10 ** (l / 10) + 10 ** (r / 10))
        f['ild'].append(l - r)
        f['tone'].append(both)
        t0 = s + int(0.06 * R)
        tl, tr = octs(L[t0:t0 + N]), octs(Rr[t0:t0 + N])
        f['room'].append(10 * np.log10(10 ** (tl / 10) + 10 ** (tr / 10)) - both)
        e = L[s:s + int(0.033 * R)] ** 2 + Rr[s:s + int(0.033 * R)] ** 2
        f['sharp'].append(10 * np.log10(e[:int(0.003 * R)].sum() / e[int(0.003 * R):].sum()))
        # timing: lag of the low-passed right ear against the left, within 1.5 ms
        x, y = Ll[a:a + 2400], Rl[a:a + 2400]
        lags = np.arange(-72, 73)
        c = [np.dot(x[max(0, -k):len(x) - max(0, k)], y[max(0, k):len(y) - max(0, -k)]) for k in lags]
        j = int(np.argmax(c))
        frac = 0.0
        if 0 < j < len(c) - 1:                      # parabolic peak: below one sample
            den = c[j - 1] - 2 * c[j] + c[j + 1]
            frac = 0.5 * (c[j - 1] - c[j + 1]) / den if den else 0.0
        f['itd'].append((lags[j] + frac) / R * 1000)
    return {k: np.array(v) for k, v in f.items()}


def compare(f, g):
    """Distances of f from g (the reference); smaller is closer."""
    off = f['tone'].mean(0) - g['tone'].mean(0)            # each version's own overall tone
    d = {
        'left/right dB': np.abs(f['ild'] - g['ild']).mean(),
        'tone by direction dB': np.abs(f['tone'] - off - g['tone']).mean(),
        'overall tone dB': np.abs(off - off.mean()).mean(),
        'timing ms': np.abs(f['itd'] - g['itd']).mean(),
        'sharpness dB': abs(f['sharp'].mean() - g['sharp'].mean()),
        'room dB': np.abs(f['room'].mean(0) - g['room'].mean(0)).mean(),
    }
    return d


# close enough: about the smallest difference people hear for each measure
TARGET = {'left/right dB': 1, 'tone by direction dB': 1, 'overall tone dB': 1,
          'timing ms': 0.02, 'sharpness dB': 0.5, 'room dB': 1}
# weights: what counts as one unit of "off" for each measure
WEIGHTS = {'left/right dB': 1, 'tone by direction dB': 1, 'overall tone dB': 0.5,
           'timing ms': 25, 'sharpness dB': 1, 'room dB': 0.5}


def cost(d):
    return sum(WEIGHTS[k] * v for k, v in d.items())


def noise_floor(g):
    """How much Dolby differs from itself: repeated hits at the same spot within one pattern."""
    ild, tone, itd = [], [], []
    for p in set(PAT):
        for az in set(AZ[PAT == p]):
            i = np.nonzero((PAT == p) & (AZ == az))[0]
            for a in i:
                for b in i:
                    if a < b:
                        ild.append(np.abs(g['ild'][a] - g['ild'][b]).mean())
                        tone.append(np.abs(g['tone'][a] - g['tone'][b]).mean())
                        itd.append(abs(g['itd'][a] - g['itd'][b]))
    return {'left/right dB': np.mean(ild), 'tone by direction dB': np.mean(tone), 'timing ms': np.mean(itd)}


_ref = None


def ref():
    global _ref
    if _ref is None:
        _ref = features(REF)
    return _ref


def fmt(d):
    return '  '.join(f'{k} {v:.2f}' for k, v in d.items())


if __name__ == '__main__':
    g = ref()
    print('close enough:', fmt(TARGET))
    for p in sys.argv[1:]:
        d = compare(features(p), g)
        print(f'{p.split("/")[-1]:<22} cost {cost(d):5.2f} | {fmt(d)}')
