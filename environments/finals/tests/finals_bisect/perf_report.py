#!/usr/bin/env python3
"""Summarise the per-process performance counters of a THE FINALS session (WINE_SYSCALL_LOG).

    python3 tests/finals_bisect/perf_report.py              # newest session
    python3 tests/finals_bisect/perf_report.py <perf dir>   # a folder of wine.<pid> files
    python3 tests/finals_bisect/perf_report.py --bucket 5   # timeline resolution in seconds

Sessions come from launch_main.sh PERF=1 (tests/finals_bisect/logs/*/perf) or from Steam with
`STEAMDRIVER_PERF=1 %command%` (<compatdata>/perf/<date>).

Reads counters only. The per-interval lists are top-6 (NT calls) and top-4 (threads), so an
item's total is a lower bound: what never reached the list in an interval counts as 0 there.
"Freeze" is the definition freeze_profile.py uses: GameThread >= 90 % of an interval while the
process makes < 6000 NT calls in it. The counters themselves add cost to every NT call.
"""
import argparse
import collections
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
STEAM_COMPAT = Path.home() / 'Library/Application Support/Steam/steamapps/compatdata/2073850/perf'
PAIR = re.compile(r'([A-Za-z_][\w:.-]*)=(-?[\d.]+)')
THREAD = re.compile(r'(\S+)#([0-9a-f]{4})=([\d.]+)\+([\d.]+)')
HEAD = re.compile(r'^(\d+\.\d+) calls=(\d+) time_ms=([\d.]+) bsd=(\d+) mach=(\d+)')
TCALL = re.compile(r'(\S+)#[0-9a-f]{4}>(\S+):(\d+):([\d.]+)')
LONG = re.compile(r'^(\d+\.\d+) long (\S+) ms=([\d.]+) tid=(\w+)')
VM = re.compile(r'n=(\d+) hold=([\d.]+) max=([\d.]+)@(\S+) wait=([\d.]+) max=([\d.]+)@(\S+)')
FIXED = {'faults', 'pageins', 'csw'}
FREEZE_CPU_MS, FREEZE_CALLS = 90.0, 6000


def parse(path):
    samples, longs = [], []
    for raw in path.open(errors='replace'):
        hit = LONG.match(raw)
        if hit:
            longs.append((float(hit[1]), hit[2], float(hit[3]), hit[4]))
            continue
        hit = HEAD.match(raw)
        if not hit:
            continue
        parts = [p.strip() for p in raw.split(' | ')]
        s = dict(t=float(hit[1]), calls=int(hit[2]), time_ms=float(hit[3]), bsd=int(hit[4]),
                 mach=int(hit[5]), counts={}, ms={}, events={}, diag={}, vm=None, threads={}, run=0, tcalls=[])
        for part in parts[1:]:
            if part.startswith('ms:'):
                s['ms'] = {k: float(v) for k, v in PAIR.findall(part)}
            elif part.startswith('faults='):
                for k, v in PAIR.findall(part):
                    (s['events'] if k in FIXED else s['diag'])[k] = float(v)
            elif part.startswith('vm:'):
                s['vm'] = VM.search(part)
            elif part.startswith('thr:'):
                run = re.search(r'run=(\d+)', part)
                s['run'] = int(run[1]) if run else 0
                for name, _tid, user, system in THREAD.findall(part):
                    u, k = s['threads'].get(name, (0.0, 0.0))
                    s['threads'][name] = (u + float(user), k + float(system))
            elif part.startswith('rare:'):
                pass
            elif part.startswith('tcalls:'):
                # macfinals-108 or later: the busiest threads' own top calls
                s['tcalls'] = [(t, c, int(n), float(ms)) for t, c, n, ms in TCALL.findall(part)]
            elif not s['counts']:
                s['counts'] = {k: int(float(v)) for k, v in PAIR.findall(part)}
        samples.append(s)
    return samples, longs


def game_cpu(s):
    return sum(s['threads'].get('GameThread', (0.0, 0.0)))


def freezes(samples):
    """Runs of adjacent frozen intervals, as (start, seconds, peak GameThread ms)."""
    out, run = [], []
    for s in samples:
        if game_cpu(s) >= FREEZE_CPU_MS and s['calls'] < FREEZE_CALLS:
            if run and s['t'] - run[-1]['t'] > 0.25:
                out.append(run)
                run = []
            run.append(s)
        elif run:
            out.append(run)
            run = []
    if run:
        out.append(run)
    return [(r[0]['t'], r[-1]['t'] - r[0]['t'] + 0.1, max(game_cpu(s) for s in r)) for r in out]


