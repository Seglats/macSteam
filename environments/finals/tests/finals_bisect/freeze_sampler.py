#!/usr/bin/env python3
"""Stack-sample THE FINALS while it freezes (debug runs only, 2026-11-09).

SUPERSEDED by freeze_profile.py: on the game `sample` gets too few stacks and starts after
short freezes have ended (tests/rosetta_nt/NOTES.md "Freeze hunt, continued and PAUSED").

Start this, then start the game with PERF=1 (tests/finals_bisect/launch_main.sh):

    python3 tests/finals_bisect/freeze_sampler.py
    RUNTIME=macfinals-47 PERF=1 tests/finals_bisect/launch_main.sh avx msync

It waits for the next session folder in tests/finals_bisect/logs/, finds the game's perf
file (the one whose lines name GameThread), and watches it live. A freeze is GameThread
>= 90 % CPU while the whole process makes < 6000 NT calls per 0.1 s sample (normal play:
~40 000) - the signature measured in NOTES.md "Freeze hunt". After 3 such samples in a row
(0.3 s) it runs Apple's `sample` on the game for 1.5 s, once per freeze, at most --max
times, into <session>/freeze-samples/. `sample` reports translated processes in x86 terms:
caller frames are x86 return addresses (map them with the +loaddll lines of the Wine log),
the innermost frame is "Rosetta JIT" (translated code) or "Rosetta Runtime Routines".

Reads thread stacks from outside the process (task port), never Rosetta's translated-code
pages; tested on probe programs first. Pauses the game's threads briefly per sample.
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
GAME = re.compile(r'GameThread#\w+=([\d.]+)\+([\d.]+)')
CALLS = re.compile(r'^\d+\.\d+ calls=(\d+)')


def log(index, text):
    line = time.strftime('%H:%M:%S ') + text
    print(line, flush=True)
    if index:
        with index.open('a') as f:
            f.write(line + '\n')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--logs', type=Path, default=HERE / 'logs')
    p.add_argument('--max', type=int, default=8, help='samples per session (default 8)')
    p.add_argument('--duration', type=float, default=1.5, help='seconds per sample (default 1.5)')
    p.add_argument('--session', type=Path, help='attach to this (running) session folder instead of waiting for a new one')
    p.add_argument('--idle-exit', type=float, default=60, help='stop when the game perf file is quiet this long')
    a = p.parse_args()

    started = time.time()
    if a.session:
        session = a.session if a.session.is_absolute() else a.logs / a.session
    else:
        log(None, 'waiting for a new session in %s (start the game with PERF=1)' % a.logs)
        session = None
        while session is None:
            for d in sorted(a.logs.glob('main-*'), key=lambda x: x.stat().st_mtime, reverse=True):
                if d.is_dir() and (d / 'perf').is_dir() and d.stat().st_ctime >= started - 5:
                    session = d
                    break
            time.sleep(0.5)
    out = session / 'freeze-samples'
    out.mkdir(exist_ok=True)
    index = out / 'index.txt'
    log(index, 'session %s' % session.name)

    # Every process whose perf lines name GameThread is watched: THE FINALS starts a short-lived
    # Discovery.exe first (it names a GameThread too, then exits) and the real game after it.
    state, taken, last_growth = {}, 0, time.time()
    while True:
        grew = False
        for f in (session / 'perf').glob('wine.*'):
            st = state.setdefault(f, {'pos': 0, 'partial': b'', 'game': False, 'streak': 0, 'frozen': False,
                                          'backlog': bool(a.session)})
            with f.open('rb') as fh:
                fh.seek(st['pos'])
                chunk = fh.read()
            if not chunk:
                continue
            st['pos'] += len(chunk)
            if not st['game']:
                if b'GameThread#' not in chunk:
                    continue
                st['game'] = True
                log(index, 'game perf file %s (macOS pid %s)' % (f.name, f.name.split('.')[1]))
            grew = True
            if st.pop('backlog', False):        # attached mid-run: judge only new samples
                continue
            lines = (st['partial'] + chunk).split(b'\n')
            st['partial'] = lines.pop()
            for raw in lines:
                line = raw.decode('utf-8', 'replace')
                m = CALLS.match(line)
                if not m:
                    continue
                g = GAME.search(line)
                frozen = g is not None and float(g.group(1)) + float(g.group(2)) >= 90 and int(m.group(1)) < 6000
                st['streak'] = st['streak'] + 1 if frozen else 0
                if not frozen and st['frozen']:
                    st['frozen'] = False
                    log(index, 'freeze over (%s)' % f.name)
                if st['streak'] >= 3 and not st['frozen']:
                    st['frozen'] = True
                    if taken >= a.max:
                        log(index, 'freeze in %s (sample limit %d reached, not sampled)' % (f.name, a.max))
                        continue
                    taken += 1
                    pid = f.name.split('.')[1]
                    target = out / time.strftime('freeze-%02d-%%H%%M%%S.txt' % taken)
                    log(index, 'freeze detected -> sampling pid %s for %.1f s -> %s' % (pid, a.duration, target.name))
                    r = subprocess.run(['sample', pid, str(a.duration), '-mayDie', '-file', str(target)],
                                       capture_output=True, text=True)
                    tail = (r.stderr or '').strip().splitlines()
                    log(index, '  sample rc=%d%s' % (r.returncode, (' ' + tail[-1]) if tail else ''))
                    # the sample took time; skip the backlog so the next freeze is detected fresh
                    st['pos'], st['partial'], st['streak'] = f.stat().st_size, b'', 0
                    break
        if grew:
            last_growth = time.time()
        elif any(st['game'] for st in state.values()) and time.time() - last_growth > a.idle_exit:
            log(index, 'game perf files quiet for %.0f s: done (%d samples)' % (a.idle_exit, taken))
            return 0
        time.sleep(0.1)


if __name__ == '__main__':
    sys.exit(main())
