#!/usr/bin/env python3
"""Controller latency inside Wine, from the macfinals_pad trace (wine-pad-latency-trace.patch).

Reads a Wine log (default: THE FINALS' steamdriver-wine.log) and times each changed controller
report through Wine's stages:

  usb     winebus.so: the pad's USB report completed           (winedevice.exe)
  hid     winebus.sys: handed to hidclass                       (winedevice.exe)
  xinput  xinput's update thread: new state ready               (game process)
  dinput  dinput: new device state ready                        (game process)
  game    the game's first XInputGetState / GetDeviceState that returns it

All times are QueryPerformanceCounter microseconds, one clock for every process.

  python3 pad_trace.py [log] [--from-end MB]
"""
import os
import re
import statistics
import sys

DEFAULT_LOG = os.path.expanduser(
    '~/Library/Application Support/Steam/steamapps/compatdata/2073850/steamdriver-wine.log')
LINE = re.compile(r'macfinals_pad:\w+ (usb|hid|xinput|dinput|game) (.*)')
VALUES = re.compile(r'b ([0-9a-f]{4}) lt (\d+) rt (\d+) l (-?\d+) (-?\d+) r (-?\d+) (-?\d+)')


def parse(path, from_end_mb):
    events = []
    with open(path, 'rb') as f:
        if from_end_mb:
            size = os.path.getsize(path)
            f.seek(max(0, size - int(from_end_mb * 1e6)))
        for raw in f:
            if b'macfinals_pad' not in raw:
                continue
            m = LINE.search(raw.decode('utf-8', 'replace'))
            if not m:
                continue
            kind, rest = m.groups()
            t = int(re.search(r'\bt (\d+)', rest).group(1))
            v = VALUES.search(rest)
            values = tuple(int(x, 16) if i == 0 else int(x) for i, x in enumerate(v.groups())) if v else None
            api = 'xinput' if values is not None else 'dinput'
            events.append({'kind': kind, 't': t, 'values': values, 'api': api})
    events.sort(key=lambda e: e['t'])
    return events


def stats(name, deltas_us):
    if not deltas_us:
        print(f'  {name:<28} no samples')
        return
    d = sorted(deltas_us)
    p = lambda q: d[min(len(d) - 1, int(q * len(d)))]
    print(f'  {name:<28} n {len(d):>6}   median {statistics.median(d)/1000:7.3f} ms   '
          f'p95 {p(0.95)/1000:7.3f}   p99 {p(0.99)/1000:7.3f}   max {d[-1]/1000:7.3f}')


def next_after(items, start, t, limit_us=100_000):
    """Index of the first item at or after time t, from start; None if beyond the limit."""
    i = start
    while i < len(items) and items[i]['t'] < t:
        i += 1
    if i < len(items) and items[i]['t'] - t <= limit_us:
        return i
    return None


def main():
    args = sys.argv[1:]
    from_end = None
    if '--from-end' in args:
        k = args.index('--from-end')
        from_end = float(args[k + 1])
        del args[k:k + 2]
    path = args[0] if args else DEFAULT_LOG
    ev = parse(path, from_end)
    by = {k: [e for e in ev if e['kind'] == k] for k in ('usb', 'hid', 'xinput', 'dinput')}
    game_x = [e for e in ev if e['kind'] == 'game' and e['api'] == 'xinput']
    game_d = [e for e in ev if e['kind'] == 'game' and e['api'] == 'dinput']
    print(f'{path}\n  events: ' + ', '.join(f'{k} {len(v)}' for k, v in by.items())
          + f', game reads xinput {len(game_x)} dinput {len(game_d)}')
    if not ev:
        print('  no macfinals_pad lines: launch with WINEDEBUG=err+all,fixme-all,+loaddll,+macfinals_pad %command%')
        return

    # usb -> hid: in order, the next hid line after each usb line.
    usb_hid, j = [], 0
    for u in by['usb']:
        k = next_after(by['hid'], j, u['t'])
        if k is None:
            continue
        usb_hid.append(by['hid'][k]['t'] - u['t'])
        j = k + 1

    # usb -> xinput state: same values, first at or after the usb line.
    usb_x, xi = [], 0
    xstates = by['xinput']
    for u in by['usb']:
        k = next_after(xstates, xi, u['t'])
        while k is not None and k < len(xstates) and xstates[k]['values'] != u['values'] \
                and xstates[k]['t'] - u['t'] <= 100_000:
            k += 1
        if k is None or k >= len(xstates) or xstates[k]['values'] != u['values']:
            continue
        usb_x.append(xstates[k]['t'] - u['t'])
        xi = k + 1

    # xinput state -> game read: the game sees a snapshot; time from when that exact state became
    # ready (latest xinput line with these values before the read) and from its usb report.
    last_x, last_u = {}, {}
    x_game, u_game, u_game_buttons = [], [], []
    stream = sorted(by['usb'] + xstates + game_x, key=lambda e: e['t'])
    prev_buttons = None
    for e in stream:
        if e['kind'] == 'usb':
            last_u[e['values']] = e['t']
        elif e['kind'] == 'xinput':
            last_x[e['values']] = e['t']
        else:
            v = e['values']
            if v in last_x:
                x_game.append(e['t'] - last_x[v])
            if v in last_u:
                u_game.append(e['t'] - last_u[v])
                if prev_buttons is not None and v[0] != prev_buttons:
                    u_game_buttons.append(e['t'] - last_u[v])
            prev_buttons = v[0]

    # dinput: hid -> dinput state (in order), dinput -> game read (in order).
    hid_d, j = [], 0
    for h in by['hid']:
        k = next_after(by['dinput'], j, h['t'])
        if k is None:
            continue
        hid_d.append(by['dinput'][k]['t'] - h['t'])
        j = k + 1
    d_game, j = [], 0
    for g in game_d:
        while j + 1 < len(by['dinput']) and by['dinput'][j + 1]['t'] <= g['t']:
            j += 1
        if j < len(by['dinput']) and by['dinput'][j]['t'] <= g['t']:
            d_game.append(g['t'] - by['dinput'][j]['t'])

    reads = [e['t'] for e in game_x] or [e['t'] for e in game_d]
    print('\nInside winedevice.exe')
    stats('usb -> hid', usb_hid)
    print('XInput (game process)')
    stats('usb -> xinput state ready', usb_x)
    stats('xinput state -> game read', x_game)
    stats('usb -> game read', u_game)
    stats('usb -> game read, buttons', u_game_buttons)
    print('DirectInput / Windows.Gaming.Input (game process)')
    stats('hid -> dinput state ready', hid_d)
    stats('dinput state -> game read', d_game)
    if len(reads) > 2:
        gaps = [b - a for a, b in zip(reads, reads[1:]) if b - a < 200_000]
        print('Game reads that saw a change')
        stats('interval between them', gaps)
    usb_gaps = [b['t'] - a['t'] for a, b in zip(by['usb'], by['usb'][1:]) if b['t'] - a['t'] < 200_000]
    print('Pad')
    stats('changed-report interval', usb_gaps)


if __name__ == '__main__':
    main()
