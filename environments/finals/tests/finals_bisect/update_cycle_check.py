#!/usr/bin/env python3
"""Does turning off AppKit's display-synchronized event loop deliver input sooner? (No game needed.)

    python3 tests/finals_bisect/update_cycle_check.py            # four 15-second rounds
    python3 tests/finals_bisect/update_cycle_check.py --build    # build only
    python3 tests/finals_bisect/update_cycle_check.py --analyse FILE

Builds update_cycle_check.m the way the finals Wine is built, then runs default, off, off, default,
so drift over the session cancels out. Each round: click the window, then keep moving the mouse,
clicking and tapping keys. Results go to logs/update-cycle/<date>.jsonl. Installs and changes nothing.
"""
import argparse
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / 'logs' / 'update-cycle'
EXE = OUT / 'update_cycle_check'
SDK = '/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk'
ORDER = ['default', 'off', 'off', 'default']


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(['/usr/bin/clang', '-arch', 'x86_64', '-fobjc-arc', '-O2', '-Wall', '-isysroot', SDK,
                    '-mmacosx-version-min=26.0', '-framework', 'AppKit', '-framework', 'GameController',
                    '-framework', 'QuartzCore', '-o', str(EXE), str(HERE / 'update_cycle_check.m')], check=True)


def analyse(rounds):
    print('%-8s %8s %9s %9s  %-22s %-22s %-22s' % ('mode', 'moving s', 'GC/s', 'AppKit/s', 'move ms med/p95 (n)',
                                                   'button ms med/p95 (n)', 'key ms med/p95 (n)'))
    for r in rounds:
        cells = ['%.2f / %.2f (%d)' % (r[k + '_median_ms'], r[k + '_p95_ms'], r[k + '_n']) for k in ('move', 'button', 'key')]
        print('%-8s %8.1f %9.0f %9.0f  %-22s %-22s %-22s' % (r['mode'], r['moving_s'], r['gc_per_s'], r['appkit_per_s'], *cells))
    print()
    for mode in ('default', 'off'):
        group = [r for r in rounds if r['mode'] == mode]
        if group:
            med = {k: statistics.mean(r[k + '_median_ms'] for r in group if r[k + '_n']) if any(r[k + '_n'] for r in group) else -1
                   for k in ('move', 'button', 'key')}
            print('%-8s AppKit %.0f/s of GCMouse %.0f/s; median age: move %.2f ms, button %.2f ms, key %.2f ms' % (
                mode, statistics.mean(r['appkit_per_s'] for r in group), statistics.mean(r['gc_per_s'] for r in group),
                med['move'], med['button'], med['key']))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', action='store_true')
    ap.add_argument('--analyse', type=Path)
    a = ap.parse_args()
    if a.analyse:
        analyse([json.loads(line) for line in a.analyse.read_text().splitlines() if line.strip()])
        return
    build()
    if a.build:
        return
    path = OUT / (time.strftime('%Y%m%d-%H%M%S') + '.jsonl')
    rounds = []
    for i, mode in enumerate(ORDER):
        print('round %d of %d' % (i + 1, len(ORDER)), flush=True)
        done = subprocess.run([str(EXE)], env={'UPDATE_CYCLE': mode, 'HOME': str(Path.home())}, capture_output=True, text=True)
        if done.returncode:
            sys.exit('round %d failed (%d): %s' % (i + 1, done.returncode, done.stderr[-400:]))
        rounds.append(json.loads(done.stdout.strip().splitlines()[-1]))
        with path.open('a') as f:
            f.write(json.dumps(rounds[-1]) + '\n')
    analyse(rounds)
    print('\nsaved', path)


if __name__ == '__main__':
    main()
