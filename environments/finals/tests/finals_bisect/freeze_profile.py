#!/usr/bin/env python3
"""Profile THE FINALS continuously and break down where GameThread spends its freezes
(debug runs only, 2026-11-09).

    python3 tests/finals_bisect/freeze_profile.py record      # then start the game:
    RUNTIME=macfinals-47 PERF=1 tests/finals_bisect/launch_main.sh avx msync
    python3 tests/finals_bisect/freeze_profile.py analyze     # newest session, after the game

record waits for the next session in tests/finals_bisect/logs/ (or --session), and attaches
Instruments' Time Profiler (`xctrace record --attach`) to every process whose perf lines name
GameThread (THE FINALS starts a short-lived Discovery.exe first, then the real game). It
records until the process exits (or --time-limit) into <session>/freeze-profile/<pid>.trace.
Time Profiler is kernel-side sampling at 1 kHz of on-CPU threads: no thread suspension, no
need for developer mode, and stacks of translated processes come out in x86 terms.

analyze finds the freezes in the perf file (GameThread >= 90 % while the process makes < 6000
NT calls per 0.1 s, as in NOTES.md "Freeze hunt"), cuts GameThread's samples to those windows
and compares them with the rest of the run: which module the leaf PC is in, the top leaf
functions/addresses, and the first named caller. Windows addresses are attributed to the
nearest module base below them from the Wine log's +loaddll lines (no sizes are logged, so
far-off addresses are marked '?'). Addresses in the anti-cheat's region (0x7000_0000_0000 +)
are only counted as such, never broken down (CLAUDE.md hard boundary 3).
"""
import argparse
import bisect
import collections
import datetime
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
GAME = re.compile(r'GameThread#\w+=([\d.]+)\+([\d.]+)')
CALLS = re.compile(r'^(\d+\.\d+) calls=(\d+)')
LOADDLL = re.compile(r'^[\d.]+:([0-9a-f]{4}):[0-9a-f]{4}:trace:loaddll:\w+ Loaded L"([^"]+)" at ([0-9A-Fa-f]+)')
AC_LO, AC_HI = 0x700000000000, 0x710000000000
ENV = dict(os.environ, DEVELOPER_DIR=os.environ.get('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer'))


def say(text):
    print(time.strftime('%H:%M:%S ') + text, flush=True)


def find_session(logs, session, wait):
    if session:
        return session if session.is_absolute() else logs / session
    if not wait:
        dirs = [d for d in logs.glob('main-*') if d.is_dir() and (d / 'perf').is_dir()]
        return max(dirs, key=lambda d: d.stat().st_mtime)
    started = time.time()
    say('waiting for a new session in %s (start the game with PERF=1)' % logs)
    while True:
        for d in sorted(logs.glob('main-*'), key=lambda x: x.stat().st_mtime, reverse=True):
            if d.is_dir() and (d / 'perf').is_dir() and d.stat().st_ctime >= started - 5:
                return d
        time.sleep(0.5)


# ---------------------------------------------------------------- record

def record(a):
    session = find_session(a.logs, a.session, wait=a.session is None)
    out = session / 'freeze-profile'
    out.mkdir(exist_ok=True)
    say('session %s' % session.name)
    # kperf (the kernel sampler behind Time Profiler) serves one recording at a time: a second
    # xctrace fails with "could not lock kperf". So record only the newest game process: stop
    # the previous recording (SIGINT = xctrace saves and exits), then start, retrying the lock.
    def start(pid):
        trace = out / ('%s.trace' % pid)
        logp = out / ('%s.xctrace.log' % pid)
        if trace.exists():
            shutil.rmtree(trace)                # our own earlier failed attempt
        with logp.open('w') as logf:
            p = subprocess.Popen(['xcrun', 'xctrace', 'record', '--template', 'Time Profiler',
                                  '--attach', pid, '--time-limit', '%ds' % a.time_limit,
                                  '--output', str(trace)],
                                 stdout=logf, stderr=subprocess.STDOUT, env=ENV)
        deadline = time.time() + 10
        while time.time() < deadline and p.poll() is None and 'Ctrl-C' not in logp.read_text():
            time.sleep(0.1)
        say('game process %s: recording Time Profiler -> %s' % (pid, trace.relative_to(session)))
        return p

    def kperf_busy(pid):
        return 'lock kperf' in (out / ('%s.xctrace.log' % pid)).read_text()

    def stop(pid, p):
        if p.poll() is None:
            p.send_signal(signal.SIGINT)
            try:
                p.wait(60)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        say('recording of %s ended (rc %d)' % (pid, p.returncode))

    seen, cur, last_game = set(), None, None
    while True:
        for f in sorted((session / 'perf').glob('wine.*'), key=lambda x: int(x.name.split('.')[1])):
            if f in seen:
                continue
            try:
                if b'GameThread#' not in f.read_bytes():
                    continue
            except OSError:
                continue
            seen.add(f)
            pid = f.name.split('.')[1]
            if cur:
                say('newer game process %s: stopping the recording of %s' % (pid, cur[0]))
                stop(cur[0], cur[1])
            cur = (pid, start(pid), 0)
            last_game = time.time()
        if cur and kperf_busy(cur[0]):          # kperf was still held by someone else
            stop(cur[0], cur[1])
            if cur[2] < 10 and subprocess.run(
                    ['kill', '-0', cur[0]], capture_output=True).returncode == 0:
                say('  kperf was busy, restarting the recording of %s (%d)' % (cur[0], cur[2] + 1))
                time.sleep(1)
                cur = (cur[0], start(cur[0]), cur[2] + 1)
            else:
                say('  recording of %s failed: kperf busy' % cur[0])
                cur = None
        if cur and cur[1].poll() is not None:
            stop(cur[0], cur[1])
            cur = None
        if last_game and not cur and time.time() - last_game > 60:
            say('all recordings ended')
            return 0
        time.sleep(0.5)


