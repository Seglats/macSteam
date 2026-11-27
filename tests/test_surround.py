"""Headphone surround in the SteamDriver tool's run script (environments/finals/compat-tool/run):
the experimental switch file, the profile keys and the launch option decide WINE_HEADPHONE_SURROUND,
which wine-coreaudio-headphone-surround.patch reads. Runs the script's block alone with zsh in a
temporary folder; nothing of Steam's or the player's is touched."""
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUN_SCRIPT = ROOT / 'environments/finals/compat-tool/run'
PATCH = ROOT / 'environments/finals/patches/wine-coreaudio-headphone-surround.patch'
FITTED = ROOT / 'environments/finals/patches/wine-coreaudio-headphone-surround-fitted.patch'
SETTINGS = ROOT / 'environments/finals/patches/wine-coreaudio-headphone-surround-settings.patch'


class RunScriptSurround(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='steamdriver-surround-')
        self.addCleanup(self.temp.cleanup)
        self.sd = Path(self.temp.name) / 'SteamDriver'
        (self.sd / 'profiles').mkdir(parents=True)

    def profile(self, name, file, text):
        folder = self.sd / 'profiles' / name
        folder.mkdir(parents=True, exist_ok=True)
        (folder / file).write_text(text)

    def run_block(self, env='', app_id='2767030', known='direct3d=dx12'):
        text = RUN_SCRIPT.read_text()
        profiles = re.search(r'^# The profile files read.*?^fi\n', text, re.S | re.M).group(0)
        surround = re.search(r'^surround=\$\{STEAMDRIVER_SURROUND.*?^  unset WINE_HEADPHONE_SURROUND[^\n]*\nfi\n', text, re.S | re.M).group(0)
        script = (f"say(){{ print -r -- \"say: $*\"; }}; steamdriver='{self.sd}' app_id={app_id} known_d3d={known} "
                  f"verb=waitforexitandrun; unset WINE_HEADPHONE_SURROUND; {env}\n{profiles}\n{surround}\n"
                  "print -r -- \"surround=$surround env=${WINE_HEADPHONE_SURROUND:-unset} filters=${WINE_HEADPHONE_SURROUND_FILTERS:-unset}\"")
        return subprocess.run(['/bin/zsh', '-f', '-c', script], capture_output=True, text=True, check=True).stdout

    def test_a_profile_setting_needs_the_experimental_switch(self):
        self.profile('global-dx12', 'd3dmetal.conf', 'upscaling = False\nsurround = True\n')
        self.assertIn('surround=0 env=unset', self.run_block())
        (self.sd / 'headphone-surround').write_text('on\n')
        out = self.run_block()
        self.assertIn('surround=1 env=1', out)
        self.assertIn('from the profile global-dx12', out)

    def test_the_game_profile_wins_over_global(self):
        (self.sd / 'headphone-surround').write_text('on\n')
        self.profile('global-dx12', 'd3dmetal.conf', 'surround = True\n')
        self.profile('app-2767030', 'd3dmetal.conf', 'surround = False\n')
        self.assertIn('surround=0 env=unset', self.run_block())
        # A DX11 game profile: the tool's key in dxmt.conf.
        self.profile('app-730', 'dxmt.conf', 'dxgi.maxFrameLatency = 1\nsteamdriver.surround = True\n')
        self.assertIn('surround=1 env=1', self.run_block(app_id='730'))
        # A game detected as DX11 without a game profile reads Global DX11.
        self.profile('global-dx11', 'dxmt.conf', 'steamdriver.surround = true\n')
        self.assertIn('surround=1 env=1', self.run_block(app_id='504230', known='direct3d=dx11'))

    def test_saved_settings_reach_the_game_only_with_surround_on(self):
        home = Path(self.temp.name) / 'home'
        saved = home / 'Library/Application Support/SteamDriver/surround-settings.sdsurr'
        self.assertIn('filters=unset', self.run_block(f"HOME='{home}' STEAMDRIVER_SURROUND=1"))   # nothing saved: fitted
        saved.parent.mkdir(parents=True)
        saved.write_bytes(b'SDSURR1\0')
        on = self.run_block(f"HOME='{home}' STEAMDRIVER_SURROUND=1")
        self.assertIn(f'filters={saved}', on)
        self.assertIn('your settings from macSteam', on)
        self.assertIn('filters=unset', self.run_block(f"HOME='{home}' STEAMDRIVER_SURROUND=0"))

    def test_the_launch_option_wins_both_ways(self):
        self.profile('global-dx12', 'd3dmetal.conf', 'surround = True\n')
        (self.sd / 'headphone-surround').write_text('on\n')
        self.assertIn('surround=0 env=unset', self.run_block('STEAMDRIVER_SURROUND=0'))
        (self.sd / 'headphone-surround').unlink()
        self.assertIn('surround=1 env=1', self.run_block('STEAMDRIVER_SURROUND=1'))
        # Upscaling's launch option leaves the profile list for surround in place.
        (self.sd / 'headphone-surround').write_text('on\n')
        self.assertIn('surround=1 env=1', self.run_block('STEAMDRIVER_UPSCALE=1'))

    def test_the_patches_are_listed_last_and_read_the_same_variable(self):
        build = (ROOT / 'environments/finals/scripts/build_portable_runtime.py').read_text()
        patches = re.findall(r"^    '([^']+\.patch)',", build, re.M)
        self.assertEqual(patches[-3:], [PATCH.name, FITTED.name, SETTINGS.name])
        # the settings patch: the player's baked file, from macSteam's Settings…, read by the driver
        self.assertIn('getenv("WINE_HEADPHONE_SURROUND_FILTERS")', SETTINGS.read_text())
        run = (ROOT / 'environments/finals/compat-tool/run').read_text()
        self.assertIn('WINE_HEADPHONE_SURROUND_FILTERS=$surround_settings', run)
        self.assertIn('SteamDriver/surround-settings.sdsurr', run)
        self.assertIn('getenv("WINE_HEADPHONE_SURROUND")', PATCH.read_text())
        # the fitted patch brings its filters, generated by scripts/surround_fit/render2 bake
        self.assertIn('+++ b/dlls/winecoreaudio.drv/surround_ir.h', FITTED.read_text())
        self.assertIn('+#include "surround_ir.h"', FITTED.read_text())


