#!/usr/bin/env python3
"""One line per THE FINALS launch from Steam: variant, how long it ran, how it ended.

    python3 tests/finals_bisect/ab_launches.py                    # launches today
    python3 tests/finals_bisect/ab_launches.py --since "2026-11-14 17:00"

For A/B runs with launch options such as `MACFINALS_NT_OFF=syscall STEAMDRIVER_PERF=1 %command%`.
With `STEAMDRIVER_PERF=1`, a thread whose current NT call has been running for over 60 s makes
the counters dump every thread's current NT call (`stuck-dump`); those dumps are printed under
the launch's line. Reads the Steam tool's run log (`steamdriver-run.log`: runtime, `nt_off=`
since 2026-11-14), the per-launch counter files it names (end time when the run log has no
`game exited` line; the game often never writes one), the Unreal crash reports the game writes
(`Saved/Crashes/*/CrashContext.runtime-xml`: crash type, e.g. Hang) and Rosetta aborts in the
Wine log.

Steam runs an iscriptevaluator helper (`=== run` block) with its own `hook=` line before each
launch and again after a launch ends; a launch ends at the next of: its `game exited` line,
the next `=== run` (the helper never runs while the game runs), or the last counter line of its
own perf folder.
"""
import argparse
from datetime import datetime
from pathlib import Path
import re

COMPAT = Path.home() / 'Library/Application Support/Steam/steamapps/compatdata/2073850'
RUN_LOG = COMPAT / 'steamdriver-run.log'
WINE_LOG = COMPAT / 'steamdriver-wine.log'
CRASHES = COMPAT / 'pfx/drive_c/users/crossover/AppData/Local/Discovery/Saved/Crashes'
STAMP = re.compile(r'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) (.*)')
HEAD = re.compile(r'^(\d+\.\d+) calls=')


def last_counter_stamp(folder):
    """Newest epoch stamp on a counter line in the session's wine.* files, or None."""
    newest = None
    for f in folder.glob('wine.*'):
        last = None
        try:
            with f.open(errors='replace') as fh:
                for line in fh:
                    m = HEAD.match(line)
                    if m:
                        last = float(m.group(1))
        except OSError:
            continue
        if last is not None and (newest is None or last > newest):
            newest = last
    return newest


def wine_log_killed():
    """The Wine log ends with wineserver's death line. The log is appended across sessions,
    so this can only describe the newest session."""
    try:
        with WINE_LOG.open('rb') as fh:
            fh.seek(0, 2)
            fh.seek(max(0, fh.tell() - 4096))
            tail = fh.read().decode(errors='replace').splitlines()
    except OSError:
        return False
    return bool(tail) and 'wineserver crashed' in tail[-1]


def launches(since):
    out, cur = [], None
    for line in RUN_LOG.read_text(errors='replace').splitlines():
        m = STAMP.match(line)
        if not m:
            continue
        when, text = datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S'), m.group(2)
        if text.startswith('=== waitforexitandrun'):
            rt = re.search(r'runtime (\S+?)\)', text)
            cur = {'start': when, 'runtime': rt.group(1) if rt else '?', 'nt_off': '?', 'end': None,
                   'how': 'still running?', 'perf': None}
            if when >= since:
                out.append(cur)
        elif text.startswith('=== run'):
            # the iscriptevaluator helper: its hook= line (nt_off=-) is not the game's, and the
            # previous launch is over by the time Steam runs it again
            if cur is not None and cur['end'] is None:
                cur['end'], cur['how'] = when, 'ended (no exit line)'
            cur = None
        elif cur is not None and text.startswith('hook='):
            m2 = re.search(r'nt_off=(\S+)', text)
            cur['nt_off'] = m2.group(1) if m2 else '(not logged)'
        elif cur is not None and text.startswith('performance counters: '):
            cur['perf'] = Path(text.split(': ', 1)[1])
        elif cur is not None and text.startswith('game exited'):
            cur['end'], cur['how'] = when, text.split(';')[0]
    # the game rarely writes a `game exited` line; its own counters' last line is the real end
    for r in out:
        if not r['perf'] or r['how'].startswith('game exited'):
            continue
        stamp = last_counter_stamp(r['perf'])
        if stamp:
            r['end'] = datetime.fromtimestamp(stamp)
            r['how'] = 'ended (counters stop)'
    return out


def crash_reports():
    reports = []
    if not CRASHES.exists():
        return reports
    for d in CRASHES.iterdir():
        ctx = d / 'CrashContext.runtime-xml'
        if not ctx.exists():
            continue
        s = ctx.read_text(errors='replace')
        kind = re.search(r'<CrashType>([^<]*)', s)
        msg = re.search(r'<ErrorMessage>([^<\n]*)', s)
        reports.append((datetime.fromtimestamp(ctx.stat().st_mtime), kind.group(1) if kind else '?',
                        msg.group(1).strip() if msg else '', d.name))
    return sorted(reports)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--since', default=datetime.now().strftime('%Y-%m-%d 00:00'))
    args = parser.parse_args()
    since = datetime.strptime(args.since, '%Y-%m-%d %H:%M')
    runs = launches(since)
    reports = crash_reports()
    killed = wine_log_killed()
    for i, r in enumerate(runs):
        stop = runs[i + 1]['start'] if i + 1 < len(runs) else datetime.max
        mine = [c for c in reports if r['start'] <= c[0] < stop]
        secs = int((r['end'] - r['start']).total_seconds()) if r['end'] else None
        how = r['how']
        if killed and i == len(runs) - 1 and not how.startswith('game exited'):
            how += '; wineserver crashed?'
        crash = '; '.join(f'UE {k}: {m[:60]}' for _, k, m, _ in mine) or 'no UE crash report'
        print(f"{r['start']:%H:%M:%S}  {r['runtime']:<14} nt_off={r['nt_off']:<10} "
              f"{(str(secs) + ' s') if secs is not None else '-':>7}  {how:<32} {crash}")
        if r['perf'] and r['perf'].exists():
            for f in sorted(r['perf'].glob('wine.*')):
                lines = f.read_text(errors='replace').splitlines()
                for j, line in enumerate(lines):
                    if 'stuck-dump' in line:
                        print(f"          {f.name}: {line.split(' ', 1)[1]}")
                        for more in lines[j + 1:]:
                            if not more.startswith('  '):
                                break
                            print(f"          {more}")
    if not runs:
        print('no launches since', since)


if __name__ == '__main__':
    main()
