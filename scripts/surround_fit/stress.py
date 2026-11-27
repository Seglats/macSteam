"""Stress test for headphone surround: about 3 minutes of 7.1 with many kinds of sound (Kenney CC0
packs, local only), at many directions, loudnesses and pitches, still and moving, alone and
overlapping. Writes stress-7.1.wav (32-bit float, mask 0x63F) and stress-events.json.

Channel order (Windows 7.1): FL FR FC LFE BL BR SL SR."""
import glob
import json
import os
import subprocess
import sys

import numpy as np

R = 48000
KENNEY = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'kenney')
RING = [(0, 2), (30, 1), (90, 7), (150, 5), (210, 4), (270, 6), (330, 0)]   # az -> channel
SPEAKERS = {'FL': (0, 330), 'FR': (1, 30), 'FC': (2, 0), 'BL': (4, 210), 'BR': (5, 150), 'SL': (6, 270), 'SR': (7, 90)}

_cache = {}


def sound(name):
    """Mono float at 48 kHz, trimmed of leading/trailing silence, peak 1."""
    if name in _cache:
        return _cache[name]
    if name.startswith('synth:'):
        _cache[name] = synth(name[6:])
        return _cache[name]
    path = glob.glob(f'{KENNEY}/**/{name}.ogg', recursive=True) if '/' not in name else glob.glob(f'{KENNEY}/{name}.ogg')
    if not path:
        raise SystemExit(f'no sound {name}')
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path[0], '-ac', '1', '-ar', str(R), '-f', 'f32le', '-'],
                         check=True, capture_output=True).stdout
    x = np.frombuffer(raw, '<f4').astype(np.float64)
    loud = np.nonzero(np.abs(x) > np.abs(x).max() * 0.01)[0]
    x = x[max(loud[0] - 48, 0):loud[-1] + 480]
    x /= np.abs(x).max()
    _cache[name] = x
    return x


def synth(kind):
    """Our own measuring sounds: pink noise (1.5 s) and a log sine sweep 100 Hz - 16 kHz (3 s)."""
    rng = np.random.default_rng(1)
    if kind == 'pink':
        n = int(1.5 * R)
        X = np.fft.rfft(rng.standard_normal(n))
        f = np.fft.rfftfreq(n, 1 / R)
        X[1:] /= np.sqrt(f[1:])
        X[f < 40] = 0
        x = np.fft.irfft(X, n)
    else:
        n = int(3 * R)
        t = np.arange(n) / R
        k = np.log(16000 / 100)
        x = np.sin(2 * np.pi * 100 * 3 / k * (np.exp(t / 3 * k) - 1))
    fade = 960
    x[:fade] *= np.linspace(0, 1, fade)
    x[-fade:] *= np.linspace(1, 0, fade)
    return x / np.abs(x).max()


def pan_gains(az):
    g = np.zeros(8)
    az %= 360
    for i in range(7):
        a, ch = RING[i]
        b = 360 if i == 6 else RING[i + 1][0]
        if a <= az < b:
            f = (az - a) / (b - a)
            g[ch] += np.cos(f * np.pi / 2)
            g[RING[(i + 1) % 7][1]] += np.sin(f * np.pi / 2)
            return g
    return g


