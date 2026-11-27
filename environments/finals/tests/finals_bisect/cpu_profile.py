#!/usr/bin/env python3
"""Where THE FINALS spends its CPU time, launched from Steam (SteamDriver tool), 2026-11-25.

    python3 tests/finals_bisect/cpu_profile.py record [--seconds 120]   # while playing
    python3 tests/finals_bisect/cpu_profile.py analyze [trace]          # newest trace by default

record attaches Instruments' Time Profiler (kernel sampling of on-CPU threads at 1 kHz, no thread
suspension, the tool freeze_profile.py uses) to the running game process (the newest
...\\Binaries\\Win64\\Discovery*.exe) for the given time, into
<compatdata>/cpu-profile/<date>.trace.

analyze breaks the samples of the busiest threads down by where the leaf PC is: the game's own
modules, Wine's Windows-side DLLs, Wine's Unix side, D3DMetal/GPTK, Rosetta, macOS libraries,
the kernel. Windows addresses are attributed to the nearest module base below them from the Wine
log's +loaddll lines. Addresses in the anti-cheat's region (0x7000_0000_0000 and up) are only
counted, never broken down (CLAUDE.md hard boundary 3); neither are the game's own functions
named beyond their module (we never patch or analyse the game's code).
"""
import argparse
import bisect
import collections
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from freeze_profile import ENV, samples, AC_LO, AC_HI, _XmlCleanReader  # noqa: E402

COMPAT = Path.home() / 'Library/Application Support/Steam/steamapps/compatdata/2073850'
OUT = COMPAT / 'cpu-profile'
WINE_LOG = COMPAT / 'steamdriver-wine.log'
LOADDLL = re.compile(r'^(?:[\d.]+:)?(?:[0-9a-f]{4}:)?([0-9a-f]{4}):trace:loaddll:\w+ Loaded L"([^"]+)" at ([0-9A-Fa-f]+)')
GAME_DIR = 'the finals\\'


def game_pid():
    out = subprocess.run(['ps', '-axo', 'pid=,lstart=,command='], capture_output=True, text=True).stdout
    best = None
    for line in out.splitlines():
        if re.search(r'Binaries\\Win64\\Discovery[^\\]*\.exe', line):
            pid = int(line.split()[0])
            best = pid if best is None or pid > best else best
    return best


def record(a):
    pid = game_pid()
    if not pid:
        sys.exit('THE FINALS is not running')
    OUT.mkdir(exist_ok=True)
    trace = OUT / (time.strftime('%Y%m%d-%H%M%S') + '.trace')
    print('recording pid %d for %d s -> %s' % (pid, a.seconds, trace), flush=True)
    r = subprocess.run(['xcrun', 'xctrace', 'record', '--template', 'Time Profiler', '--attach', str(pid),
                        '--time-limit', '%ds' % a.seconds, '--output', str(trace)], env=ENV)
    print('done (rc %d)' % r.returncode)
    return r.returncode


def modules():
    """Module bases from the loads after the game exe's last load (first base seen per name)."""
    lines = WINE_LOG.read_bytes().decode('utf-8', 'replace').splitlines()
    start = 0
    for i, line in enumerate(lines):
        if 'loaddll' in line and '\\Binaries\\Win64\\Discovery' in line and '.exe"' in line:
            start = i
    seen = {}
    for line in lines[start:]:
        m = LOADDLL.match(line)
        if m:
            path = m.group(2).replace('\\\\', '\\')
            name = path.rsplit('\\', 1)[-1]
            game = GAME_DIR in path.lower()
            seen.setdefault((int(m.group(3), 16)), ('game:' if game else 'pe:') + name)
    mods = sorted(seen.items())
    return [b for b, _ in mods], [n for _, n in mods]


def category(addr, binary, bases, names):
    """(category, detail) of one frame."""
    if binary:
        b = binary.lower()
        if b.startswith('d3dmetal') or 'd3dshared' in b or b.startswith('libd3d') or b == 'd3dmetal':
            return 'D3DMetal/GPTK', binary
        if 'rosetta' in b or b in ('runtime', 'libRosettaRuntime'.lower()):
            return 'Rosetta', binary
        if b.endswith('.so') or b in ('wine', 'wine-preloader', 'wineserver') or 'macfinals' in b:
            return 'Wine (Unix side)', binary
        if b in ('mach_kernel', 'kernel', 'kernel.release.t6041', 'kernel.release.t6020') or b.startswith('kernel'):
            return 'kernel', binary
        if 'metal' in b or b.startswith('agx') or 'iogpu' in b:
            return 'Metal/GPU driver', binary
        return 'macOS libraries', binary
    if AC_LO <= addr < AC_HI:
        return 'anti-cheat region', ''
    i = bisect.bisect_right(bases, addr) - 1
    if i >= 0 and addr - bases[i] < 0x10000000:
        n = names[i]
        if n.startswith('game:'):
            return 'game modules', n[5:]
        return 'Wine (Windows side)', n[3:]
    if addr >= 0x7ff000000000:
        return 'unattributed (high, likely Rosetta JIT/runtime)', ''
    return 'unattributed', ''