# ---------------------------------------------------------------- analyze

def freezes(perf):
    rows = []
    for line in perf.open(errors='replace'):
        m, g = CALLS.match(line), GAME.search(line)
        if m and g:
            rows.append((float(m.group(1)), int(m.group(2)), float(g.group(1)) + float(g.group(2))))
    out, run = [], []
    for t, c, gt in rows + [(0, 1 << 30, 0)]:
        if gt >= 90 and c < 6000:
            run.append(t)
        else:
            if len(run) >= 3:
                out.append((run[0] - 0.1, run[-1]))     # a sample line covers the 0.1 s before it
            run = []
    return out


def modules(log, exe_name='Discovery.exe'):
    """Module bases of the last process that loaded the game exe."""
    per_pid, game_pid = collections.defaultdict(list), None
    for line in log.open(errors='replace'):
        m = LOADDLL.match(line)
        if m:
            path = m.group(2).replace('\\\\', '\\')
            per_pid[m.group(1)].append((int(m.group(3), 16), path.rsplit('\\', 1)[-1]))
            if path.endswith(exe_name):
                game_pid = m.group(1)
    mods = sorted(set(per_pid.get(game_pid, [])))
    return [b for b, _ in mods], [n for _, n in mods]


def classify(addr, binary, bases, names):
    if binary:
        return binary
    if AC_LO <= addr < AC_HI:
        return '[anti-cheat region]'
    i = bisect.bisect_right(bases, addr) - 1
    if i >= 0 and addr - bases[i] < 0x10000000:
        return names[i]
    return '[no module]'


def label(addr, name, binary, bases, names):
    mod = classify(addr, binary, bases, names)
    if mod == '[anti-cheat region]':
        return mod
    if name and not name.startswith('0x'):
        return '%s`%s' % (mod, name)
    if binary:
        return '%s+%#x' % (mod, addr)
    i = bisect.bisect_right(bases, addr) - 1
    return '%s+%#x' % (mod, addr - bases[i]) if mod not in ('[no module]',) and i >= 0 else '%#x' % addr


def trace_start(trace):
    toc = subprocess.run(['xcrun', 'xctrace', 'export', '--input', str(trace), '--toc'],
                         capture_output=True, text=True, env=ENV, check=True).stdout
    d = re.search(r'<start-date>([^<]+)</start-date>', toc).group(1)
    return datetime.datetime.fromisoformat(d).timestamp()


class _XmlCleanReader:
    """xctrace export writes symbol names verbatim, control bytes included (seen 2026-11-25: a
    '__block_descriptor_…' name with a raw \\x01 and an invalid UTF-8 byte), which makes the XML
    unparseable. Replaces invalid UTF-8 and drops the characters XML 1.0 forbids (C0 controls
    except tab, newline, carriage return)."""
    _DROP = {c: None for c in range(32) if c not in (9, 10, 13)}

    def __init__(self, path):
        import codecs
        self._f = open(path, 'rb')
        self._dec = codecs.getincrementaldecoder('utf-8')('replace')

    def read(self, n=-1):
        data = self._f.read(n)
        text = self._dec.decode(data, final=not data)
        if not data:
            self._f.close()
        return text.translate(self._DROP).encode('utf-8')