def rows(title, header, data):
    print(f'\n{title}')
    print('  ' + header)
    for line in data:
        print('  ' + line)


def report(path, bucket):
    samples, longs = parse(path)
    if not samples:
        print(f'{path}: no counter lines')
        return
    span = max(samples[-1]['t'] - samples[0]['t'], 0.1)
    n = len(samples)
    t0 = samples[0]['t']
    print(f'\n=== {path.name}: {span:.0f} s, {n} intervals ({n / 10 / span * 100:.0f} % of the span active)')

    calls, secs = collections.Counter(), collections.Counter()
    pair_n, pair_ms = collections.Counter(), collections.Counter()
    for s in samples:
        calls.update(s['counts'])
        secs.update(s['ms'])
        for k in s['counts'].keys() & s['ms'].keys():
            pair_n[k] += s['counts'][k]
            pair_ms[k] += s['ms'][k]
    total_calls = sum(s['calls'] for s in samples)
    print(f"NT calls {total_calls / span:,.0f}/s   Mach traps {sum(s['mach'] for s in samples) / span:,.0f}/s"
          f"   BSD syscalls {sum(s['bsd'] for s in samples) / span:,.0f}/s"
          f"   context switches {sum(s['events'].get('csw', 0) for s in samples) / span:,.0f}/s"
          f"   page faults {sum(s['events'].get('faults', 0) for s in samples) / span:,.0f}/s")

    def per_call(name):
        return f'{pair_ms[name] * 1e6 / pair_n[name]:9.0f}' if pair_n[name] else '        -'

    rows('NT calls by count (lower bound)', f"{'calls/s':>12} {'share':>6} {'ns/call':>9}  name",
         [f'{c / span:12,.0f} {c / max(total_calls, 1) * 100:5.1f}% {per_call(k)}  {k}'
          for k, c in calls.most_common(12)])
    rows('NT calls by time in the call, all threads (waits included)', f"{'ms/s':>10} {'ns/call':>9}  name",
         [f'{v / span:10.1f} {per_call(k)}  {k}' for k, v in secs.most_common(12)])

    threads = collections.defaultdict(lambda: [0.0, 0.0, 0])
    for s in samples:
        for name, (u, k) in s['threads'].items():
            t = threads[name]
            t[0] += u
            t[1] += k
            t[2] += 1
    ordered = sorted(threads.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:14]
    rows('Busiest threads (% of one core over the span; lower bound)',
         f"{'user':>7} {'system':>7} {'sys share':>9}  name",
         [f'{u / span / 10:6.1f}% {k / span / 10:6.1f}% {k / max(u + k, 0.1) * 100:8.1f}%  {name}'
          for name, (u, k, _) in ordered])

    # Which NT calls each busy thread makes itself (only intervals where it was among the 4
    # busiest; workers with the same name are added together).
    per = collections.defaultdict(lambda: [collections.Counter(), collections.Counter()])
    for s in samples:
        for thread, call, n, ms in s['tcalls']:
            per[thread][0][call] += n
            per[thread][1][call] += ms
    if per:
        print('\nNT calls each busy thread makes itself (per second of the span; lower bound)')
        for name, _ in ordered[:6]:
            if name not in per:
                continue
            n, ms = per[name]
            print(f'  {name}:')
            for call, c in n.most_common(6):
                print(f'    {c / span:10,.0f}/s  {ms[call] / span:7.1f} ms/s  {ms[call] * 1e6 / c:8.0f} ns/call  {call}')

    if any(s['threads'] for s in samples):
        with_game = [s for s in samples if 'GameThread' in s['threads']]
        if with_game:
            sat = sum(game_cpu(s) >= FREEZE_CPU_MS for s in with_game)
            print(f"\nGameThread on the CPU >= 90 % in {sat} of {len(with_game)} intervals "
                  f'({sat / len(with_game) * 100:.0f} %): that share is limited by the CPU, not by waiting')
    diag = collections.Counter()
    for s in samples:
        diag.update(s['diag'])
    if diag:
        print('Fault and signal counters per second: ' +
              ', '.join(f'{k} {v / span:,.1f}' for k, v in diag.most_common()))
    vms = [s['vm'] for s in samples if s['vm']]
    if vms:
        worst_hold = max(vms, key=lambda m: float(m[3]))
        worst_wait = max(vms, key=lambda m: float(m[6]))
        print(f'VM lock: held {sum(float(m[2]) for m in vms) / span:.1f} ms/s, '
              f'waited {sum(float(m[5]) for m in vms) / span:.1f} ms/s, '
              f'longest hold {float(worst_hold[3]):.1f} ms in {worst_hold[4]}, '
              f'longest wait {float(worst_wait[6]):.1f} ms in {worst_wait[7]}')

    if longs:
        by = collections.defaultdict(list)
        for _t, name, ms, _tid in longs:
            by[name].append(ms)
        data = sorted(by.items(), key=lambda kv: -sum(kv[1]))[:12]
        rows('NT calls longer than 30 ms that are not waits', f"{'count':>6} {'total ms':>10} {'max ms':>9}  name",
             [f'{len(v):6} {sum(v):10.1f} {max(v):9.1f}  {k}' for k, v in data])

    frozen = freezes(samples)
    if frozen:
        rows(f'Freezes ({len(frozen)}; GameThread >= 90 %, < {FREEZE_CALLS} NT calls per interval)',
             f"{'at s':>8} {'seconds':>8}", [f'{t - t0:8.1f} {d:8.1f}' for t, d, _ in frozen[:40]])

    buckets = collections.defaultdict(list)
    for s in samples:
        buckets[int((s['t'] - t0) // bucket)].append(s)
    head = f"{'at s':>6} {'NT/s':>10} {'QPC/s':>9} {'yield/s':>9} {'mach/s':>9} {'csw/s':>8} {'faults/s':>9} {'Game%':>6} {'Rndr%':>6} {'run':>4}"
    data = []
    for b in sorted(buckets):
        chunk = buckets[b]
        scale = len(chunk) * 0.1
        total = lambda f: sum(f(s) for s in chunk) / scale
        render = lambda s: sum(sum(v) for k, v in s['threads'].items() if k.startswith('RenderThread'))
        data.append(f"{b * bucket:6} {total(lambda s: s['calls']):10,.0f} "
                    f"{total(lambda s: s['counts'].get('NtQueryPerformanceCounter', 0)):9,.0f} "
                    f"{total(lambda s: s['counts'].get('NtYieldExecution', 0)):9,.0f} "
                    f"{total(lambda s: s['mach']):9,.0f} {total(lambda s: s['events'].get('csw', 0)):8,.0f} "
                    f"{total(lambda s: s['events'].get('faults', 0)):9,.0f} "
                    f'{sum(game_cpu(s) for s in chunk) / len(chunk):6.0f} {sum(render(s) for s in chunk) / len(chunk):6.0f} '
                    f"{sum(s['run'] for s in chunk) / len(chunk):4.0f}")
    rows(f'Timeline ({bucket} s buckets; per-second rates over active intervals; Game%/Rndr% of one core)', head, data)


def newest():
    found = [p for p in (HERE / 'logs').glob('*/perf') if p.is_dir()]
    if STEAM_COMPAT.is_dir():
        found += [p for p in STEAM_COMPAT.iterdir() if p.is_dir()]
    return max(found, key=lambda p: p.stat().st_mtime, default=None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('perf', nargs='?', type=Path, help='folder of wine.<pid> files (default: newest session)')
    ap.add_argument('--bucket', type=int, default=10, help='timeline bucket in seconds (default 10)')
    ap.add_argument('--all', action='store_true', help='report every process with a GameThread, not only the longest')
    args = ap.parse_args()
    folder = args.perf or newest()
    if not folder or not folder.is_dir():
        sys.exit('no session found; pass a folder of wine.<pid> files')
    files = [p for p in folder.glob('wine.*') if p.stat().st_size]
    games = [p for p in files if 'GameThread#' in p.read_text(errors='replace')]
    print(f'session: {folder}\n{len(files)} processes logged, {len(games)} with a GameThread')
    if not games:
        sys.exit('no process named a GameThread: the game did not run in this session')
    # THE FINALS starts a short-lived Discovery.exe first; the real game is the longest log.
    games.sort(key=lambda p: -p.stat().st_size)
    for path in games if args.all else games[:1]:
        report(path, args.bucket)


if __name__ == '__main__':
    main()
