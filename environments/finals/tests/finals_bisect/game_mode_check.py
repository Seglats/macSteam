#!/usr/bin/env python3
"""Does macOS treat a program started by a runtime as a game (Game Mode on)?

    python3 tests/finals_bisect/game_mode_check.py                       # the installed runtime
    python3 tests/finals_bisect/game_mode_check.py ~/SteamDriver-build/finals/phase-2/artifacts/macfinals-106
    python3 tests/finals_bisect/game_mode_check.py --off                 # control: no game apps

No Steam and no game: builds Recall's Game Mode stand-in (tests/game_mode/standin.c, a Direct3D 11
window that asks DXGI for fullscreen, as a game does; here it draws through D3DMetal) and starts it
with the environment compat-tool/run sets up, WINE_GAME_MODE=* included, in a prefix of its own
(~/Library/Caches/SteamDriver/game-mode-test, made on the first run). Each second it prints whether
Game Mode is on (gamepolicyctl), which app macOS has in front, and which executable the stand-in's
process runs (game-mode/Game.app/... when the game app took). The stand-in exits by itself.

It takes over the screen for the run (default 25 s): leave the mouse and keyboard alone. Exit
status 0 only when Game Mode came on.
"""
import argparse
import ctypes
import os
from pathlib import Path
import re
import subprocess
import sys
import time

HERE = Path(__file__).resolve()
REPO = HERE.parents[4]
STANDIN = REPO / 'tests/game_mode/standin.c'
INSTALLED = Path.home() / 'Library/Application Support/Steam/compatibilitytools.d/notproton-steamdriver-finals/runtime'
CACHE = Path.home() / 'Library/Caches/SteamDriver'
PREFIX = CACHE / 'game-mode-test'
XCODE = dict(os.environ, DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')


def game_mode():
    out = subprocess.run(['xcrun', 'gamepolicyctl', 'game-mode', 'status'], env=XCODE,
                         capture_output=True, text=True).stdout
    m = re.search(r'Game mode is (\w+)', re.sub(r'\x1b\[[0-9;]*m', '', out))
    return m.group(1) if m else '?'


def frontmost():
    asn = subprocess.run(['lsappinfo', 'front'], capture_output=True, text=True).stdout.strip()
    info = subprocess.run(['lsappinfo', 'info', '-only', 'name', asn], capture_output=True, text=True).stdout
    m = re.search(r'^"([^"]*)" ASN', info, re.M) or re.search(r'"LSDisplayName"="([^"]*)"', info)
    return m.group(1) if m else '?'


def standin_process():
    """(pid, executable) of the stand-in's Wine process, or None."""
    out = subprocess.run(['ps', '-axo', 'pid=,comm=,args='], capture_output=True, text=True).stdout
    for line in out.splitlines():
        pid, _, rest = line.strip().partition(' ')
        if 'GameModeStandIn.exe' in rest and 'game_mode_check' not in rest:
            # The kernel's path of the executable; ps shows the Windows program, as Wine rewrites argv.
            buffer = ctypes.create_string_buffer(4096)
            size = ctypes.CDLL('/usr/lib/libproc.dylib').proc_pidpath(int(pid), buffer, len(buffer))
            return pid, buffer.value.decode() if size > 0 else '?'

    return None


def build_standin():
    out = CACHE / 'GameModeStandIn.exe'
    if out.exists() and out.stat().st_mtime >= STANDIN.stat().st_mtime:
        return out
    CACHE.mkdir(parents=True, exist_ok=True)
    subprocess.run(['x86_64-w64-mingw32-gcc', '-O1', '-municode', '-mwindows', str(STANDIN), '-o', str(out),
                    '-ld3d11', '-ldxgi', '-ldxguid', '-luuid'], check=True)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('runtime', nargs='?', type=Path, default=INSTALLED)
    parser.add_argument('--seconds', type=int, default=25)
    parser.add_argument('--off', action='store_true', help='without WINE_GAME_MODE (the control)')
    parser.add_argument('--no-hook', action='store_true', help='without the Rosetta NT hook')
    args = parser.parse_args()
    engine = args.runtime.expanduser().resolve()
    if not (engine / 'bin/wine').exists():
        sys.exit(f'{engine}: no bin/wine')
    exe = build_standin()

    env = {k: v for k, v in os.environ.items() if not k.startswith(('WINE', 'DYLD_'))}
    env.update(WINEPREFIX=str(PREFIX), WINEARCH='win64', WINEDEBUG='-all',
               WINELOADER=str(engine / 'bin/wine'), WINESERVER=str(engine / 'bin/wineserver'),
               WINEDLLOVERRIDES='d3d11,dxgi,d3d10,d3d12,d3d12core=b;d3d10core,winemetal,nvapi64,nvngx=',
               CX_APPLEGPTK_LIBD3DSHARED_PATH=str(engine / 'lib/external/libd3dshared.dylib'),
               D3DMETAL_FRAMEWORK_PATH=str(engine / 'lib/external/D3DMetal.framework/D3DMetal'),
               CX_ACTIVE_GRAPHICS_BACKEND='d3dmetal', CX_GRAPHICS_BACKEND='d3dmetal', D3DM_ENABLE_METALFX='0',
               WINEESYNC='0', WINEMSYNC='1', ROSETTA_ADVERTISE_AVX='1')
    hook = engine / 'lib/macfinals-nt-hook.dylib'
    if hook.exists() and not args.no_hook:
        env.update(DYLD_INSERT_LIBRARIES=str(hook), DISABLE_AOT='1')
    if not args.off:
        env['WINE_GAME_MODE'] = '*'
    if not PREFIX.exists():
        print('making the test prefix (once)...')
        subprocess.run([str(engine / 'bin/wine'), 'wineboot', '-i'], env=dict(env, WINEDLLOVERRIDES='mscoree,mshtml='),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([str(engine / 'bin/wineserver'), '-w'], env=env)

    print(f'runtime {engine.name}, game apps {"off" if args.off else "on"}; game mode before: {game_mode()}')
    print(f'starting the stand-in for {args.seconds} s; it takes over the screen')
    log = open(CACHE / 'game-mode-test.log', 'w')
    proc = subprocess.Popen([str(engine / 'bin/wine'), str(exe), str(args.seconds)], env=env, stdout=log, stderr=log)
    seen_on, seen_exe = False, None
    start = time.time()
    while time.time() - start < args.seconds + 15:
        time.sleep(1)
        found = standin_process()
        if found:
            seen_exe = found[1]
        state = game_mode()
        seen_on |= state == 'on'
        print(f'  {time.time() - start:4.0f} s  game mode {state:<4} front: {frontmost():<22}'
              f' process: {found[1] if found else "-"}')
        if proc.poll() is not None and not found:
            break
    proc.wait(timeout=30)
    subprocess.run([str(engine / 'bin/wineserver'), '-k'], env=env)
    print(f'\nexit status {proc.returncode}; ran from: {seen_exe or "never seen"}')
    print('GAME MODE CAME ON' if seen_on else 'Game Mode never came on',
          f'(Wine output in {CACHE / "game-mode-test.log"})')
    return 0 if seen_on else 1


if __name__ == '__main__':
    sys.exit(main())
