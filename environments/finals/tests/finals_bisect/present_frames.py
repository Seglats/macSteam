#!/usr/bin/env python3
"""Where frames that reached the display late lost their time, from the per-frame file.

    python3 tests/finals_bisect/present_frames.py /tmp/kov-frames-frames-12345.csv
    python3 tests/finals_bisect/present_frames.py --newest /tmp/kov-frames    # largest <prefix>-frames-*.csv

Launch with `WINEMAC_INPUT_STATS=<prefix> %command%` (macfinals-102 or later); the Mac driver then
writes <prefix>-frames-<pid>.csv next to the per-second <prefix>-<pid>.jsonl
(wine-winemac-present-frames.patch). Per frame, on one clock:

    requested  the game (through D3DMetal) asked the layer for a drawable
    acquired   it got one
    handed     D3DMetal's present of that drawable ran (its command buffer was scheduled)
    presented  the frame was on the display (-1 = never shown)

So a frame's time to the display splits into acquired -> handed (D3DMetal encoding and
submitting the frame) and handed -> presented (GPU work, then the compositor and the next
refresh). A late frame is one whose acquired -> presented is at least half a refresh longer than
usual; the script says which part grew.
"""
import csv
import glob
import os
import statistics as st
import sys


def load(path):
    frames = []
    with open(path, errors='replace') as f:
        for row in csv.DictReader(line for line in f if not line.startswith('#')):
            try:
                frames.append({k: (float(v) if k not in ('how',) else v) for k, v in row.items()})
            except (TypeError, ValueError):
                continue
    return sorted(frames, key=lambda r: r['requested'])


def q(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))] if v else float('nan')


def line(name, v):
    if not v:
        return f'  {name:<34} -'
    return (f'  {name:<34} median {st.median(v):6.2f}  p90 {q(v, .9):6.2f}  p99 {q(v, .99):6.2f}'
            f'  max {max(v):6.2f} ms')


def main(args):
    if args[:1] == ['--newest']:
        files = sorted(glob.glob(args[1] + '-frames-*.csv'), key=os.path.getsize)[-1:]
    else:
        files = args
    if not files:
        sys.exit('no per-frame file')
    for path in files:
        report(path)


def report(path):
    frames = load(path)
    print('file:', path)
    if len(frames) < 50:
        print('  too few frames')
        return
    # Play only: the game's steady rate, leaving out menus (KovaaK's caps them) and loading.
    gaps = [b['requested'] - a['requested'] for a, b in zip(frames, frames[1:])]
    typical = st.median(gaps)
    shown = [r for r in frames if r['presented'] > 0]
    print(f'  {len(frames)} frames, {len(shown)} shown, {len(frames) - len(shown)} never shown;'
          f' game frame interval median {typical:.2f} ms ({1000 / typical:.0f}/s)')

    def count(key):
        c = {}
        for r in frames:
            k = r[key] if key == 'how' else int(r[key])
            c[k] = c.get(k, 0) + 1
        return ', '.join(f'{k}: {v}' for k, v in sorted(c.items(), key=lambda kv: -kv[1]))
    print('  present call seen:', count('how'), '  (p present, t presentAtTime, m minimum duration, - not seen)')
    print('  layer: display sync', count('sync'), '| with transaction', count('transaction'),
          '| drawables', count('drawables'),
          '| size', ', '.join(sorted({f"{int(r['width'])}x{int(r['height'])}" for r in frames})))

    on = [r for r in shown if r['presented'] > r['acquired']]
    # Refresh from the display side: the smallest common step between presented times.
    steps = sorted(b['presented'] - a['presented'] for a, b in zip(shown, shown[1:]) if b['presented'] > a['presented'])
    refresh = st.median([s for s in steps if s < 1.5 * q(steps, .1)] or steps)
    print(f'  display refresh from presented times: {refresh:.2f} ms')
    print()
    print(line('game frame interval', gaps))
    print(line('wait for a drawable', [r['acquired'] - r['requested'] for r in frames]))
    timed = [r for r in on if r['handed'] > 0]
    print(line('acquired -> handed (D3DMetal)', [r['handed'] - r['acquired'] for r in timed]))
    print(line('handed -> presented (GPU+display)', [r['presented'] - r['handed'] for r in timed]))
    print(line('acquired -> presented (total)', [r['presented'] - r['acquired'] for r in on]))
    targets = [r['presented'] - r['target'] for r in timed if r['target'] > 0]
    if targets:
        print(line('presented - asked-for time', targets))

    total = [r['presented'] - r['acquired'] for r in on]
    base = st.median(total)
    late = [r for r in on if r['presented'] - r['acquired'] >= base + refresh / 2]
    normal = [r for r in on if r['presented'] - r['acquired'] < base + refresh / 2]
    print(f'\n  late frames (>= {base + refresh / 2:.2f} ms to the display, median {base:.2f}): {len(late)}'
          f' ({100 * len(late) / len(on):.1f} %)')
    if not late:
        return
    if timed:
        nd = st.median([r['handed'] - r['acquired'] for r in normal if r['handed'] > 0] or [0])
        nr = st.median([r['presented'] - r['handed'] for r in normal if r['handed'] > 0] or [0])
        lt = [r for r in late if r['handed'] > 0]
        d3d = sum(1 for r in lt if (r['handed'] - r['acquired']) - nd >= refresh / 2)
        after = sum(1 for r in lt if (r['presented'] - r['handed']) - nr >= refresh / 2)
        print(f'  of those, extra time before the handover (D3DMetal): {d3d}; after it (GPU or display): {after};'
              f' both: {sum(1 for r in lt if (r["handed"] - r["acquired"]) - nd >= refresh / 2 and (r["presented"] - r["handed"]) - nr >= refresh / 2)}')
    # Did the frame lose its refresh to the frame before it (that one took the slot)?
    index = {id(r): i for i, r in enumerate(frames)}
    queued = 0
    slow_game = 0
    for r in late:
        i = index[id(r)]
        prev = next((frames[j] for j in range(i - 1, max(-1, i - 4), -1) if frames[j]['presented'] > 0), None)
        if prev and abs((r['presented'] - prev['presented']) - refresh) < refresh / 4:
            queued += 1
        if i and frames[i]['requested'] - frames[i - 1]['requested'] > typical * 1.5:
            slow_game += 1
    print(f'  shown exactly one refresh after the frame before it (waited behind it): {queued};'
          f' came after a long game frame: {slow_game}')
    print('\n  first late frames (ms after start: requested, +acquired, +handed, +presented; previous presented):')
    for r in late[:12]:
        i = index[id(r)]
        prev = next((frames[j]['presented'] for j in range(i - 1, -1, -1) if frames[j]['presented'] > 0), -1)
        h = f"{r['handed'] - r['requested']:6.2f}" if r['handed'] > 0 else '     -'
        print(f"   {r['requested']:10.2f}  +{r['acquired'] - r['requested']:5.2f}  +{h}"
              f"  +{r['presented'] - r['requested']:6.2f}   prev shown {prev - r['requested']:+7.2f}")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