@unittest.skipUnless(shutil.which('ffmpeg'), 'needs ffmpeg (brew install ffmpeg)')
class OfflineRender(unittest.TestCase):
    """scripts/surround_render.py: the patch's own model, compiled for the Mac, on files."""
    def test_the_hits_are_placed_left_and_right(self):
        with tempfile.TemporaryDirectory(prefix='surround-render-test-') as folder:
            subprocess.run(['python3', ROOT / 'scripts/surround_render.py', 'test', folder], check=True, capture_output=True)
            self.assertEqual(sorted(p.name for p in Path(folder).iterdir()),
                             ['test-7.1.wav', 'test-headphones.wav', 'test-hits.txt', 'test-plain.wav'])
            hits = [line.split() for line in (Path(folder) / 'test-hits.txt').read_text().splitlines()]
            for name in ('test-headphones.wav',):
                data = (Path(folder) / name).read_bytes()
                at = data.index(b'data') + 8      # 24-bit stereo
                def levels(t):
                    base = at + int(t * 48000) * 6
                    values = [int.from_bytes(data[base + i * 3:base + i * 3 + 3], 'little', signed=True) for i in range(2 * 9600)]
                    return sum(v * v for v in values[0::2]), sum(v * v for v in values[1::2])
                for t, az, pattern, *_ in hits:
                    if pattern == '5':      # right and left: the near ear clearly louder
                        left, right = levels(float(t))
                        if az == '90.0':
                            self.assertGreater(right, left * 2, (name, t))
                        else:
                            self.assertGreater(left, right * 2, (name, t))

RUNTIME = Path.home() / 'SteamDriver-build/finals/phase-2/artifacts/macfinals-212'
GPTK = Path.home() / 'Library/Application Support/SteamDriver/GPTK/redist/lib'
MINGW = ROOT / 'runtime/toolchains/llvm-mingw-20261006-ucrt-macos-universal/bin/x86_64-w64-mingw32-clang'


