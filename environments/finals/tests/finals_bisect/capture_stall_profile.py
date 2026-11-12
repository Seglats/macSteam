#!/usr/bin/env python3
"""Profile what THE FINALS is doing while a Metal capture fails to finish (debug runs only).

    python3 tests/finals_bisect/capture_stall_profile.py arm       # before starting the game
    python3 tests/finals_bisect/capture_stall_profile.py analyze   # newest recording again

arm waits for the next "Metal capture stopping" line in the Steam tool's Wine log, then attaches
Instruments' Time Profiler to the game for --seconds and prints a breakdown. Time Profiler is
kernel sampling of on-CPU threads, with no thread suspension and no debugger (PROFILING.md).
Blocked threads do not appear in it: this answers where the CPU goes, not who waits on whom.

Addresses in the anti-cheat's region are only counted, never broken down (CLAUDE.md, hard
boundary 3); the shared classify() from freeze_profile.py does that.
"""
import argparse
import collections
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from freeze_profile import ENV, classify, label, samples  # noqa: E402

COMPAT = Path.home() / 'Library/Application Support/Steam/steamapps/compatdata/2073850'
OUT = HERE / 'logs' / 'capture-stall'
GAME = 'Binaries.Win64.Discovery.exe'
MARK = b'Metal capture stopping'


def game_pid():
    found = subprocess.run(['pgrep', '-f', GAME], capture_output=True, text=True).stdout.split()
    return found[-1] if found else None


def wait_for_stop(log, poll=1.0):
    """Returns once a new stop line appears after the log's current end; None if the game ended."""
    seen = log.stat().st_size if log.exists() else 0
    started = False
    gone = None     # THE FINALS starts a short-lived Discovery.exe first, so a gap is normal
    while True:
        size = log.stat().st_size if log.exists() else 0
        if size > seen:
            with log.open('rb') as f:
                f.seek(seen)
                chunk = f.read()
            if MARK in chunk:
                return True
            seen = size
        pid = game_pid()
        started = started or bool(pid)
        gone = None if pid else (gone or time.monotonic())
        if started and gone and time.monotonic() - gone > 120:
            return None
        time.sleep(poll)


def record(pid, seconds):
    OUT.mkdir(parents=True, exist_ok=True)
    trace = OUT / ('%s-%s.trace' % (time.strftime('%Y%m%d-%H%M%S'), pid))
    print('recording pid %s for %d s: %s' % (pid, seconds, trace), flush=True)
    subprocess.run(['xcrun', 'xctrace', 'record', '--template', 'Time Profiler', '--attach', str(pid),
                    '--time-limit', '%ds' % seconds, '--output', str(trace)], env=ENV, check=True,
                   stdout=subprocess.DEVNULL)
    return trace


def table(title, counter, total, top):
    print('\n' + title)
    for name, n in counter.most_common(top):
        print('  %8d %5.1f%%  %s' % (n, n * 100 / total, name[:110]))


def analyze(trace, top):
    threads, leaf_bin, any_bin, leaf_sym, any_sym, stop_threads, stop_leaf = (collections.Counter() for _ in range(7))
    total = 0
    first = last = None
    for t, th, frames in samples(trace, trace.with_suffix('.xml')):
        if not frames:
            continue
        total += 1
        first = t if first is None else first
        last = t
        threads[th] += 1
        leaf = frames[0]
        leaf_bin[classify(leaf[0], leaf[2], [], [])] += 1
        leaf_sym[label(leaf[0], leaf[1], leaf[2], [], [])] += 1
        seen_bin, seen_sym = set(), set()
        for addr, name, binary in frames:
            seen_bin.add(classify(addr, binary, [], []))
            if binary:
                seen_sym.add(label(addr, name, binary, [], []))
        any_bin.update(seen_bin)
        any_sym.update(seen_sym)
        if any('stopCapture' in (name or '') for _, name, _ in frames):
            stop_threads[th] += 1
            stop_leaf[label(leaf[0], leaf[1], leaf[2], [], [])] += 1
    if not total:
        print('no samples')
        return
    span = max(last - first, 0.001)
    print('%d on-CPU samples over %.1f s (about %.0f %% of one core at 1 kHz)' % (total, span, total / span / 10))
    table('By thread', threads, total, 16)
    table('Leaf frame by binary', leaf_bin, total, 14)
    table('Anywhere on the stack, by binary', any_bin, total, 18)
    table('Leaf functions', leaf_sym, total, top)
    table('Named functions anywhere on the stack', any_sym, total, top)
    if stop_threads:
        table('Samples inside stopCapture, by thread', stop_threads, sum(stop_threads.values()), 8)
        table('Samples inside stopCapture, leaf', stop_leaf, sum(stop_leaf.values()), 20)
    else:
        print('\nNo on-CPU sample is inside stopCapture: the thread stopping the capture is blocked, not working.')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['arm', 'analyze'])
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--log', type=Path, default=COMPAT / 'steamdriver-wine.log')
    ap.add_argument('--pid', help='profile this process instead of the game (to test the tool)')
    a = ap.parse_args()
    if a.mode == 'analyze':
        traces = sorted(OUT.glob('*.trace'), key=lambda p: p.stat().st_mtime)
        if not traces:
            sys.exit('no recording in ' + str(OUT))
        analyze(traces[-1], a.top)
        return
    print('armed: waiting for "Metal capture stopping"', flush=True)
    if a.pid:
        while MARK not in a.log.read_bytes():
            time.sleep(0.2)
    elif wait_for_stop(a.log) is None:
        sys.exit('the game ended before a capture stopped')
    pid = a.pid or game_pid()
    if not pid:
        sys.exit('game process not found')
    analyze(record(pid, a.seconds), a.top)


if __name__ == '__main__':
    main()