def samples(trace, xml_path):
    """Yield (seconds since trace start, thread label, [(addr, name, binary), ...] leaf first)."""
    if not xml_path.exists():
        with xml_path.open('w') as f:
            subprocess.run(['xcrun', 'xctrace', 'export', '--input', str(trace), '--xpath',
                            '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'],
                           stdout=f, env=ENV, check=True)
    ids = {}

    def resolve(el):
        r = el.get('ref')
        return ids[r] if r is not None else el

    for _, el in ET.iterparse(_XmlCleanReader(xml_path), events=('end',)):
        i = el.get('id')
        if i is not None:
            ids[i] = el
        if el.tag != 'row':
            continue
        t = th = bt = None
        for c in el:
            c = resolve(c)
            if c.tag == 'sample-time':
                t = int(c.text) / 1e9
            elif c.tag == 'thread':
                th = c.get('fmt', '')
            elif c.tag in ('tagged-backtrace', 'backtrace'):
                bt = c
        frames = []
        if bt is not None:
            for fr in bt.iter('frame'):
                fr = resolve(fr)
                b = fr.find('binary')
                b = resolve(b) if b is not None else None
                frames.append((int(fr.get('addr', '0'), 16), fr.get('name', ''),
                               b.get('name') if b is not None else None))
        yield t, th, frames
        el.clear()


def report(title, rows, bases, names, top):
    n = len(rows)
    print('\n== %s: %d GameThread samples (%.2f s on CPU)' % (title, n, n / 1000.0))
    if not n:
        return
    leafmod, leaf, caller = collections.Counter(), collections.Counter(), collections.Counter()
    for frames in rows:
        if not frames:
            leafmod['[no stack]'] += 1
            continue
        a, nm, b = frames[0]
        leafmod[classify(a, b, bases, names)] += 1
        leaf[label(a, nm, b, bases, names)] += 1
        for a2, nm2, b2 in frames[1:]:
            if nm2 and not nm2.startswith('0x'):
                caller[label(a2, nm2, b2, bases, names)] += 1
                break
    for head, cnt in (('leaf module', leafmod), ('leaf', leaf), ('first named caller', caller)):
        print('  -- %s' % head)
        for k, v in cnt.most_common(top):
            print('    %5.1f %%  %6d  %s' % (100.0 * v / n, v, k))


def analyze(a):
    session = find_session(a.logs, a.session, wait=False)
    log = session.with_suffix('.log')
    bases, names = modules(log) if log.exists() else ([], [])
    print('session %s, %d modules of the game process from +loaddll' % (session.name, len(bases)))
    traces = sorted((session / 'freeze-profile').glob('*.trace'))
    if not traces:
        print('no traces in %s/freeze-profile (run record first)' % session)
        return 1
    for trace in traces:
        pid = trace.stem
        perf = session / 'perf' / ('wine.%s' % pid)
        fz = freezes(perf)
        t0 = trace_start(trace)
        print('\n#### process %s: %d freezes in perf: %s' % (
            pid, len(fz), ' '.join('%.1fs' % (e - s) for s, e in fz)))
        if not fz:
            continue
        starts = [s for s, _ in fz]
        frozen, normal, per = [], [], collections.defaultdict(list)
        threads = collections.Counter()
        for t, th, frames in samples(trace, trace.with_suffix('.time-profile.xml')):
            threads[th] += 1
            if a.thread not in th:
                continue
            ep = t0 + t
            i = bisect.bisect_right(starts, ep) - 1
            if i >= 0 and ep <= fz[i][1]:
                frozen.append(frames)
                per[i].append(frames)
            else:
                normal.append(frames)
        if not frozen and not normal:
            print('no GameThread samples; busiest threads: %s' % threads.most_common(5))
            continue
        report('all freezes', frozen, bases, names, a.top)
        report('outside freezes', normal, bases, names, a.top)
        for i in sorted(per, key=lambda k: -len(per[k]))[:a.each]:
            s, e = fz[i]
            report('freeze at %s, %.1f s' % (time.strftime('%H:%M:%S', time.localtime(s)), e - s),
                   per[i], bases, names, 8)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('mode', choices=('record', 'analyze'))
    p.add_argument('--logs', type=Path, default=HERE / 'logs')
    p.add_argument('--session', type=Path, help='session folder (record: attach to a running one)')
    p.add_argument('--time-limit', type=int, default=900, help='record: seconds per process (default 900)')
    p.add_argument('--thread', default='GameThread', help='analyze: thread whose samples are broken down')
    p.add_argument('--top', type=int, default=15, help='analyze: rows per table')
    p.add_argument('--each', type=int, default=4, help='analyze: longest freezes shown one by one')
    a = p.parse_args()
    return record(a) if a.mode == 'record' else analyze(a)


if __name__ == '__main__':
    sys.exit(main())
