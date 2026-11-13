#!/usr/bin/env python3
"""Frame pacing from an Instruments trace made by game_perf_trace.sh.

    python3 tests/finals_bisect/frame_pacing.py ~/Desktop/kovaaks-160fps-175208.trace [PROCESS]

Prints the game's present rate and intervals, how long each frame stayed on each display
(a frame held for two refreshes or more is a visible stutter), vsync cadence, and whether the
game went straight to the display or through WindowServer's compositor (and why not).
"""
import collections
import os
import statistics as st
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ENV = dict(os.environ, DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')


def rows(path):
    """Rows of an exported xctrace table as {column: (raw, formatted)}; resolves id/ref sharing."""
    node = ET.parse(path).getroot().find('node')
    if node is None or node.find('schema') is None:
        return
    cols = [c.find('mnemonic').text for c in node.find('schema').findall('col')]
    ids = {}

    def value(e):
        if 'ref' in e.attrib:
            return ids[e.attrib['ref']]
        v = (None, None) if e.tag == 'sentinel' else (e.text, e.attrib.get('fmt'))
        if 'id' in e.attrib:
            ids[e.attrib['id']] = v
        for child in e:
            value(child)
        return v
    for r in node.findall('row'):
        yield {c: value(e) for c, e in zip(cols, list(r))}


def export(trace, schema, folder):
    out = os.path.join(folder, schema + '.xml')
    done = subprocess.run(['xcrun', 'xctrace', 'export', '--input', trace, '--xpath',
                           f'/trace-toc/run[@number="1"]/data/table[@schema="{schema}"]', '--output', out],
                          env=ENV, capture_output=True)
    if done.returncode or not os.path.exists(out):
        return []  # not recorded (presents need FULL=1)
    return list(rows(out))


def quantile(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))]


def hist(v, edges):
    c = collections.Counter(next((e for e in edges if x < e), 'more') for x in v)
    return '  '.join(f'<{e}:{c[e]}' for e in edges) + f'  more:{c["more"]}'


def main(trace, process):
    folder = tempfile.mkdtemp()
    for x in export(trace, 'device-display-info', folder):
        print('display', x['display-id'][1], x['device-name'][1], x['resolution'][1],
              'max', x['max-refresh-rate'][1], 'Hz', 'main' if x['is-main-display'][1] == 'Yes' else '')

    presents = sorted(int(x['timestamp'][0]) for x in export(trace, 'ca-client-present-request', folder)
                      if process in (x['process'][1] or ''))
    if len(presents) > 2:
        dp = [(b - a) / 1e6 for a, b in zip(presents, presents[1:])]
        span = (presents[-1] - presents[0]) / 1e9
        print(f'\n{process} presents: {len(presents)} in {span:.1f} s ({presents[0]/1e9:.1f}-{presents[-1]/1e9:.1f} s)'
              f' = {(len(presents) - 1) / span:.1f}/s')
        print(f'  interval ms: median {st.median(dp):.2f}  p10 {quantile(dp, .1):.2f}  p90 {quantile(dp, .9):.2f}'
              f'  p99 {quantile(dp, .99):.2f}  max {max(dp):.2f}')
        print('  ', hist(dp, [3, 4, 5.5, 6.5, 7.5, 9, 11, 13, 17, 25, 50]))
        # Per-second rate, to see menus vs play and changes over the run.
        per = collections.Counter(int((t - presents[0]) / 1e9) for t in presents)
        print('   per second:', ' '.join(str(per[i]) for i in range(int(span) + 1)))
    else:
        print(f'\nno presents from {process} in this trace')

    shown = export(trace, 'displayed-surfaces-interval', folder)
    for name in sorted({x['display-name'][1] for x in shown}):
        mine = [x for x in shown if x['display-name'][1] == name]
        d = sorted((int(x['start'][0]), int(x['duration'][0]) / 1e6) for x in mine)
        dur = [b for _, b in d]
        refresh = st.median(dur)
        held = sum(1 for x in dur if x > refresh * 1.5)
        print(f'\n{name}: {len(d)} frames shown in {(d[-1][0] - d[0][0]) / 1e9:.1f} s; on screen median {refresh:.2f} ms,'
              f' {held} held for 2+ refreshes ({100 * held / len(dur):.0f} %), max {max(dur):.1f} ms')
        print('  ', hist(dur, [3, 5, 6.5, 7.5, 9, 11, 13, 17, 25, 50]))
        why = collections.Counter((x['direct-to-display'][1], x['detachment-reason'][1]) for x in mine)
        for (direct, reason), n in why.most_common(4):
            print(f'   direct to display: {direct} x{n}' + (f' ({reason})' if reason else ''))

    vs = collections.defaultdict(list)
    for x in export(trace, 'display-vsyncs-interval', folder):
        vs[x['display-name'][1]].append(int(x['timestamp'][0]))
    for name, t in vs.items():
        t.sort()
        d = [(b - a) / 1e6 for a, b in zip(t, t[1:])]
        if len(d) > 5:
            print(f'vsync {name}: interval median {st.median(d):.2f} ms')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(os.path.expanduser(sys.argv[1]), sys.argv[2] if len(sys.argv) > 2 else 'FPSAimTrainer')
