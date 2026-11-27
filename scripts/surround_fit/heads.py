"""Measured heads (SOFA files) -> head tables for render2: for a grid of directions (azimuth every 5
degrees clockwise from the front, right positive; elevation -30..60 every 10), each ear's level per
FFT bin (512-point FFT at 48 kHz, dB, diffuse-field normalised) and when it first hears the sound
(samples at 48 kHz, fractional). A group of heads is averaged (levels in dB, times in samples), which
gives a smoother, more generic head.

usage: heads.py OUTDIR   (builds every single head and every group average it finds)"""
import glob
import os
import sys

import h5py
import numpy as np
from scipy.signal import resample_poly

R = 48000
NFFT = 512
AZ = np.arange(0, 360, 5)
EL = np.arange(-30, 61, 10)
HRTF = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'hrtf')
UP = 8


def sph_to_vec(az, el):
    a, e = np.radians(az), np.radians(el)
    return np.stack([np.cos(e) * np.cos(a), np.cos(e) * np.sin(a), np.sin(e)], -1)


def read_sofa(path):
    with h5py.File(path, 'r') as f:
        ir = np.array(f['Data.IR'], dtype=np.float64)            # M x R x N
        rate = float(np.array(f['Data.SamplingRate']).ravel()[0])
        pos = np.array(f['SourcePosition'], dtype=np.float64)
        typ = f['SourcePosition'].attrs.get('Type', b'spherical')
        typ = typ.decode() if isinstance(typ, bytes) else str(typ)
    if 'cartesian' in typ.lower():
        x, y, z = pos[:, 0], pos[:, 1], pos[:, 2]
        az = np.degrees(np.arctan2(y, x))
        el = np.degrees(np.arctan2(z, np.hypot(x, y)))
    else:
        az, el = pos[:, 0], pos[:, 1]
    # SOFA azimuth is counterclockwise (left positive); ours is clockwise (right positive)
    return ir[:, :2, :], rate, (-az) % 360, el


def to48k(ir, rate):
    if abs(rate - R) < 1:
        return ir
    from math import gcd
    g = gcd(int(R), int(rate))
    return resample_poly(ir, R // g, int(rate) // g, axis=-1)


def onset(h):
    """First moment within 20 dB of the peak, on an 8x upsampled response (samples at 48 kHz)."""
    u = resample_poly(h, UP, 1)
    a = np.abs(u)
    return np.nonzero(a >= a.max() * 0.1)[0][0] / UP


def table(path):
    ir, rate, az, el = read_sofa(path)
    ir = to48k(ir, rate)[:, :, :NFFT]
    vec = sph_to_vec(az, el)
    db = np.zeros((len(EL), len(AZ), 2, NFFT // 2 + 1))
    dl = np.zeros((len(EL), len(AZ), 2))
    for i, e in enumerate(EL):
        for j, a in enumerate(AZ):
            k = int(np.argmax(vec @ sph_to_vec(a, e)))           # nearest measured direction
            for ear in range(2):
                h = ir[k, ear]
                db[i, j, ear] = 10 * np.log10(np.abs(np.fft.rfft(h, NFFT)) ** 2 + 1e-12)
                dl[i, j, ear] = onset(h)
    # diffuse-field normalisation: the power average over directions (weighted by cos elevation)
    w = np.cos(np.radians(EL))[:, None, None, None]
    p = (10 ** (db / 10) * w).sum((0, 1, 2)) / (w.sum() * len(AZ) * 2)
    db -= 10 * np.log10(p)
    dl -= dl.min()
    return db, dl


def save(out, name, db, dl):
    np.concatenate([db.astype('<f4').ravel(), dl.astype('<f4').ravel()]).tofile(os.path.join(out, name + '.head'))


GROUPS = {
    'cipic-avg': 'cipic/*.sofa', 'sadie-humans-avg': 'sadie/H*.sofa', 'hutubs-avg': 'hutubs/*measured*.sofa',
    'listen-avg': 'listen/*.sofa', 'riec-avg': 'riec/*.sofa',
}
SINGLES = ['mit/mit_kemar_normal_pinna.sofa', 'mit/mit_kemar_large_pinna.sofa', 'sadie/D1_*.sofa', 'sadie/D2_*.sofa']


def main(out):
    os.makedirs(out, exist_ok=True)
    every = {}
    for pat in list(GROUPS.values()) + SINGLES:
        for p in glob.glob(os.path.join(HRTF, pat)):
            every[p] = None
    for p in sorted(every):
        name = os.path.basename(os.path.dirname(p)) + '-' + os.path.splitext(os.path.basename(p))[0]
        dst = os.path.join(out, name + '.head')
        if os.path.exists(dst):
            continue
        try:
            db, dl = table(p)
        except Exception as e:                                      # noqa: BLE001
            print('skip', p, e)
            continue
        save(out, name, db, dl)
        print('head', name, flush=True)
    for g, pat in GROUPS.items():
        files = sorted(glob.glob(os.path.join(HRTF, pat)))
        names = [os.path.basename(os.path.dirname(p)) + '-' + os.path.splitext(os.path.basename(p))[0] for p in files]
        heads = [os.path.join(out, n + '.head') for n in names if os.path.exists(os.path.join(out, n + '.head'))]
        if not heads:
            continue
        n_db = len(EL) * len(AZ) * 2 * (NFFT // 2 + 1)
        v = np.mean([np.fromfile(h, '<f4') for h in heads], 0)
        save(out, g, v[:n_db].reshape(len(EL), len(AZ), 2, -1), v[n_db:].reshape(len(EL), len(AZ), 2))
        print('group', g, len(heads), flush=True)
    allh = [h for h in glob.glob(os.path.join(out, '*.head')) if not any(t in os.path.basename(h) for t in ('-avg', 'mit-', 'sadie-D'))]
    if allh:
        n_db = len(EL) * len(AZ) * 2 * (NFFT // 2 + 1)
        v = np.mean([np.fromfile(h, '<f4') for h in allh], 0)
        save(out, 'all-humans-avg', v[:n_db].reshape(len(EL), len(AZ), 2, -1), v[n_db:].reshape(len(EL), len(AZ), 2))
        print('group all-humans-avg', len(allh))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'heads'))
