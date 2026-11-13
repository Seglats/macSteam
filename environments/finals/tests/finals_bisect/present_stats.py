#!/usr/bin/env python3
"""When the game's frames reached the display, from WINEMAC_INPUT_STATS files (present_* fields).

    python3 tests/finals_bisect/present_stats.py /tmp/kovaaks-input-12345.jsonl [more files]
    python3 tests/finals_bisect/present_stats.py --newest /tmp/kovaaks-input     # newest file with that prefix

Launch with e.g. `WINEMAC_INPUT_STATS=/tmp/kovaaks-input %command%`. The Mac driver records, for
every D3DMetal frame, when it reached the display or that it never did, and writes a summary each
second (wine-winemac-present-stats.patch). Nothing else runs, unlike an Instruments recording.

An interval is how long a frame stayed on screen. On a fixed-refresh display it should be a
multiple of the refresh (6.25 ms at 160 Hz); "held" = 9 ms or more (about 1.5 refreshes), "short" = under
5.5 ms (the display path did not wait for a refresh). The thresholds assume a 160 Hz display.
"""
import glob
import json
import os
import sys

EDGES = [4, 5.5, 7, 9, 11, 13.5, 17, 25]  # bucket upper edges, as in the Mac driver
LABELS = [f'<{e}' for e in EDGES] + [f'>={EDGES[-1]}']


def load(paths):
    rows = []
    for path in paths:
        for line in open(path, errors='replace'):
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if 'present_hist' in row:
                rows.append(row)
    return rows


def main(args):
    if args[:1] == ['--newest']:
        files = sorted(glob.glob(args[1] + '-*.jsonl'), key=os.path.getmtime)
        files = [f for f in files if load([f])][-1:]
    else:
        files = args
    rows = [r for r in load(files) if r['present_shown'] or r['present_dropped']]
    if not rows:
        sys.exit('no present_* data (WINEMAC_INPUT_STATS set? macfinals-98 or later?)')
    print('files:', ', '.join(files))
    # Loading, quitting and menus (KovaaK's caps its menu, e.g. at 60) are left out: only seconds
    # at 85 % or more of the run's highest frame rate count as play.
    top = max(r['present_shown'] for r in rows)
    play = [r for r in rows if r['present_shown'] >= 0.85 * top]
    hist = [sum(r['present_hist'][i] for r in play) for i in range(len(LABELS))]
    shown = sum(r['present_shown'] for r in play)
    dropped = sum(r['present_dropped'] for r in play)
    intervals = sum(hist)
    print(f'{len(play)} s of play: {shown} frames shown ({shown / len(play):.0f}/s), {dropped} never shown'
          f' ({100 * dropped / max(1, shown + dropped):.1f} %)')
    print('time on screen:', '  '.join(f'{l}:{n}' for l, n in zip(LABELS, hist)))
    # For a 160 Hz display (6.25 ms): short = under 5.5 ms, held = 9 ms or more (about 1.5 refreshes).
    short, held = hist[0] + hist[1], sum(hist[4:])
    print(f'held (>= 9 ms): {held} ({100 * held / max(1, intervals):.1f} %)   '
          f'short (< 5.5 ms): {short} ({100 * short / max(1, intervals):.1f} %)   '
          f'worst {max(r["present_ms_max"] for r in play):.1f} ms')
    timed = [r for r in play if 'present_latency_jump_hist' in r]
    if timed:
        # Delay from the drawable to the screen; its change between frames moves a steadily moving
        # object off its path by speed x change (wine-winemac-present-latency.patch).
        jumps = [sum(r['present_latency_jump_hist'][i] for r in timed) for i in range(5)]
        n = max(1, sum(jumps))
        avg = sum(r['present_latency_ms_avg'] for r in timed) / len(timed)
        sd = sum(r['present_latency_ms_sd'] for r in timed) / len(timed)
        print(f'drawable -> screen: avg {avg:.2f} ms, spread (sd) {sd:.2f} ms, max {max(r["present_latency_ms_max"] for r in timed):.1f} ms')
        print('  change from frame to frame:', '  '.join(f'{l}:{v} ({100 * v / n:.1f} %)' for l, v in
                                                     zip(['<0.5', '<1', '<2', '<4', '>=4 ms'], jumps)))
        print(f'  wait for a drawable: avg {sum(r["present_wait_ms_avg"] for r in timed) / len(timed):.2f} ms,'
              f' max {max(r["present_wait_ms_max"] for r in timed):.2f} ms')
        print(f'  game frame interval (CPU side): avg {sum(r["present_request_ms_avg"] for r in timed) / len(timed):.2f} ms,'
              f' spread (sd) {sum(r["present_request_ms_sd"] for r in timed) / len(timed):.2f} ms,'
              f' max {max(r["present_request_ms_max"] for r in timed):.1f} ms')
    print('per second (shown/never shown):', ' '.join(f'{r["present_shown"]}/{r["present_dropped"]}' for r in play))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
