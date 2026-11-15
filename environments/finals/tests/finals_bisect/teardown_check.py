#!/usr/bin/env python3
"""Reproduce THE FINALS' start-up Rosetta crash without the game (tests/windows_probe/teardown_probe.c).

    python3 tests/finals_bisect/teardown_check.py                     # installed runtime, hook on
    python3 tests/finals_bisect/teardown_check.py --nt-off syscall    # fast path off (the A/B)
    python3 tests/finals_bisect/teardown_check.py --no-hook           # no Rosetta hook at all
    python3 tests/finals_bisect/teardown_check.py --mode suspendonly  # suspensions only, clean exit
    python3 tests/finals_bisect/teardown_check.py --hook tests/rosetta_nt/hooklib.dylib   # a new build

Runs the probe in a prefix of its own (~/Library/Caches/SteamDriver/teardown-test) with the
environment compat-tool/run sets up, and counts `rosetta error: no code fragment` in Wine's
output. No Steam, no game, no window.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
PROBE = HERE.parent / 'windows_probe/teardown_probe.c'
CACHE = Path.home() / 'Library/Caches/SteamDriver'
PREFIX = CACHE / 'teardown-test'
INSTALLED = Path.home() / 'Library/Application Support/Steam/compatibilitytools.d/notproton-steamdriver-finals/runtime'
CRASH = 'rosetta error: no code fragment'


def build_probe():
    out = CACHE / 'teardown-probe.exe'
    if out.exists() and out.stat().st_mtime >= PROBE.stat().st_mtime:
        return out
    CACHE.mkdir(parents=True, exist_ok=True)
    subprocess.run(['x86_64-w64-mingw32-gcc', '-O1', '-Wall', str(PROBE), '-o', str(out), '-lwinmm'], check=True)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('runtime', nargs='?', type=Path, default=INSTALLED)
    parser.add_argument('--rounds', type=int, default=30)
    parser.add_argument('--threads', type=int, default=16)
    parser.add_argument('--mode', choices=('exit', 'suspend', 'suspendonly', 'mixed', 'waitexit'), default='exit')
    parser.add_argument('--nt-off', help='MACFINALS_NT_OFF value, e.g. syscall')
    parser.add_argument('--no-hook', action='store_true', help='without the Rosetta NT hook')
    parser.add_argument('--winedebug', help='WINEDEBUG value (overrides --debug), e.g. +server')
    parser.add_argument('--debug', action='store_true', help='WINEDEBUG=-all,+macfinals_hook (the hook says where its code runs)')
    parser.add_argument('--hook', type=Path, help="another hook dylib (default: the runtime's)")
    args = parser.parse_args()
    engine = args.runtime.expanduser().resolve()
    if not (engine / 'bin/wine').exists():
        sys.exit(f'{engine}: no bin/wine')
    exe = build_probe()

    env = {k: v for k, v in os.environ.items() if not k.startswith(('WINE', 'DYLD_', 'MACFINALS_'))}
    env.update(WINEPREFIX=str(PREFIX), WINEARCH='win64', WINEDEBUG='-all',
               WINELOADER=str(engine / 'bin/wine'), WINESERVER=str(engine / 'bin/wineserver'),
               CX_APPLEGPTK_LIBD3DSHARED_PATH=str(engine / 'lib/external/libd3dshared.dylib'),
               WINEESYNC='0', WINEMSYNC='1', ROSETTA_ADVERTISE_AVX='1', WINE_GAME_MODE='*')
    hook = args.hook.expanduser().resolve() if args.hook else engine / 'lib/macfinals-nt-hook.dylib'
    if hook.exists() and not args.no_hook:
        env.update(DYLD_INSERT_LIBRARIES=str(hook), DISABLE_AOT='1')
    if args.debug:
        env['WINEDEBUG'] = '-all,+macfinals_hook'
    if args.winedebug:
        env['WINEDEBUG'] = args.winedebug
    if args.nt_off:
        env['MACFINALS_NT_OFF'] = args.nt_off
    if not PREFIX.exists():
        print('making the test prefix (once)...')
        subprocess.run([str(engine / 'bin/wine'), 'wineboot', '-i'], env=dict(env, WINEDLLOVERRIDES='mscoree,mshtml='),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([str(engine / 'bin/wineserver'), '-w'], env=env)

    label = 'no hook' if args.no_hook or not hook.exists() else f'hook, MACFINALS_NT_OFF={args.nt_off or "-"}'
    print(f'runtime {engine.name}, {label}; {args.rounds} rounds x {args.threads} threads, mode {args.mode}')
    log = CACHE / 'teardown-test.log'
    with open(log, 'w') as out:
        subprocess.run([str(engine / 'bin/wine'), str(exe), str(args.rounds), str(args.threads), args.mode],
                       env=env, stdout=out, stderr=subprocess.STDOUT, timeout=args.rounds * 40 + 60)
    subprocess.run([str(engine / 'bin/wineserver'), '-k'], env=env)
    text = log.read_text(errors='replace')
    speeds = [float(m) for m in re.findall(r'child \d+: (\d+) ns per syscall', text)]
    rounds = re.findall(r'round \d+: (\w+), exit code (0x[0-9a-f]+|0)', text)
    crashes = text.count(CRASH)
    nonzero = sum(1 for how, code in rounds if how != 'exited' or int(code, 16) != 0)
    if speeds:
        speeds.sort()
        print(f'syscall cost per child: median {speeds[len(speeds) // 2]:.0f} ns'
              f' ({"fast path engaged" if speeds[len(speeds) // 2] < 5000 else "stock path"})')
    print(f'{crashes} Rosetta crashes, {nonzero} abnormal exits in {len(rounds)} rounds   (log: {log})')


if __name__ == '__main__':
    main()
