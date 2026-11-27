#!/usr/bin/env python3
"""Watch a THE FINALS session for a frozen frame pump and capture it with Time Profiler.

    python3 tests/finals_bisect/freeze_watch.py /tmp/finals-stress [--seconds 70]
                                                 [--poll 1.5] [--no-record] [--replay]

<stats-prefix> is WINEMAC_INPUT_STATS from the launch options; the Mac driver writes
<prefix>-<pid>.jsonl, one row a second, while the game runs. A freeze is: the newest row is
fresh (the driver's stats thread is alive), the game is frontmost (app_active), and no frame was
shown for the last 6 seconds. The watcher arms only after frames have flowed, so game startup
and restarts don't fire. On a freeze it attaches Instruments' Time Profiler to the game for
--seconds (kernel-side sampling at 1 kHz, no thread suspension — the method cpu_profile.py and
freeze_profile.py proved on this game), capturing the frozen threads through the ~60 s until
Unreal's hang watchdog crashes the game. One capture per freeze; it re-arms once frames flow
again. Traces: <compatdata>/cpu-profile/freeze-<time>.trace.

A freeze while the game is backgrounded is not detected (a backgrounded game may legitimately
stop presenting); the STEAMDRIVER_PERF counters and the game's own crash report still cover that
case. --replay + --no-record is the offline self-test (no freshness or pid checks, no recording).
"""
import argparse
import glob
import json
import os
import re
import subprocess
import time
from pathlib import Path

COMPAT = Path.home() / 'Library/Application Support/Steam/steamapps/compatdata/2073850'
OUT = COMPAT / 'cpu-profile'
ENV = dict(os.environ, DEVELOPER_DIR=os.environ.get('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer'))
FREEZE_ROWS = 6      # default seconds without a shown frame, while frontmost (--rows to raise:
                     # under heavy CPU stress, level-transition load stalls reach 8+ s with the
                     # GameThread still working — not the hang; the hang is 60+ s and dead)
FRESH_MS = 4000      # the stats file must have been written to within this


def say(msg):
    print(time.strftime('%H:%M:%S ') + msg, flush=True)


def newest_stats(prefix):
    """The newest stats file, preferring the game's: WINEMAC_INPUT_STATS is process-global, so
    explorer.exe and other helpers write files too, and one can briefly out-new the game's
    during a window change (seen live: explorer wrote 2 rows mid-transition)."""
    files = glob.glob(glob.escape(prefix) + '-*.jsonl')
    game = [f for f in files if is_game_stats(f)]
    pool = game or files
    return max(pool, key=os.path.getmtime) if pool else None


def is_game_stats(path):
    rows = rows_of(path, 1)
    return bool(rows) and 'Discovery' in (rows[-1].get('exe') or '')


def rows_of(path, n):
    out = []
    try:
        with open(path, 'rb') as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - 262144))
            lines = f.read().decode('utf-8', 'replace').splitlines()
    except OSError:
        return out
    for line in lines[-n:]:
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return out


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('prefix', help='WINEMAC_INPUT_STATS prefix from the launch options')
    ap.add_argument('--seconds', type=int, default=70, help='Time Profiler seconds per freeze')
    ap.add_argument('--poll', type=float, default=1.5)
    ap.add_argument('--rows', type=int, default=FREEZE_ROWS,
                    help='seconds without a shown frame that count as frozen (default 6)')
    ap.add_argument('--no-record', action='store_true')
    ap.add_argument('--replay', action='store_true', help='offline self-test')
    a = ap.parse_args()

    freeze_rows = a.rows
    cur, armed, frozen, rec = '', False, False, None
    while True:
        path = newest_stats(a.prefix)
        if path != cur:
            cur, armed, frozen = path, False, False
            say('watching %s' % (path or '(no stats file yet)'))
        if path:
            rows = rows_of(path, freeze_rows + 2)
            if rows:
                last = rows[-1]
                pid = int(re.search(r'-(\d+)\.jsonl$', path).group(1))
                fresh = a.replay or (int(time.time() * 1000) - last.get('unix_ms', 0) < FRESH_MS)
                window = rows[-freeze_rows:]
                shown = any(r.get('present_shown', 0) for r in window)
                if not armed and any(r.get('present_shown', 0) for r in rows):
                    armed = True
                    say('frames flowing (pid %d)' % pid)
                if (armed and fresh and not shown and last.get('app_active')
                        and len(window) == freeze_rows and not frozen
                        and (a.replay or alive(pid))):
                    frozen = True
                    say('FREEZE: no frames for %d s while frontmost (pid %d)' % (freeze_rows, pid))
                    if not a.no_record and not a.replay:
                        OUT.mkdir(exist_ok=True)
                        trace = OUT / ('freeze-%s.trace' % time.strftime('%Y%m%d-%H%M%S'))
                        say('capturing %d s of Time Profiler -> %s' % (a.seconds, trace.name))
                        rec = subprocess.Popen(
                            ['xcrun', 'xctrace', 'record', '--template', 'Time Profiler',
                             '--attach', str(pid), '--time-limit', '%ds' % a.seconds,
                             '--output', str(trace)], env=ENV)
                if frozen and shown:
                    frozen = False
                    say('frames resumed')
        if rec is not None and rec.poll() is not None:
            say('capture finished (rc %d)' % rec.returncode)
            rec = None
        time.sleep(a.poll)


if __name__ == '__main__':
    main()
