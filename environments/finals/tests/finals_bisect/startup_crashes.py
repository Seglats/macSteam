#!/usr/bin/env python3
"""Count and place THE FINALS' Rosetta start-up crashes, launch by launch, from the Wine log.

    python3 tests/finals_bisect/startup_crashes.py                 # every launch in the log
    python3 tests/finals_bisect/startup_crashes.py --from 48232    # launches after that line

The crash is Rosetta's `rosetta error: no code fragment associated with the given arm pc`
(tests/rosetta_nt/R23_STACKS_POPUP_ROSETTA_CRASH.md). A launch starts where steam.exe is loaded
and counts when it loads Discovery.exe. For each crash it prints how far start-up had come: which
of the game's processes had started (Discovery.exe loads; Discovery-d.exe), whether the
minimum-spec popup had been clicked, and the last modules loaded before it. The log is
`<compatdata>/steamdriver-wine.log`, which the Steam tool appends to (rotated past 200 MB).
"""
import argparse
from pathlib import Path
import re

LOG = Path.home() / 'Library/Application Support/Steam/steamapps/compatdata/2073850/steamdriver-wine.log'
STEAM = re.compile(r'Loaded L"C:\\\\Program Files \(x86\)\\\\Steam\\\\steam\.exe"')
LOADED = re.compile(r'Loaded L"([^"]+)"')
CRASH = 'rosetta error: no code fragment'


def launches(lines, start):
    """(first line number, lines) per segment that starts at a steam.exe load."""
    segment, first = [], start
    for n, line in enumerate(lines[start:], start + 1):
        if STEAM.search(line) and any('The Finals\\\\Discovery.exe' in l for l in segment):
            yield first, segment
            segment, first = [], n
        elif STEAM.search(line) and not segment:
            first = n
        segment.append(line)
    if any('The Finals\\\\Discovery.exe' in l for l in segment):
        yield first, segment


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--log', type=Path, default=LOG)
    parser.add_argument('--from', dest='start', type=int, default=0, help='start after this line (1-based)')
    args = parser.parse_args()
    lines = args.log.read_text(errors='replace').splitlines()
    total = crashed = 0
    for first, seg in launches(lines, args.start):
        total += 1
        at = next((i for i, l in enumerate(seg) if CRASH in l), None)
        if at is None:
            game = any('Discovery-d.exe' in l for l in seg)
            print(f'#{total:<3} line {first:>7}: no crash' + (' (game process started)' if game else ''))
            continue
        crashed += 1
        before = seg[:at]
        stage = sum(1 for l in before if re.search(r'The Finals\\\\Discovery\.exe" at', l))
        game = any('Discovery-d.exe' in l for l in before)
        popup = any('dialog_accept_click' in l for l in before)
        mods = [LOADED.search(l).group(1).split('\\')[-1] for l in before if LOADED.search(l)][-4:]
        print(f'#{total:<3} line {first:>7}: CRASH at line {first + at}: Discovery.exe started {stage}x,'
              f' game process {"yes" if game else "no"}, popup {"clicked" if popup else "not yet"};'
              f' last loaded: {", ".join(mods)}')
    print(f'\n{crashed} crashes in {total} launches')


if __name__ == '__main__':
    main()