@unittest.skipUnless(RUNTIME.is_dir() and MINGW.exists() and GPTK.is_dir(), 'needs the macfinals-212 build and the toolkit')
class SurroundTestPlayer(unittest.TestCase):
    """scripts/surround_test.c in the runtime, 60 dB down (-quiet), on the Mac's default output."""
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='steamdriver-surround-test-')
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        cls.exe = root / 'surround_test.exe'
        subprocess.run([MINGW, '-O2', '-I', ROOT / 'scripts', ROOT / 'scripts/surround_test.c', '-o', cls.exe, '-lole32', '-lksuser'], check=True)
        cls.engine = root / 'runtime'
        subprocess.run(['/bin/cp', '-c', '-R', RUNTIME, cls.engine], check=True)
        (cls.engine / 'lib/apple-gptk').unlink(missing_ok=True)
        (cls.engine / 'lib/apple-gptk').symlink_to(GPTK)
        cls.prefix = root / 'prefix'
        cls.addClassCleanup(lambda: subprocess.run([cls.engine / 'bin/wineserver', '-k'], env={**os.environ, 'WINEPREFIX': str(cls.prefix)}))
        # 5.1 at 44.1 kHz, 16-bit: mapped to 7.1 and resampled to the device rate
        rate, ch = 44100, 6
        frames = b''.join(struct.pack('<6h', *(int(30 * math.sin(2 * math.pi * 440 * i / rate)) if c == 4 else 0 for c in range(ch)))
                          for i in range(rate // 2))
        fmt = struct.pack('<HHIIHH', 1, ch, rate, rate * ch * 2, ch * 2, 16)
        cls.wav = root / 'five-one.wav'
        cls.wav.write_bytes(b'RIFF' + struct.pack('<I', 4 + 8 + len(fmt) + 8 + len(frames)) + b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt))
                            + fmt + b'data' + struct.pack('<I', len(frames)) + frames)

    def play(self, *args, surround=False):
        env = {**os.environ, 'WINEPREFIX': str(self.prefix), 'WINEDEBUG': '-all', 'WINEDLLOVERRIDES': 'mscoree,mshtml='}
        env.pop('WINE_HEADPHONE_SURROUND', None)
        if surround:
            env['WINE_HEADPHONE_SURROUND'] = '1'
        out = subprocess.run([self.engine / 'bin/wine', self.exe, '-quiet', *args], env=env, capture_output=True, text=True, timeout=240)
        return [line.strip() for line in out.stdout.splitlines() if line.strip()]

    def test_the_bounce_is_heard_at_every_spot_with_and_without_surround(self):
        clip = ROOT / 'scripts/surround/ball-bounce.wav'
        off = self.play('-test', str(clip))
        on = self.play('-test', str(clip), surround=True)
        hits = [l for l in on if l.startswith('hit ')]
        self.assertEqual(len(hits), 88)
        self.assertEqual([l for l in off if l.startswith('hit ')], hits)
        self.assertEqual(hits[0], 'hit 0.0 1')
        self.assertIn('hit 180.0 4', hits)           # front and back
        self.assertEqual(hits[-1], 'hit 270.0 5')
        self.assertEqual(on[-1], 'done')
        # On a stereo output, the game side sees 7.1 only with it on.
        if off[0].startswith('mix 2 '):
            self.assertTrue(on[0].startswith('mix 8 0x63f '), on[0])

    def test_it_starts_where_asked_and_reports_its_place(self):
        clip = ROOT / 'scripts/surround/ball-bounce.wav'
        out = self.play('-start', '30', '-test', str(clip), surround=True)
        self.assertIn('length 36.8', out)
        times = [float(l.split()[1]) for l in out if l.startswith('time ')]
        self.assertGreaterEqual(times[0], 29.9)          # -start: no hit of patterns 1-4 is heard
        self.assertEqual({l.split()[2] for l in out if l.startswith('hit ')}, {'4', '5'})
        self.assertEqual(out[-1], 'done')

    def test_two_players_share_a_timeline_and_take_commands(self):
        """macSteam's On and Off players: both wait for play, follow one timeline and end together."""
        clip = ROOT / 'scripts/surround/ball-bounce.wav'
        env = {**os.environ, 'WINEPREFIX': str(self.prefix), 'WINEDEBUG': '-all', 'WINEDLLOVERRIDES': 'mscoree,mshtml='}
        players = []
        for voice in ('on', 'off'):
            e = dict(env)
            e.pop('WINE_HEADPHONE_SURROUND', None)
            if voice == 'on':
                e['WINE_HEADPHONE_SURROUND'] = '1'
            players.append(subprocess.Popen([self.engine / 'bin/wine', self.exe, '-quiet', '-sync', f'surround-unittest-{os.getpid()}',
                                             '-voice', voice, '-start', '30', '-test', str(clip)],
                                            env=e, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1))
        lead = players[0]
        lines = {id(p): [] for p in players}
        def read(p):
            for line in p.stdout:
                lines[id(p)].append(line.strip())
        for p in players:
            threading.Thread(target=read, args=(p,), daemon=True).start()
        def wait(cond, limit=120):
            end = time.time() + limit
            while not cond() and time.time() < end:
                time.sleep(0.05)
            self.assertTrue(cond())
        wait(lambda: all('ready' in lines[id(p)] for p in players))
        out = lines[id(lead)]
        self.assertFalse([l for l in out if l.startswith('hit ')])   # waits for play
        def say(c):
            lead.stdin.write(c + '\n'); lead.stdin.flush()
        say('play'); time.sleep(1.5); say('surround 0'); time.sleep(0.5); say('pause'); time.sleep(0.5)
        paused_at = len(out)
        time.sleep(1)
        self.assertFalse([l for l in out[paused_at:] if l.startswith('hit ')])   # paused: nothing heard
        say('seek 35'); say('play')
        for p in players:
            p.wait(timeout=60)
        self.assertEqual(out[-1], 'done')
        self.assertEqual(lines[id(players[1])][-1], 'done')
        # On plays 7.1 for Wine to render; Off a stereo mix, as a game does for stereo output (handed
        # 7.1, macOS dropped centre, sides and backs).
        self.assertIn('stream 8', out)
        self.assertIn('stream 2', lines[id(players[1])])
        self.assertGreaterEqual(float([l for l in out if l.startswith('time ')][-1].split()[1]), 36)
        self.assertEqual({l.split()[2] for l in out if l.startswith('hit ')}, {'4', '5'})

    def test_stereo_mixes_every_speaker_down_and_still_hears_every_hit(self):
        clip = ROOT / 'scripts/surround/ball-bounce.wav'
        out = self.play('-stereo', '-test', str(clip))
        self.assertIn('stream 2', out)
        self.assertEqual(len([l for l in out if l.startswith('hit ')]), 88)
        self.assertEqual(out[-1], 'done')

    def test_wine_loads_settings_baked_by_macsteam_and_falls_back_from_a_broken_file(self):
        """Headphone surround › Settings…'s Save: the driver reads WINE_HEADPHONE_SURROUND_FILTERS (206 on)."""
        root = Path(self.temp.name)
        preview = root / 'surround_preview'
        if not preview.exists():
            subprocess.run(['/usr/bin/clang', '-O2', '-I', ROOT / 'scripts', ROOT / 'scripts/surround_preview.c', '-framework', 'AudioToolbox',
                            '-o', preview], check=True, capture_output=True)
        baked = root / 'mine.sdsurr'
        subprocess.run([preview, ROOT / 'scripts/surround/kemar.head', ROOT / 'scripts/surround/ball-bounce.wav', 'front=60', f'--bake={baked}'],
                       check=True, capture_output=True)
        broken = root / 'broken.sdsurr'
        broken.write_bytes(baked.read_bytes()[:1000])
        clip = ROOT / 'scripts/surround/ball-bounce.wav'
        for path, expect in ((baked, 'settings from'), (broken, "isn't a settings file")):
            env = {**os.environ, 'WINEPREFIX': str(self.prefix), 'WINEDEBUG': '-all,warn+coreaudio,trace+coreaudio',
                   'WINEDLLOVERRIDES': 'mscoree,mshtml=', 'WINE_HEADPHONE_SURROUND': '1', 'WINE_HEADPHONE_SURROUND_FILTERS': str(path)}
            out = subprocess.run([self.engine / 'bin/wine', self.exe, '-quiet', '-start', '36', '-test', str(clip)],
                                 env=env, capture_output=True, text=True, timeout=240)
            self.assertIn(expect, out.stderr, path.name)
            self.assertEqual(out.stdout.split()[-1], 'done', path.name)

    def test_a_wav_file_plays_and_a_bad_one_says_why(self):
        out = self.play(str(self.wav), surround=True)
        self.assertIn('file 6 44100 0.5', out)
        self.assertEqual(out[-1], 'done')
        bad = self.play(str(ROOT / 'README.md'))
        self.assertTrue(bad[-1].startswith('error not a WAV file'), bad)