def core_split(xml_path):
    """Samples per thread group (numbered workers merged) and core type (S, P, E)."""
    import xml.etree.ElementTree as ET
    ids, res = {}, collections.defaultdict(collections.Counter)
    for _, el in ET.iterparse(_XmlCleanReader(xml_path), events=('end',)):
        i = el.get('id')
        if i is not None:
            ids[i] = el
        if el.tag != 'row':
            continue
        th = core = None
        for c in el:
            r = c.get('ref')
            c = ids[r] if r is not None else c
            if c.tag == 'thread':
                th = c.get('fmt', '')
            elif c.tag == 'core':
                core = c.get('fmt', '')
        group = re.sub(r'#\d+', '#N', re.sub(r'\s*\(0x[0-9a-f]+\).*$', '', th or '?').strip())
        m = re.search(r'\((\w) Core\)', core or '')
        res[group][m.group(1) if m else '?'] += 1
        el.clear()
    return res


def analyze(a):
    trace = Path(a.trace) if a.trace else max(OUT.glob('*.trace'), key=lambda p: p.stat().st_mtime)
    bases, names = modules()
    print('trace %s, %d Windows modules from the Wine log' % (trace, len(bases)))
    per_thread = collections.defaultdict(list)
    for t, th, frames in samples(trace, trace.with_suffix('.xml')):
        name = re.sub(r'\s*0x[0-9a-f]+.*$', '', th or '?').strip() or '?'
        per_thread[name].append(frames)
    total = sum(len(v) for v in per_thread.values())
    dur = None
    print('%d samples in all (%.1f s of CPU)\n' % (total, total / 1000))
    busiest = sorted(per_thread.items(), key=lambda kv: -len(kv[1]))
    print('threads by CPU (samples = ms on CPU)')
    for name, rows in busiest[:a.threads + 6]:
        print('  %7d  %5.1f%%  %s' % (len(rows), 100.0 * len(rows) / total, name))

    def breakdown(title, rows):
        cat, det, wine_caller = collections.Counter(), collections.Counter(), collections.Counter()
        for frames in rows:
            if not frames:
                cat['[no stack]'] += 1
                continue
            a0, n0, b0 = frames[0]
            c, d = category(a0, b0, bases, names)
            cat[c] += 1
            if c not in ('game modules', 'anti-cheat region'):
                label = '%s %s' % (d, n0 if n0 and not n0.startswith('0x') else '')
                det['[%s] %s' % (c, label.strip())] += 1
            # Time outside the game: which game-facing module called into it (first non-macOS
            # frame up the stack), to tell Wine's own work from work the game asked for.
        n = len(rows)
        print('\n== %s: %d samples' % (title, n))
        for k, v in cat.most_common():
            print('    %5.1f %%  %6d  %s' % (100.0 * v / n, v, k))
        print('  -- top leaves outside the game')
        for k, v in det.most_common(a.top):
            print('    %5.1f %%  %6d  %s' % (100.0 * v / n, v, k))

    cores = core_split(trace.with_suffix('.xml'))
    print('\ncore type per thread group (S = the fastest cores)')
    for name, c in sorted(cores.items(), key=lambda kv: -sum(kv[1].values()))[:12]:
        n = sum(c.values())
        print('  %-32s %7d  ' % (name, n) + '  '.join('%s %.0f%%' % (k, 100.0 * v / n) for k, v in c.most_common()))

    for name, rows in busiest[:a.threads]:
        breakdown(name, rows)
    breakdown('whole process', [f for rows in per_thread.values() for f in rows])


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('mode', choices=('record', 'analyze'))
    p.add_argument('trace', nargs='?')
    p.add_argument('--seconds', type=int, default=120)
    p.add_argument('--threads', type=int, default=5, help='analyze: busiest threads broken down')
    p.add_argument('--top', type=int, default=15)
    a = p.parse_args()
    return record(a) if a.mode == 'record' else analyze(a)


if __name__ == '__main__':
    sys.exit(main())
