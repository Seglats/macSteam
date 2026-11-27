#!/usr/bin/env python3
"""Capture THE FINALS' NtProtectVirtualMemory bursts with Time Profiler.

    python3 tests/finals_bisect/burst_watch.py [--threshold 500] [--seconds 30] [--count 1]

Follows the newest STEAMDRIVER_PERF session's game file (perf/<session>/wine.<pid> of the newest
Discovery*.exe) and starts a Time Profiler capture of the game the moment one 100 ms counter row
shows the GameThread making at least --threshold NtProtectVirtualMemory calls (500 per row =
5,000 a second; steady play makes almost none). The bursts come at transitions - leaving the
lobby or the practice range, loading in - with the GameThread ~90 % busy in protect/exception
cycles (tests/rosetta_nt/R26_CPU_PROFILE.md, 2026-11-25). Traces go to
<compatdata>/cpu-profile/burst-<time>.trace; analyze them with cpu_profile.py analyze.
"""
import argparse
import os
import re
import subprocess
import time
from pathlib import Path

COMPAT = Path.home() / 'Library/Application Support/Steam/steamapps/compatdata/2073850'
PERF = COMPAT / 'perf'
OUT = COMPAT / 'cpu-profile'
ENV = dict(os.environ, DEVELOPER_DIR=os.environ.get('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer'))
ROW = re.compile(r'^\d+\.\d+ calls=')
PROTECT = re.compile(r'GameThread#\w+>NtProtectVirtualMemory:(\d+):')


def say(msg):
    print(time.strftime('%H:%M:%S ') + msg, flush=True)


def game_pid():
    out = subprocess.run(['pgrep', '-f', r'Binaries\\Win64\\Discovery'], capture_output=True, text=True).stdout.split()
    return max(map(int, out)) if out else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--threshold', type=int, default=500, help='GameThread protects per 100 ms row')
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--count', type=int, default=1, help='captures before exiting')
    a = ap.parse_args()

    done, path, pos = 0, None, 0
    while done < a.count:
        pid = game_pid()
        if not pid:
            time.sleep(1)
            continue
        sessions = sorted(PERF.iterdir(), key=lambda p: p.stat().st_mtime) if PERF.exists() else []
        want = sessions[-1] / ('wine.%d' % pid) if sessions else None
        if want != path:
            if not (want and want.exists()):
                time.sleep(1)
                continue
            path, pos = want, want.stat().st_size  # only rows from now on
            say('watching %s' % path)
        with open(path, 'rb') as f:
            f.seek(pos)
            chunk = f.read()
        cut = chunk.rfind(b'\n') + 1
        pos += cut
        for line in chunk[:cut].decode('utf-8', 'replace').splitlines():
            if not ROW.match(line):
                continue
            m = PROTECT.search(line)
            if m and int(m.group(1)) >= a.threshold:
                OUT.mkdir(exist_ok=True)
                trace = OUT / ('burst-%s.trace' % time.strftime('%Y%m%d-%H%M%S'))
                say('burst: %s GameThread protects in 100 ms; capturing %d s -> %s'
                    % (m.group(1), a.seconds, trace.name))
                rc = subprocess.run(['xcrun', 'xctrace', 'record', '--template', 'Time Profiler',
                                     '--attach', str(pid), '--time-limit', '%ds' % a.seconds,
                                     '--output', str(trace)], env=ENV,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
                say('capture finished (rc %d)' % rc)
                done += 1
                pos = path.stat().st_size  # skip the rows written during the capture
                break
        time.sleep(0.5)


if __name__ == '__main__':
    main()