APP_HARNESS = r'''
import Foundation
import SwiftUI
enum LauncherStyle { static let accent=Color.accentColor }
// macSteam's surround test (SurroundTest.swift) as the window drives it, step by step, printing
// what the window would show; the steps come from the command line.
@main struct H { @MainActor static func main() {
    let a=CommandLine.arguments
    NotProton.home=URL(fileURLWithPath:a[1])
    let exe=URL(fileURLWithPath:a[2]),test=SurroundTest()
    test.volume=0.001
    var steps=Array(a[3...]),waited=0.0
    func show(_ what:String) { print("\(what) \(test.phase) \(String(format:"%.2f",test.time)) \(String(format:"%.1f",test.length)) \(test.surround)") }
    Timer.scheduledTimer(withTimeInterval:0.1,repeats:true) { _ in MainActor.assumeIsolated {
        waited+=0.1
        guard let step=steps.first else { test.stopForQuit();exit(0) }
        let parts=step.split(separator:":").map(String.init)
        switch parts[0] {
        case "play": test.playPause(exe)
        case "pause": test.playPause(exe)
        case "off": test.surround=false
        case "on": test.surround=true
        case "seek": test.seek(to:Double(parts[1])!)
        case "replay": test.replay(exe)
        case "until":          // until:playing / until:idle; after 30 s the test goes on and fails
            if "\(test.phase)" != parts[1] && waited<30 { return }
        case "wait": if waited<Double(parts[1])! { return }
        case "show": show(parts[1])
        default: break
        }
        steps.removeFirst();waited=0
    } }
    RunLoop.main.run()
}}
'''


