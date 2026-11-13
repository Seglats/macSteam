#!/usr/bin/env python3
"""Each game frame from present to screen, from a GPU=1 FULL=1 game_perf_trace.sh recording.

    python3 tests/finals_bisect/gpu_frames.py ~/Desktop/kovaaks-gpu-full-164224.trace [more traces]

Per frame, all on Instruments' clock:
    request    the game's process asked Core Animation to present (ca-client-present-request)
    GPU        the frame's GPU work (metal-gpu-intervals with that frame number), and the end
               of the command buffer that carries the present
    composite  the first WindowServer GPU pass reading surfaces after that end
    screen     the frame on the display (displayed-surfaces-interval, "Frame N")

A held frame stays on screen half a typical frame or more longer than usual; the frame after it
is the late one, and the script shows which step made it late. The game is the busiest process
whose presents are recorded (KovaaK's on D3DMetal is FPSAimTrainer-..., on DXMT "wine").
"""
import bisect
import collections
import os
import re
import statistics as st
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import frame_pacing as fp  # noqa: E402

TABLES = ['metal-gpu-intervals', 'displayed-surfaces-interval', 'ca-client-present-request']


def q(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))] if v else float('nan')


def line(name, v):
    if not v:
        return f'  {name:<30} -'
    return (f'  {name:<30} median {st.median(v):5.2f}  p90 {q(v, .9):5.2f}  p99 {q(v, .99):5.2f}'
            f'  max {max(v):6.2f} ms')


def report(trace):
    folder = tempfile.mkdtemp()
    data = {t: fp.export(trace, t, folder) for t in TABLES}
    requests = data['ca-client-present-request']
    if not requests:
        print(trace, ': no present requests (record with FULL=1)')
        return
    game = collections.Counter(r['process'][1] for r in requests).most_common(1)[0][0]
    pid = re.search(r'\((\d+)\)', game).group(1)
    print(f'{os.path.basename(trace)}: game process {game}')

    frames = collections.defaultdict(lambda: {'s': None, 'e': None})
    cb_end, composites = {}, []
    for r in data['metal-gpu-intervals']:
        proc = r['process'][1] or ''
        s = int(r['start'][0])
        e = s + int(r['duration'][0])
        if proc == game:
            f = frames[r['frame-number'][1]]
            f['s'] = s if f['s'] is None else min(f['s'], s)
            f['e'] = e if f['e'] is None else max(f['e'], e)
            cb = r['cmdbuffer-id'][1]
            cb_end[cb] = max(cb_end.get(cb, 0), e)
            f.setdefault('cbs', set()).add(cb)
        elif proc.startswith('WindowServer') and 'Read Surface' in (r['event-label'][1] or ''):
            composites.append(s)
    composites.sort()
    shown = {}
    for r in data['displayed-surfaces-interval']:
        m = re.search(r'\((\d+)\):Frame (\d+)', r['event-label'][1] or '')
        if m and m.group(1) == pid:
            shown['Frame ' + m.group(2)] = (int(r['start'][0]), int(r['duration'][0]) / 1e6)
    req_by_cb = {r['cmdbuffer-id'][1]: int(r['timestamp'][0]) for r in requests}

    rows = []
    for name, f in frames.items():
        if name not in shown or f['s'] is None:
            continue
        cbs = [cb for cb in f.get('cbs', ()) if cb in req_by_cb]
        if not cbs:
            continue
        cb = cbs[0]
        req, pend = req_by_cb[cb], cb_end[cb]
        i = bisect.bisect_left(composites, pend)
        comp = composites[i] if i < len(composites) else None
        on, dur = shown[name]
        rows.append(dict(n=int(name.split()[1]), req=req, gs=f['s'], ge=f['e'], pend=pend, comp=comp,
                         on=on, dur=dur))
    rows.sort(key=lambda r: r['n'])
    if len(rows) < 20:
        print('  too few matched frames', len(rows))
        return
    ms = lambda a, b: (b - a) / 1e6  # noqa: E731
    span = ms(rows[0]['on'], rows[-1]['on']) / 1000
    print(f'  {len(rows)} frames matched in {span:.1f} s ({len(rows) / span:.0f}/s)')
    print(line('GPU work (frame start->end)', [ms(r['gs'], r['ge']) for r in rows]))
    print(line('present request -> its GPU end', [ms(r['req'], r['pend']) for r in rows]))
    print(line('GPU end -> composite', [ms(r['pend'], r['comp']) for r in rows if r['comp']]))
    print(line('composite -> on screen', [ms(r['comp'], r['on']) for r in rows if r['comp'] and r['on'] >= r['comp']]))
    print(line('present request -> on screen', [ms(r['req'], r['on']) for r in rows]))
    print(line('time on screen', [r['dur'] for r in rows if r['dur'] < 100]))
    typical = st.median(r['dur'] for r in rows)
    held = [i for i, r in enumerate(rows[:-1]) if typical * 1.4 <= r['dur'] < 100]
    print(f'  held (>= {typical * 1.4:.1f} ms on screen): {len(held)} ({100 * len(held) / len(rows):.1f} %)')
    if not held:
        return
    # What made the next frame late, compared with that step's usual value.
    usual = {
        'request -> GPU end': st.median(ms(r['req'], r['pend']) for r in rows),
        'GPU end -> composite': st.median(ms(r['pend'], r['comp']) for r in rows if r['comp']),
        'composite -> screen': st.median(ms(r['comp'], r['on']) for r in rows if r['comp'] and r['on'] >= r['comp']),
    }
    blame = collections.Counter()
    for i in held:
        r = rows[i + 1]
        if not r['comp']:
            continue
        steps = {'request -> GPU end': ms(r['req'], r['pend']), 'GPU end -> composite': ms(r['pend'], r['comp']),
                 'composite -> screen': ms(r['comp'], r['on'])}
        worst = max(steps, key=lambda k: steps[k] - usual[k])
        blame[worst] += 1
        gap = ms(rows[i]['req'], r['req'])
        if gap > typical * 1.4:
            blame['(game presented late)'] += 1
    print('  late step after each held frame:', ', '.join(f'{k}: {v}' for k, v in blame.most_common()))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for t in sys.argv[1:]:
        report(os.path.expanduser(t))
        print()