class Mix:
    def __init__(self, seconds):
        self.out = np.zeros((int(seconds * R), 8))
        self.events = []

    def add(self, t, name, az, db=0.0, section='', az_end=None, length=None, channel=None, loop=False):
        x = sound(name)
        if length is not None:
            n = int(length * R)
            x = np.tile(x, n // len(x) + 1)[:n] if loop else x[:n]
            fade = min(2400, len(x) // 4)
            x = x.copy()
            x[:fade] *= np.linspace(0, 1, fade)
            x[-fade:] *= np.linspace(1, 0, fade)
        x = x * 10 ** (db / 20)
        s = int(t * R)
        n = min(len(x), len(self.out) - s)
        if channel is not None:
            self.out[s:s + n, channel] += x[:n]
        elif az_end is None:
            self.out[s:s + n] += x[:n, None] * pan_gains(az)[None, :]
        else:
            # moving: gains per block of 256 frames, the direction going linearly from az to az_end
            for b in range(0, n, 256):
                a = az + (az_end - az) * (b + 128) / n
                e = min(b + 256, n)
                self.out[s + b:s + e] += x[b:e, None] * pan_gains(a)[None, :]
        self.events.append({'t': round(t, 4), 'len': round(n / R, 4), 'sound': name, 'az': az, 'az_end': az_end,
                            'db': db, 'section': section, 'channel': channel})
        return t + n / R


def build():
    rng = np.random.default_rng(7)
    m = Mix(330)
    t = 0.5

    # 1. each speaker alone (the same footstep), and the LFE with a low explosion
    for name, (ch, az) in SPEAKERS.items():
        m.add(t, 'footstep_concrete_000', az, -6, 'speakers', channel=ch)
        t += 0.8
    m.add(t, 'lowFrequency_explosion_000', 0, -6, 'speakers', channel=3)
    t += 2.5

    # 2. footsteps walking around on five surfaces, 24 steps a lap
    for k, surf in enumerate(['concrete', 'wood', 'grass', 'snow', 'carpet']):
        for i in range(24):
            az = (i * 15 * (1 if k % 2 == 0 else -1)) % 360
            m.add(t, f'footstep_{surf}_{i % 5:03d}', az, -8, 'footsteps')
            t += 0.3
        t += 0.6

    # 3. pitch: low, mid, high sounds at 8 directions
    for name, db in [('impactPunch_heavy_000', -6), ('explosionCrunch_000', -10), ('impactWood_heavy_000', -6),
                     ('impactPlank_medium_000', -6), ('impactMetal_medium_000', -8), ('impactGlass_light_000', -8),
                     ('impactBell_heavy_000', -12), ('click_003', -6), ('laserSmall_000', -12), ('tick_002', -6)]:
        for i in range(8):
            m.add(t, name, i * 45, db, 'pitch')
            t += 0.45
        t += 0.4

    # 4. loudness ladder: the same hit from 0 to -40 dB, front, right side, back left
    for az in (0, 90, 210):
        for db in (0, -10, -20, -30, -40):
            m.add(t, 'impactMetal_heavy_000', az, db - 6, 'loudness')
            t += 0.7
        t += 0.5

    # 5. fine angles: a click every 15 degrees, between speakers too
    for i in range(24):
        m.add(t, 'metalClick', i * 15, -6, 'angles')
        t += 0.35
    t += 0.6

    # 6. moving: engines circling slowly and fast, a force field the other way, a fly-by front to back
    t = m.add(t, 'engineCircular_000', 0, -10, 'moving', az_end=360, length=10, loop=True) + 0.5
    t = m.add(t, 'spaceEngineLow_002', 0, -10, 'moving', az_end=-720, length=6, loop=True) + 0.5
    t = m.add(t, 'forceField_000', 90, -12, 'moving', az_end=-270, length=8, loop=True) + 0.5
    t = m.add(t, 'thrusterFire_000', 20, -10, 'moving', az_end=160, length=3, loop=True) + 0.5
    t = m.add(t, 'thrusterFire_001', 340, -10, 'moving', az_end=200, length=3, loop=True) + 0.8

    # 7. a busy scene: several sounds at once, random directions and levels, an engine circling under it
    busy = ['footstep_wood_001', 'impactGlass_medium_001', 'laserRetro_002', 'impactPunch_medium_001', 'doorClose_001',
            'zap1', 'impactPlate_light_002', 'chop', 'drawKnife1', 'explosionCrunch_002', 'switch_005', 'bookPlace1',
            'impactTin_medium_000', 'laserLarge_001', 'impactSoft_heavy_000', 'pluck_001', 'metalPot1', 'creak1']
    start = t
    m.add(start, 'spaceEngine_000', 0, -20, 'busy', az_end=720, length=25, loop=True)
    while t < start + 24:
        m.add(t, busy[rng.integers(len(busy))], float(rng.integers(0, 24) * 15), float(rng.uniform(-24, -4)), 'busy')
        t += float(rng.uniform(0.08, 0.5))
    t = start + 26

    # 8. long sounds alone, with silence after: bell, glass, door, explosion
    for name, az in [('impactBell_heavy_002', 45), ('impactGlass_heavy_001', 135), ('doorOpen_1', 270),
                     ('explosionCrunch_004', 180), ('impactBell_heavy_003', 300), ('creak3', 90)]:
        m.add(t, name, az, -6, 'tails')
        t += 2.2

    # 9. steady pink noise from 24 directions (clean spectra per direction)
    for i in range(24):
        m.add(t, 'synth:pink', i * 15, -14, 'noise')
        t += 1.8

    # 10. sine sweeps 100 Hz - 16 kHz: front, the speakers, and between them
    for az in (0, 30, 60, 90, 120, 150, 180, 270):
        m.add(t, 'synth:sweep', az, -16, 'sweeps')
        t += 3.4

    # 11. voices (male and female call-outs) from all around
    lines = ['war_look_out', 'war_reloading', 'war_cover_me', 'war_get_down', 'war_go_go_go', 'war_medic',
             'war_sniper', 'war_watch_my_back', 'war_fire_in_the_hole', 'war_call_for_backup', 'war_target_engaged',
             'war_rpg']
    for i, line in enumerate(lines * 2):
        voice = 'Male' if i % 2 == 0 else 'Female'
        t = m.add(t, f'voiceover-pack/{voice}/{line}', (i * 105) % 360, -8, 'voices') + 0.35

    # 12. a louder, denser scene: footsteps running around, gunfire-like hits, voices over it
    start = t
    m.add(start, 'spaceEngineLarge_001', 180, -18, 'busy-loud', az_end=-180, length=22, loop=True)
    step = 0
    while t < start + 21:
        m.add(t, f'footstep_concrete_{step % 5:03d}', (step * 20) % 360, -10, 'busy-loud')
        if step % 3 == 0:
            m.add(t + 0.05, busy[rng.integers(len(busy))], float(rng.integers(0, 24) * 15), float(rng.uniform(-12, 0)), 'busy-loud')
        if step % 11 == 5:
            m.add(t, f'voiceover-pack/Male/{lines[rng.integers(len(lines))]}', float(rng.integers(0, 24) * 15), -6, 'busy-loud')
        step += 1
        t += 0.22
    t = start + 23

    m.out = m.out[:int((t + 1) * R)]
    peak = np.abs(m.out).max()
    m.out *= 0.5 / peak                         # headroom: no limiter anywhere should act
    for e in m.events:
        e['db'] = round(e['db'] + 20 * np.log10(0.5 / peak), 2)
    return m


def write_wav(path, x):
    import struct
    data = x.astype('<f4').tobytes()
    fmt = struct.pack('<HHIIHHHHIH14s', 0xFFFE, 8, R, R * 32, 32, 32, 22, 32, 0x63F, 3,
                      b'\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71')
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 4 + 8 + len(fmt) + 8 + len(data)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<I', len(fmt)) + fmt)
        f.write(b'data' + struct.pack('<I', len(data)) + data)


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    m = build()
    write_wav(os.path.join(out, 'stress-7.1.wav'), m.out)
    m.out.astype('<f4').tofile(os.path.join(out, 'stress-7.1.f32'))
    json.dump(m.events, open(os.path.join(out, 'stress-events.json'), 'w'), indent=0)
    sections = {}
    for e in m.events:
        sections.setdefault(e['section'], [0, e['t']])[0] += 1
    print('%.1f s, %d events:' % (len(m.out) / R, len(m.events)),
          ', '.join(f'{k} {v[0]} (from {v[1]:.0f} s)' for k, v in sections.items()))