@unittest.skipUnless(RUNTIME.is_dir() and GPTK.is_dir(), 'needs the macfinals-212 build and the toolkit')
class SurroundTestInTheApp(unittest.TestCase):
    """macSteam's own control of the two players (SurroundTest.swift) with real Wine, in a throwaway
    home: what the window shows after Play, Pause, the On/Off swap, a jump and the end. The players'
    lines end in \\r\\n (Wine's C runtime); reading them as Swift Strings never saw a line, so the
    test stayed at Starting… (dev76 to dev82)."""
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='steamdriver-surround-app-')
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        cls.home = root / 'home'
        tool = cls.home / 'Library/Application Support/Steam/compatibilitytools.d/notproton-steamdriver'
        tool.mkdir(parents=True)
        subprocess.run(['/bin/cp', '-c', '-R', RUNTIME, tool / 'runtime'], check=True)
        (tool / 'runtime/lib/apple-gptk').unlink(missing_ok=True)
        (tool / 'runtime/lib/apple-gptk').symlink_to(GPTK)
        cls.addClassCleanup(lambda: subprocess.run([tool / 'runtime/bin/wineserver', '-k'],
                            env={**os.environ, 'WINEPREFIX': str(cls.home / 'Library/Application Support/SteamDriver/surround-test/prefix')}))
        cls.exe_dir = root / 'SurroundTest'
        cls.exe_dir.mkdir()
        subprocess.run([MINGW, '-O2', '-I', ROOT / 'scripts', ROOT / 'scripts/surround_test.c', '-o', cls.exe_dir / 'surround_test.exe', '-lole32', '-lksuser'], check=True)
        shutil.copy(ROOT / 'scripts/surround/ball-bounce.wav', cls.exe_dir / 'ball-bounce.wav')
        (root / 'harness.swift').write_text(APP_HARNESS)
        cls.harness = root / 'harness'
        sources = ['SurroundTest', 'NotProton', 'NotProtonExtras', 'Profiles', 'KeyOverrides', 'Migration', 'Brand', 'InstallLocation', 'Surround', 'GameTools', 'ControllerSupport']
        subprocess.run(['xcrun', 'swiftc', '-parse-as-library', *[str(ROOT / f'app/Sources/{s}.swift') for s in sources], str(root / 'harness.swift'),
                        '-o', str(cls.harness)], check=True, capture_output=True)

    def drive(self, *steps):
        out = subprocess.run([str(self.harness), str(self.home), str(self.exe_dir / 'surround_test.exe'), *steps],
                             capture_output=True, text=True, timeout=180).stdout
        return {parts[0]: (parts[1], float(parts[2]), float(parts[3]), parts[4] == 'true')
                for parts in (line.split() for line in out.splitlines() if line.strip())}

    def test_play_pause_swap_jump_and_the_end_as_the_window_shows_them(self):
        seen = self.drive('play', 'until:playing', 'wait:1.5', 'show:playing',
                          'pause', 'wait:1', 'show:paused', 'wait:1.5', 'show:still',
                          'play', 'wait:1', 'off', 'wait:1', 'show:off',
                          'seek:33', 'wait:1.5', 'show:jumped', 'until:idle', 'show:end')
        phase, t, length, _ = seen['playing']
        self.assertEqual((phase, length), ('playing', 36.8))
        self.assertGreater(t, 0.8)
        self.assertEqual(seen['paused'][0], 'paused')
        self.assertAlmostEqual(seen['still'][1], seen['paused'][1], delta=0.3)     # paused: the place holds
        phase, t, _, surround = seen['off']
        self.assertEqual((phase, surround), ('playing', False))                   # the swap kept it playing
        self.assertGreater(t, seen['still'][1])                                    # on from where it was
        self.assertGreaterEqual(seen['jumped'][1], 33)
        self.assertEqual(seen['end'][0], 'idle')
        self.assertEqual(seen['end'][1], 36.8)                                     # the bar stays full


if __name__ == '__main__':
    unittest.main()
